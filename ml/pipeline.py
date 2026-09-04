#!/usr/bin/env python3
"""Deterministic load forecasting pipeline for UC-M-01 through UC-M-04."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

RANDOM_SEED = 20260901
FEATURE_SCHEMA_VERSION = 1
HOUR = 3600


def _dependencies() -> tuple[Any, ...]:
    try:
        import joblib
        import numpy as np
        from sklearn.compose import ColumnTransformer
        from sklearn.ensemble import RandomForestRegressor
        from sklearn.impute import SimpleImputer
        from sklearn.metrics import mean_absolute_error, mean_squared_error
        from sklearn.pipeline import Pipeline
        from sklearn.preprocessing import OneHotEncoder
    except ImportError as error:
        raise RuntimeError("scikit-learn, numpy and joblib are required") from error
    return (joblib, np, ColumnTransformer, RandomForestRegressor,
            SimpleImputer, Pipeline, OneHotEncoder,
            mean_absolute_error, mean_squared_error)


def load_holidays(path: Path) -> set[str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 1 or not isinstance(document.get("dates"), list):
        raise ValueError("unsupported holiday calendar")
    return {str(value) for value in document["dates"]}


def simulated_weather(station_id: int, bucket_at: int) -> tuple[float, str]:
    # One independent deterministic stream per station/hour. Pagination order
    # therefore cannot change the generated feature values.
    generator = random.Random(RANDOM_SEED ^ station_id ^ (bucket_at // HOUR))
    instant = datetime.fromtimestamp(bucket_at, tz=timezone.utc)
    seasonal = 14.0 * math.sin((instant.timetuple().tm_yday - 80) * 2 * math.pi / 365)
    temperature = 13.0 + seasonal + generator.uniform(-4.0, 4.0)
    precipitation = "RAIN" if generator.random() < 0.18 else "NONE"
    return round(temperature, 3), precipitation


def build_samples(rows: list[dict[str, Any]], holidays: set[str]) -> list[dict[str, Any]]:
    by_station: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        by_station.setdefault(int(row["stationId"]), []).append(row)
    samples: list[dict[str, Any]] = []
    for station_id, station_rows in by_station.items():
        station_rows.sort(key=lambda item: int(item["bucketAt"]))
        energies = [float(item["energyMwh"]) for item in station_rows]
        for index in range(168, len(station_rows)):
            row = station_rows[index]
            bucket_at = int(row["bucketAt"])
            instant = datetime.fromtimestamp(bucket_at, tz=timezone.utc)
            temperature, precipitation = simulated_weather(station_id, bucket_at)
            samples.append({
                "stationId": str(station_id),
                "bucketAt": bucket_at,
                "hour": instant.hour,
                "weekday": instant.weekday(),
                "isWeekend": int(instant.weekday() >= 5),
                "isHoliday": int(instant.date().isoformat() in holidays),
                "lag1": energies[index - 1],
                "lag24": energies[index - 24],
                "lag168": energies[index - 168],
                "rolling24": sum(energies[index - 24:index]) / 24.0,
                "temperature": temperature,
                "precipitation": precipitation or "UNKNOWN",
                "energyMwh": energies[index],
                "operationalChargerCount": int(row["operationalChargerCount"]),
                "busyDeviceSeconds": int(row.get("busyDeviceSeconds", 0)),
            })
    return sorted(samples, key=lambda item: (item["bucketAt"], item["stationId"]))


NUMERIC_FEATURES = [
    "hour", "weekday", "isWeekend", "isHoliday", "lag1", "lag24",
    "lag168", "rolling24", "temperature",
]
CATEGORICAL_FEATURES = ["stationId", "precipitation"]
FEATURES = NUMERIC_FEATURES + CATEGORICAL_FEATURES


def _matrix(samples: Iterable[dict[str, Any]]) -> list[list[Any]]:
    return [[sample.get(feature) for feature in FEATURES] for sample in samples]


def metrics(actual: Any, predicted: Any, np: Any,
            mean_absolute_error: Any, mean_squared_error: Any) -> dict[str, float | int]:
    actual_values = np.asarray(actual, dtype=float)
    predicted_values = np.maximum(0.0, np.asarray(predicted, dtype=float))
    nonzero = actual_values > 0
    excluded = int((~nonzero).sum())
    mape = float(np.mean(np.abs((actual_values[nonzero] - predicted_values[nonzero]) /
                                actual_values[nonzero])) * 100) if nonzero.any() else 0.0
    denominator = float(np.abs(actual_values).sum())
    wape = float(np.abs(actual_values - predicted_values).sum() / denominator * 100) \
        if denominator > 0 else 0.0
    return {
        "mae": float(mean_absolute_error(actual_values, predicted_values)),
        "rmse": float(math.sqrt(mean_squared_error(actual_values, predicted_values))),
        "mape": mape,
        "wape": wape,
        "excludedSampleCount": excluded,
    }


def train(rows: list[dict[str, Any]], model_path: Path, holiday_path: Path) -> dict[str, Any]:
    (joblib, np, ColumnTransformer, RandomForestRegressor, SimpleImputer,
     Pipeline, OneHotEncoder, mean_absolute_error, mean_squared_error) = _dependencies()
    samples = build_samples(rows, load_holidays(holiday_path))
    if len(samples) < 200:
        raise ValueError("at least 200 lag-complete hourly samples are required")
    split = max(1, int(len(samples) * 0.8))
    if split >= len(samples):
        raise ValueError("fixed test set is empty")
    train_samples, test_samples = samples[:split], samples[split:]
    transformer = ColumnTransformer([
        ("numeric", SimpleImputer(strategy="median"),
         list(range(len(NUMERIC_FEATURES)))),
        ("category", Pipeline([
            ("imputer", SimpleImputer(strategy="constant", fill_value="UNKNOWN")),
            ("onehot", OneHotEncoder(handle_unknown="ignore")),
        ]), list(range(len(NUMERIC_FEATURES), len(FEATURES)))),
    ])
    model = Pipeline([
        ("features", transformer),
        ("model", RandomForestRegressor(
            n_estimators=200, random_state=RANDOM_SEED, n_jobs=1,
            min_samples_leaf=2)),
    ])
    model.fit(_matrix(train_samples), [sample["energyMwh"] for sample in train_samples])
    actual = np.asarray([sample["energyMwh"] for sample in test_samples], dtype=float)
    predicted = model.predict(_matrix(test_samples))
    baseline = np.asarray([sample["lag168"] for sample in test_samples], dtype=float)
    model_metrics = metrics(actual, predicted, np, mean_absolute_error, mean_squared_error)
    baseline_metrics = metrics(actual, baseline, np, mean_absolute_error, mean_squared_error)
    model_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=model_path.parent, delete=False) as output:
        temporary = Path(output.name)
    try:
        joblib.dump({"schemaVersion": FEATURE_SCHEMA_VERSION, "model": model}, temporary)
        os.replace(temporary, model_path)
    finally:
        temporary.unlink(missing_ok=True)
    checksum = hashlib.sha256(model_path.read_bytes()).hexdigest()
    return {
        **model_metrics,
        "baselineMae": baseline_metrics["mae"],
        "baselineRmse": baseline_metrics["rmse"],
        "qualified": model_metrics["mae"] < baseline_metrics["mae"] and
                     model_metrics["rmse"] < baseline_metrics["rmse"],
        "artifactChecksum": checksum,
        "trainFromAt": int(samples[0]["bucketAt"]),
        "trainToAt": int(samples[-1]["bucketAt"]),
    }


def predict(rows: list[dict[str, Any]], model_path: Path, holiday_path: Path,
            horizons: list[int], model_version_hint: str = "") -> tuple[str, list[dict[str, Any]]]:
    holidays = load_holidays(holiday_path)
    by_station: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        by_station.setdefault(int(row["stationId"]), []).append(row)
    model_version = "BASELINE"
    model = None
    metadata_path = model_path.with_suffix(model_path.suffix + ".json")
    use_server_model = bool(model_version_hint and model_version_hint != "BASELINE")
    if model_path.is_file() and (use_server_model or
                                 (not model_version_hint and metadata_path.is_file())):
        joblib, *_ = _dependencies()
        artifact = joblib.load(model_path)
        if artifact.get("schemaVersion") == FEATURE_SCHEMA_VERSION:
            model = artifact["model"]
            model_version = model_version_hint or json.loads(
                metadata_path.read_text(encoding="utf-8"))["modelVersionNo"]
    results: list[dict[str, Any]] = []
    for station_id, history in by_station.items():
        history.sort(key=lambda item: int(item["bucketAt"]))
        if len(history) < 168:
            continue
        generated_at = int(history[-1]["bucketAt"])
        last = history[-1]
        recent = history[-30 * 24:]
        total_busy_hours = sum(int(item.get("busyDeviceSeconds", 0))
                               for item in recent) / 3600.0
        total_energy = sum(float(item["energyMwh"]) for item in recent)
        energy_per_busy_hour = (total_energy / total_busy_hours
                                if total_busy_hours > 0 else 0.0)
        busy_hour_denominator = energy_per_busy_hour if energy_per_busy_hour > 0 else 1.0
        actual_energies = [float(item["energyMwh"]) for item in history]
        forecast_series = list(actual_energies)
        percentile_values = sorted(actual_energies)
        percentile90 = percentile_values[int((len(percentile_values) - 1) * 0.9)]
        forecasts: dict[int, float] = {}
        for step in range(1, max(horizons) + 1):
            target_at = generated_at + step * HOUR
            instant = datetime.fromtimestamp(target_at, tz=timezone.utc)
            temperature, precipitation = simulated_weather(station_id, target_at)
            feature = {
                "stationId": str(station_id), "hour": instant.hour,
                "weekday": instant.weekday(), "isWeekend": int(instant.weekday() >= 5),
                "isHoliday": int(instant.date().isoformat() in holidays),
                "lag1": forecast_series[-1], "lag24": forecast_series[-24],
                "lag168": forecast_series[-168],
                "rolling24": sum(forecast_series[-24:]) / 24.0,
                "temperature": temperature, "precipitation": precipitation,
            }
            prediction = (float(model.predict(_matrix([feature]))[0])
                          if model else float(feature["lag168"]))
            prediction = max(0.0, prediction)
            forecast_series.append(prediction)
            forecasts[step] = prediction
        for horizon in horizons:
            prediction = forecasts[horizon]
            target_at = generated_at + horizon * HOUR
            operational = max(0, int(last["operationalChargerCount"]))
            busy = min(operational,
                       int(math.ceil(prediction / busy_hour_denominator)))
            results.append({
                "stationId": station_id, "generatedAt": generated_at,
                "targetAt": target_at, "horizonHour": horizon,
                "predictedEnergyMwh": int(round(prediction)),
                "predictedFreeCount": operational - busy,
                "isPeak": (operational > 0 and busy / operational >= 0.8) or prediction >= percentile90,
            })
    return model_version, results


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--train", action="store_true")
    mode.add_argument("--predict", action="store_true")
    mode.add_argument("--evaluate", action="store_true")
    parser.add_argument("--features", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--holidays", type=Path,
                        default=Path(__file__).parent / "data" / "holidays-cn-2026.json")
    parser.add_argument("--horizons", default="1,6,24")
    arguments = parser.parse_args()
    rows = json.loads(arguments.features.read_text(encoding="utf-8"))
    if arguments.train or arguments.evaluate:
        print(json.dumps(train(rows, arguments.model, arguments.holidays), ensure_ascii=False))
    else:
        model_version, values = predict(
            rows, arguments.model, arguments.holidays,
            [int(value) for value in arguments.horizons.split(",")])
        print(json.dumps({"modelVersionNo": model_version, "items": values}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
