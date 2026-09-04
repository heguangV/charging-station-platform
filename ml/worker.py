#!/usr/bin/env python3
"""HTTP worker launched by Crow; the bearer token is accepted only on stdin."""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

from pipeline import FEATURE_SCHEMA_VERSION, RANDOM_SEED, predict, train


class Client:
    def __init__(self, base_url: str, token: str, ca_file: str) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        endpoint = urllib.parse.urlsplit(self.base_url)
        if endpoint.scheme == "https":
            self.context = ssl.create_default_context(cafile=ca_file)
        elif endpoint.scheme == "http" and endpoint.hostname in ("127.0.0.1", "::1"):
            self.context = None
        else:
            raise ValueError("ML HTTP transport is restricted to numeric loopback")

    def request(self, method: str, path: str, body: dict | None = None,
                idempotency_key: str | None = None) -> dict:
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"Authorization": f"Bearer {self.token}"}
        if payload is not None:
            headers["Content-Type"] = "application/json; charset=utf-8"
        if idempotency_key:
            headers["Idempotency-Key"] = idempotency_key
        request = urllib.request.Request(self.base_url + path, data=payload,
                                         headers=headers, method=method)
        with urllib.request.urlopen(request, context=self.context, timeout=35) as response:
            document = json.loads(response.read())
        if not document.get("success"):
            raise RuntimeError("internal API rejected the ML operation")
        return document["data"]


def fetch_features(client: Client, task_no: str) -> list[dict]:
    now = int(time.time())
    parameters = {
        "taskNo": task_no,
        "fromAt": now - 90 * 24 * 3600 + 1,
        "toAt": now,
        "limit": 5000,
    }
    values: list[dict] = []
    while True:
        path = "/api/v1/internal/ml/features/hourly?" + urllib.parse.urlencode(parameters)
        page = client.request("GET", path)
        values.extend(page["items"])
        if not page.get("nextCursor"):
            return values
        parameters["cursor"] = page["nextCursor"]


def complete(client: Client, task_no: str, succeeded: bool, **values: str) -> None:
    client.request("POST", f"/api/v1/internal/ml/tasks/{task_no}/completion",
                   {"status": "SUCCEEDED" if succeeded else "FAILED", **values})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("train", "predict"), required=True)
    parser.add_argument("--task-no", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--ca-file", required=True)
    parser.add_argument("--model-path", required=True, type=Path)
    parser.add_argument("--model-version", default="BASELINE")
    parser.add_argument("--horizons", default="1,6,24")
    parser.add_argument("--token-stdin", action="store_true", required=True)
    arguments = parser.parse_args()
    token = sys.stdin.readline().strip()
    if len(token) < 43:
        return 2
    client = Client(arguments.base_url, token, arguments.ca_file)
    try:
        rows = fetch_features(client, arguments.task_no)
        holiday_path = Path(__file__).parent / "data" / "holidays-cn-2026.json"
        if arguments.mode == "train":
            result = train(rows, arguments.model_path, holiday_path)
            registration = client.request("POST", "/api/v1/internal/ml/model-versions", {
                "taskNo": arguments.task_no,
                "algorithm": "RandomForestRegressor",
                "featureSchemaVersion": FEATURE_SCHEMA_VERSION,
                "randomSeed": RANDOM_SEED,
                "trainFromAt": result["trainFromAt"],
                "trainToAt": result["trainToAt"],
                "mae": result["mae"], "rmse": result["rmse"],
                "mape": result["mape"], "wape": result["wape"],
                "baselineMae": result["baselineMae"],
                "baselineRmse": result["baselineRmse"],
                "excludedSampleCount": result["excludedSampleCount"],
                "artifactChecksum": result["artifactChecksum"],
            })
            summary = json.dumps({key: result[key] for key in
                                  ("mae", "rmse", "mape", "wape", "qualified")})
            complete(client, arguments.task_no, True,
                     modelVersionNo=registration["modelVersionNo"],
                     metricsSummary=summary, errorSummary="")
        else:
            horizons = [int(value) for value in arguments.horizons.split(",") if value]
            model_version, predictions = predict(rows, arguments.model_path,
                                                 holiday_path, horizons,
                                                 arguments.model_version)
            client.request("POST", "/api/v1/internal/ml/predictions/batch", {
                "taskNo": arguments.task_no,
                "modelVersionNo": model_version,
                "items": predictions,
            }, idempotency_key=str(uuid.uuid5(uuid.NAMESPACE_URL,
                                              "ncs:" + arguments.task_no)))
            complete(client, arguments.task_no, True,
                     modelVersionNo=model_version,
                     metricsSummary="", errorSummary="")
        return 0
    except Exception:
        try:
            complete(client, arguments.task_no, False, modelVersionNo="",
                     metricsSummary="", errorSummary="ML 任务执行失败")
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
