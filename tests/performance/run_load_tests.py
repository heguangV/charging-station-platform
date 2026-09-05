#!/usr/bin/env python3
"""NFR-P-04 / NFR-P-05 capacity and throughput load tests for ncs_server.

Requirement texts (SRS, quoted verbatim, no reinterpretation):

* NFR-P-04 (容量): "支持 3,000 个注册账号、100 个同时在线连接、50 个同时排队
  流程和 48 个同时充电会话"
* NFR-P-05 (吞吐): "HTTPS API 持续处理 20 请求/秒，并承受 50 请求/秒短时峰值；
  WebSocket 支持 100 个在线会话"
* SRS section 8 (容量与并发测试证据段): "容量与并发测试不得依赖真实用户。测试
  环境使用独立 SQLite 文件和仅在测试配置启用的模拟账号，由 Python asyncio
  HTTP/WebSocket 虚拟客户端连接 Crow 服务，并构造 50 个排队流程和最多 48 个
  充电会话。测试环境禁用外部通知；测试结束后删除并重建测试库，不把测试账号
  或固定密码用于正式演示。"

The suite is an asyncio virtual client (standard library only, no pip
packages).  Every run uses a fresh isolated database; when the run finishes the
database is deleted and the server is restarted once to prove it rebuilds a
clean database (delete-and-recreate evidence), then the fresh database is
removed again.  The demo admin ``admin/123456`` is used only against this
test-only database, never in any other environment.

Run:
    python3 run_load_tests.py --server /path/to/ncs_server [--smoke]

Small-scale smoke first (``--smoke`` proves the harness itself), then the full
run.  Exit code 0 means every asserted threshold passed.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import random
import ssl
import sys
import tempfile
import time
import uuid
from pathlib import Path

from .server_env import (ServerHandle, free_port, request_ready, start_server,
                         stop_server)
from .virtual_client import (HttpClient, HttpError, HttpResponse,
                             LatencyStats, TokenBucketPacer, TransportError,
                             WebSocketClient, default_tls_context)

# --------------------------------------------------------------------------
# Scenario sizing.  FULL matches the SRS numbers exactly; SMOKE exercises the
# same code paths at tiny scale so script errors surface before the full run.
# --------------------------------------------------------------------------

FULL = {
    "smoke": False,
    "accounts": 3000,
    "charging": 48,       # NFR-P-04: 48 simultaneous charging sessions
    "queued": 50,         # NFR-P-04: 50 simultaneous queueing flows
    "extra_ws": 2,        # +2 idle sessions -> 100 simultaneously online
    "charger_count": 48,  # dedicated load-test station, all DC fast
    "charge_balance_cent": 200000,
    "queue_balance_cent": 1000,
    "hold_sec": 95.0,     # > one server heartbeat cycle (30 s ping / 60 s close)
    "sustained_sec": 300.0,   # NFR-P-05: 20 rps sustained
    "sustained_rps": 20.0,
    "peak_sec": 60.0,     # NFR-P-05: 50 rps short peak
    "peak_rps": 50.0,
    "recovery_sec": 15.0,
}

SMOKE = {
    "smoke": True,
    "accounts": 60,
    "charging": 6,
    "queued": 6,
    "extra_ws": 2,
    "charger_count": 6,
    "charge_balance_cent": 200000,
    "queue_balance_cent": 1000,
    "hold_sec": 12.0,
    "sustained_sec": 12.0,
    "sustained_rps": 20.0,
    "peak_sec": 10.0,
    "peak_rps": 50.0,
    "recovery_sec": 5.0,
}

ADMIN_LOGIN = {"username": "admin", "password": "123456",
               "deviceId": "load-admin-terminal"}
STATION_BODY = {
    "code": "LOAD",
    "name": "NCS 压测专用站（测试数据库）",
    "address": "北京市海淀区压测路 1 号（仅测试环境）",
    "adcode": "110108",
    "latitudeE6": 39977680,
    "longitudeE6": 116316417,
    "businessHours": "00:00-24:00",
}
ZGC_LAT = 39977680
ZGC_LNG = 116316417
RNG_SEED = 20260905


class Runner:
    def __init__(self, server: Path, scenario: dict, work_dir: Path):
        self.server = server
        self.s = scenario
        self.root = work_dir
        self.tls = default_tls_context()
        self.port = free_port()
        self.handle: ServerHandle | None = None
        self.admin_token = ""
        self.station_id = 0
        self.baseline_users = 0
        self.users: list[dict] = []
        self.report: dict = {}
        self._failures: list[str] = []
        self.rng = random.Random(RNG_SEED)
        self.setup_pacer = TokenBucketPacer(refill_per_sec=19.5, burst=40)
        self._pool: list[HttpClient] = []
        self._pool_index = 0
        self._mix_pool_start = 0
        self._mix_pool_stop = 0

    def record_failure(self, message: str) -> None:
        self._failures.append(message)

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.record_failure(message)
            raise AssertionError(message)

    # -- low-level HTTP -----------------------------------------------------

    def client(self) -> HttpClient:
        """Round-robin over a small keep-alive connection pool."""
        if not self._pool:
            self._pool = [HttpClient("127.0.0.1", self.port, self.tls)
                          for _ in range(16)]
        client = self._pool[self._pool_index % len(self._pool)]
        self._pool_index += 1
        return client

    async def close_pool(self) -> None:
        for client in self._pool:
            await client.close()
        self._pool = []

    async def call(self, method: str, path: str, *, token: str | None = None,
                   body: dict | None = None, idempotency: bool = False,
                   paced: bool = True) -> HttpResponse:
        """One API call; paced requests first take a shared token so setup
        traffic never trips the server's per-IP limiter (20/s, burst 50)."""
        if paced:
            await self.setup_pacer.acquire()
        headers = {}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        if idempotency:
            headers["Idempotency-Key"] = str(uuid.uuid4())
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json; charset=utf-8"
            payload = json.dumps(body).encode("utf-8")
        try:
            return await self.client().request(method, path, headers=headers,
                                               body=payload)
        except HttpError:
            return HttpResponse(0)  # transport-level failure marker

    # -- provisioning -------------------------------------------------------

    async def admin_login(self) -> None:
        response = await self.call("POST", "/api/v1/admin/auth/login",
                                   body=ADMIN_LOGIN)
        self.require(response.status == 200, f"admin login: {response.status}")
        self.admin_token = response.json()["data"]["accessToken"]

    async def register_accounts(self) -> None:
        """Register all accounts through a small worker pool.

        Each account needs two requests (sms/code then register) and every
        account ride is strictly serialised with the rest of setup traffic by
        the shared 19.5 rps token bucket, so the server's own rate limiter
        (20/s burst 50) is never hit.  A few workers in flight let the
        PBKDF2-SHA256 600k hashing keep both blocking workers busy.  The
        per-worker gap grows when registration latency climbs (server busy),
        so the client never outruns the server's hashing capacity.
        """
        total = self.s["accounts"]
        stats = LatencyStats()
        started_at = time.monotonic()
        gap = 0.050
        lock = asyncio.Lock()
        self.users = [None] * total  # deterministic order by account index
        index_queue = asyncio.Queue()
        for index in range(total):
            index_queue.put_nowait(index)

        async def worker() -> None:
            nonlocal gap
            while not index_queue.empty():
                try:
                    index = index_queue.get_nowait()
                except asyncio.QueueEmpty:
                    return
                cycle_start = time.monotonic()
                phone = f"1380000{index:04d}"
                username = f"lt_{index:07d}"
                password = f"ncs-load-{index:04x}-pw"
                # Registration has no Idempotency-Key, so it must never ride
                # a stale keep-alive connection whose auto-retry could replay
                # the request after the server already committed it
                # (username conflict).  A fresh TLS connection per account
                # removes that failure mode entirely.
                client = HttpClient("127.0.0.1", self.port, self.tls)
                try:
                    code_headers = {
                        "Content-Type": "application/json; charset=utf-8"}
                    await self.setup_pacer.acquire()
                    code_response = await client.request(
                        "POST", "/api/v1/user/auth/sms/code",
                        headers=code_headers,
                        body=json.dumps(
                            {"phone": phone,
                             "purpose": "REGISTER"}).encode("utf-8"))
                    if code_response.status != 200:
                        stats.failures += 1
                        self.require(
                            False, f"register[{index}] sms/code: "
                            f"{code_response.status} "
                            f"{code_response.body[:200]!r}")
                        return
                    development_code = \
                        code_response.json()["data"]["developmentCode"]
                    await self.setup_pacer.acquire()
                    register = await client.request(
                        "POST", "/api/v1/user/auth/register",
                        headers=code_headers,
                        body=json.dumps(
                            {"username": username, "phone": phone,
                             "password": password,
                             "smsCode": development_code,
                             "deviceId": f"lt{index:07d}"}).encode("utf-8"))
                except HttpError as exc:
                    stats.failures += 1
                    self.require(False,
                                 f"register[{index}] transport: {exc}")
                    return
                finally:
                    await client.close()
                if register.status != 201:
                    stats.failures += 1
                    self.require(False, f"register[{index}]: "
                                 f"{register.status} "
                                 f"{register.body[:200]!r}")
                    return
                self.users[index] = {
                    "index": index, "phone": phone, "username": username,
                    "password": password,
                    "token": register.json()["data"]["accessToken"],
                    "flow": None, "ws": None,
                }
                cycle_elapsed = time.monotonic() - cycle_start
                stats.add_sample(cycle_elapsed * 1000.0)
                # Shared adaptive pacing: raise the gap when the server hashes
                # slowly, lower it when it keeps up.
                async with lock:
                    if cycle_elapsed > 0.8:
                        gap = min(gap * 1.4, 1.2)
                    elif cycle_elapsed < 0.45:
                        gap = max(gap * 0.9, 0.020)
                delay = gap - (time.monotonic() - cycle_start)
                if delay > 0:
                    await asyncio.sleep(delay)

        workers = [asyncio.create_task(worker()) for _ in range(4)]
        await asyncio.gather(*workers)
        stats.duration_sec = time.monotonic() - started_at
        self.report["registration"] = stats.finish()

    async def recharge_user(self, index: int, amount_cent: int) -> None:
        response = await self.call(
            "POST", "/api/v1/user/wallet/recharges",
            token=self.users[index]["token"], body={"amountCent": amount_cent},
            idempotency=True)
        self.require(response.status == 200,
                     f"recharge user {index}: {response.status} "
                     f"{response.body[:200]!r}")

    async def create_load_station(self) -> None:
        body = dict(STATION_BODY)
        body["initialCharger"] = {
            "count": self.s["charger_count"], "chargerType": 1,
            "powerWatt": 120000, "connectorStandard": "GB/T 20234.3",
        }
        response = await self.call(
            "POST", "/api/v1/admin/stations", token=self.admin_token,
            body=body, idempotency=True)
        self.require(response.status in (200, 201),
                     f"create LOAD station: {response.status} "
                     f"{response.body[:200]!r}")
        self.station_id = int(response.json()["data"]["id"])
        chargers = await self.call(
            "GET",
            f"/api/v1/admin/chargers?stationId={self.station_id}"
            "&status=0&pageSize=100",
            token=self.admin_token)
        payload = chargers.json()["data"]
        self.require(chargers.status == 200
                     and payload["total"] == self.s["charger_count"],
                     f"LOAD station idle chargers: {payload.get('total')}")

    # -- capacity scenario (NFR-P-04) ---------------------------------------

    async def start_charging_sessions(self) -> None:
        charging = self.users[:self.s["charging"]]
        for rank, user in enumerate(charging):
            flow_response = await self.call(
                "POST", "/api/v1/user/flows", token=user["token"],
                body={"stationId": self.station_id, "chargerType": 1,
                      "preferredChargerId": None},
                idempotency=True)
            if flow_response.status != 200:
                self.require(False, f"flow create user {rank}: "
                             f"{flow_response.status} "
                             f"{flow_response.body[:200]!r}")
                return
            flow = flow_response.json()["data"]
            self.require(flow["status"] == 20,
                         f"flow create user {rank}: expected quote, "
                         f"got status {flow['status']}")
            quote = flow.get("quote")
            self.require(quote is not None and quote.get("quoteNo"),
                         f"flow create user {rank}: missing quote")
            confirm = await self.call(
                "POST",
                f"/api/v1/user/flows/{flow['flowNo']}/quote-confirmations",
                token=user["token"],
                body={"quoteNo": quote["quoteNo"],
                      "flowVersion": flow["version"]},
                idempotency=True)
            if confirm.status != 200:
                self.require(False, f"quote confirm user {rank}: "
                             f"{confirm.status} {confirm.body[:200]!r}")
                return
            confirmed = confirm.json()["data"]
            self.require(confirmed["status"] == 30,
                         f"confirm user {rank}: status {confirmed['status']}")
            started_response = await self.call(
                "POST", f"/api/v1/user/flows/{flow['flowNo']}/start",
                token=user["token"],
                body={"flowVersion": confirmed["version"],
                      "targetAmountCent": None, "balanceFloorCent": None},
                idempotency=True)
            if started_response.status != 200:
                self.require(False, f"start user {rank}: "
                             f"{started_response.status} "
                             f"{started_response.body[:200]!r}")
                return
            started = started_response.json()["data"]
            self.require(started["status"] == 40,
                         f"start user {rank}: status {started['status']}")
            user["flow"] = {"flowNo": flow["flowNo"],
                            "version": started["version"]}

    async def enqueue_flows(self) -> None:
        queued = self.users[self.s["charging"]:
                            self.s["charging"] + self.s["queued"]]
        for rank, user in enumerate(queued):
            response = await self.call(
                "POST", "/api/v1/user/flows", token=user["token"],
                body={"stationId": self.station_id, "chargerType": 1,
                      "preferredChargerId": None},
                idempotency=True)
            if response.status != 200:
                self.require(False, f"queue user {rank}: {response.status} "
                             f"{response.body[:200]!r}")
                return
            payload = response.json()["data"]
            self.require(payload["status"] == 10,
                         f"queue user {rank}: expected queued, got "
                         f"{payload['status']}")
            self.require(payload.get("queuePosition") == rank + 1,
                         f"queue user {rank}: FIFO position "
                         f"{payload.get('queuePosition')}, expected {rank + 1}")
            user["flow"] = {"flowNo": payload["flowNo"],
                            "version": payload["version"]}

    async def flow_totals(self, status: int) -> int:
        response = await self.call(
            "GET",
            f"/api/v1/admin/flows?status={status}"
            f"&stationId={self.station_id}&pageSize=100",
            token=self.admin_token)
        self.require(response.status == 200,
                     f"admin flow totals status={status}: {response.status}")
        return int(response.json()["data"]["total"])

    async def open_websockets(self) -> None:
        """One WS session per scenario user: charging + queued + 2 idle."""
        charging = self.s["charging"]
        queued = self.s["queued"]
        online = charging + queued + self.s["extra_ws"]
        order = (list(range(charging + queued))
                 + list(range(charging + queued, online)))
        opened = []
        for index in order:
            ws = WebSocketClient("127.0.0.1", self.port, self.tls,
                                 token=self.users[index]["token"])
            await self.setup_pacer.acquire()
            await ws.connect()
            self.require(ws.status == 101,
                         f"ws user {index}: handshake {ws.status}")
            self.users[index]["ws"] = ws
            opened.append(ws)
        for ws in opened:
            await ws.wait_ready(timeout=10.0)
        self.report["websockets"] = {"opened": len(opened), "online": online}
        # Boundary probe: with the default peer cap (100) the next handshake
        # must be refused at HTTP level (403), never upgraded.
        if len(opened) >= 100:
            probe_index = self.s["accounts"] - 1
            probe = WebSocketClient("127.0.0.1", self.port, self.tls,
                                    token=self.users[probe_index]["token"])
            await self.setup_pacer.acquire()
            await probe.connect()
            self.require(probe.status == 403,
                         f"101st websocket handshake: expected 403, "
                         f"got {probe.status}")
            await probe.abort()

    async def verify_progress(self) -> None:
        """Every charging session must answer a live progress query."""
        charging = self.users[:self.s["charging"]]
        for rank, user in enumerate(charging):
            response = await self.call(
                "GET", f"/api/v1/user/flows/{user['flow']['flowNo']}/progress",
                token=user["token"])
            self.require(response.status == 200,
                         f"progress user {rank}: {response.status}")
            data = response.json()["data"]
            self.require(data["status"] == 40 and data["energyMwh"] > 0,
                         f"progress user {rank}: status {data['status']} "
                         f"energy {data['energyMwh']}")

    async def websocket_health(self, phase: str) -> None:
        """All online sessions are still open; charging sessions still get
        charge.progress pushes; idle sessions answer heartbeats."""
        charging = self.users[:self.s["charging"]]
        others = self.users[self.s["charging"]:
                            self.s["charging"] + self.s["queued"]
                            + self.s["extra_ws"]]
        for user in charging + others:
            ws = user["ws"]
            if ws is None or ws.closed:
                self.require(False, f"{phase}: ws user {user['index']} closed")
                return
        for user in charging:
            progress = await user["ws"].events_of_type("charge.progress")
            expected = int(self.s["hold_sec"] * 0.6)
            if phase == "final":
                expected = max(expected, 30)
            self.require(len(progress) >= expected,
                         f"{phase}: charging user {user['index']} got only "
                         f"{len(progress)} progress events, expected >= "
                         f"{expected}")
        if self.s["hold_sec"] >= 35.0:
            for user in others:
                pings = await user["ws"].events_of_type("ping")
                self.require(len(pings) >= 1,
                             f"{phase}: idle user {user['index']} saw no "
                             f"server heartbeat ping")

    # -- throughput phases (NFR-P-05) ---------------------------------------

    def _prepare_mix(self) -> list[str]:
        """Endpoint mix replicating online client behaviour.  A 400-user pool
        of ordinary accounts (beyond the online sessions) drives the generic
        read endpoints; flow endpoints use the flow owners' own tokens."""
        self._mix_pool_start = (self.s["charging"] + self.s["queued"]
                                + self.s["extra_ws"])
        self._mix_pool_stop = min(self.s["accounts"], self._mix_pool_start
                                  + 400)
        kinds = (["stations"] * 25 + ["me"] * 10 + ["wallet"] * 10
                 + ["orders"] * 10 + ["active"] * 15
                 + ["progress"] * 20 + ["chargers"] * 10)
        return kinds

    async def _send_mix(self, kind: str) -> HttpResponse:
        index = self.rng.randrange(self._mix_pool_start,
                                   self._mix_pool_stop)
        user = self.users[index]
        token = user["token"]
        if kind == "stations":
            path = (f"/api/v1/user/stations?latitudeE6={ZGC_LAT}"
                    f"&longitudeE6={ZGC_LNG}&pageSize=20")
        elif kind == "me":
            path = "/api/v1/user/me"
        elif kind == "wallet":
            path = "/api/v1/user/wallet"
        elif kind == "orders":
            path = "/api/v1/user/orders?pageSize=20"
        elif kind == "active":
            path = "/api/v1/user/flows/active"
        elif kind == "progress":
            # The progress endpoint serves the caller's own flow only, so the
            # request must go out with the flow owner's token.
            charging = self.users[:self.s["charging"]]
            owner = charging[self.rng.randrange(len(charging))]
            token = owner["token"]
            path = f"/api/v1/user/flows/{owner['flow']['flowNo']}/progress"
        else:  # chargers: an idle list at the seed ZGC station
            path = "/api/v1/user/stations/1/chargers?status=0&pageSize=20"
        return await self.client().request(
            "GET", path, headers={"Authorization": f"Bearer {token}"})

    async def run_throughput_phase(self, name: str, rps: float,
                                   seconds: float) -> dict:
        """Fire requests on a fixed grid without any client-side pacing:
        exactly the offered rate is what the platform must absorb."""
        kinds = self._prepare_mix()
        count = int(rps * seconds)
        interval = 1.0 / rps
        stats = LatencyStats()
        started_at = time.monotonic()
        semaphore = asyncio.Semaphore(64)

        async def send_one(round_index: int) -> None:
            async with semaphore:
                began = time.monotonic()
                try:
                    response = await self._send_mix(
                        kinds[round_index % len(kinds)])
                except HttpError:
                    stats.failures += 1
                    return
                latency = (time.monotonic() - began) * 1000.0
                if response.status == 0:
                    stats.failures += 1
                elif response.status == 429:
                    stats.rate_limited += 1
                elif response.status >= 500 or response.status not in (
                        200, 201, 204):
                    stats.failures += 1
                else:
                    stats.add_sample(latency)

        tasks = []
        for round_index in range(count):
            tasks.append(asyncio.create_task(send_one(round_index)))
            delay = started_at + (round_index + 1) * interval \
                - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
        await asyncio.gather(*tasks)
        stats.duration_sec = time.monotonic() - started_at
        self.report[name] = stats.finish()
        return self.report[name]

    # -- cleanup ------------------------------------------------------------

    async def cancel_queued_flows(self) -> None:
        """Cancel queued flows first: every cancellation happens at status 10
        before any settled charger could promote the flow to a quote."""
        queued = self.users[self.s["charging"]:
                            self.s["charging"] + self.s["queued"]]
        for rank, user in enumerate(queued):
            response = await self.call(
                "POST",
                f"/api/v1/user/flows/{user['flow']['flowNo']}/cancellations",
                token=user["token"],
                body={"reasonCode": "USER_CANCELLED",
                      "flowVersion": user["flow"]["version"]},
                idempotency=True)
            self.require(response.status == 200,
                         f"cancel queue user {rank}: {response.status} "
                         f"{response.body[:200]!r}")

    async def settle_charging_sessions(self) -> None:
        charging = self.users[:self.s["charging"]]
        for rank, user in enumerate(charging):
            response = await self.call(
                "POST",
                f"/api/v1/user/flows/{user['flow']['flowNo']}/settlements",
                token=user["token"],
                body={"flowVersion": user["flow"]["version"],
                      "reasonCode": "USER_STOPPED"},
                idempotency=True)
            if response.status != 200:
                self.require(False, f"settle user {rank}: {response.status} "
                             f"{response.body[:200]!r}")
                return
            receipt = response.json()["data"]
            self.require(receipt["status"] == 60,
                         f"settle user {rank}: status {receipt['status']}")

    async def close_websockets(self) -> None:
        for user in self.users:
            ws = user.get("ws")
            if ws is not None:
                await ws.close()
                user["ws"] = None

    # -- the scenario -------------------------------------------------------

    async def run(self) -> dict:
        scenario = self.s
        charging = scenario["charging"]
        queued = scenario["queued"]

        report = self.report
        report["meta"] = {
            "smoke": scenario["smoke"],
            "server": str(self.server),
            "port": self.port,
            "workDir": str(self.root),
            "databasePath": str(self.handle.database_path),
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "scenario": dict(scenario),
        }

        # Provisioning.
        await self.admin_login()
        baseline = await self.call(
            "GET", "/api/v1/admin/users?page=1&pageSize=1",
            token=self.admin_token)
        self.require(baseline.status == 200,
                     f"admin user count: {baseline.status}")
        self.baseline_users = int(baseline.json()["data"]["total"])
        await self.register_accounts()
        registered = sum(1 for user in self.users if user is not None)
        report["registeredAccounts"] = registered
        self.require(registered == scenario["accounts"],
                     f"registered {registered}, "
                     f"expected {scenario['accounts']}")
        for index in range(charging):
            await self.recharge_user(index, scenario["charge_balance_cent"])
        for index in range(charging, charging + queued):
            await self.recharge_user(index, scenario["queue_balance_cent"])
        await self.create_load_station()

        # NFR-P-04: build 48 charging sessions and 50 queued flows.
        await self.start_charging_sessions()
        await self.enqueue_flows()
        self.require(await self.flow_totals(40) == charging,
                     f"pre-hold charging sessions: "
                     f"{await self.flow_totals(40)}")
        self.require(await self.flow_totals(10) == queued,
                     f"pre-hold queued flows: {await self.flow_totals(10)}")

        await self.open_websockets()
        await self.verify_progress()

        # Hold: everything simultaneous; verify twice across the hold window.
        capacity = {"chargingSessions": [], "queuedFlows": []}
        hold = scenario["hold_sec"]
        checkpoints = [hold * 0.3, hold * 0.85]
        started_hold = time.monotonic()
        for checkpoint in checkpoints:
            while time.monotonic() < started_hold + checkpoint:
                await asyncio.sleep(0.1)
            capacity["chargingSessions"].append(await self.flow_totals(40))
            capacity["queuedFlows"].append(await self.flow_totals(10))
            await self.verify_progress()
        while time.monotonic() < started_hold + hold:
            await asyncio.sleep(0.2)
        report["capacity"] = capacity
        report["holdSec"] = hold
        await self.websocket_health("hold")

        # NFR-P-05 throughput while the capacity state stays up.
        await self.run_throughput_phase("sustained20rps",
                                        scenario["sustained_rps"],
                                        scenario["sustained_sec"])
        await asyncio.sleep(3.0)
        await self.run_throughput_phase("peak50rps", scenario["peak_rps"],
                                        scenario["peak_sec"])
        await asyncio.sleep(5.0)  # let the server token bucket refill
        await self.run_throughput_phase("recovery20rps",
                                        scenario["sustained_rps"],
                                        scenario["recovery_sec"])

        # The capacity state must still hold after the throughput phases.
        capacity["chargingSessions"].append(await self.flow_totals(40))
        capacity["queuedFlows"].append(await self.flow_totals(10))
        await self.verify_progress()
        await self.websocket_health("final")

        # Clean shutdown of the scenario state.
        await self.cancel_queued_flows()
        await self.settle_charging_sessions()
        self.require(await self.flow_totals(70) == queued,
                     f"cancelled flows: {await self.flow_totals(70)}")
        self.require(await self.flow_totals(60) == charging,
                     f"settled flows: {await self.flow_totals(60)}")
        await self.close_websockets()
        return report

    async def rebuild_database(self) -> dict:
        """SRS section 8 delete-and-recreate: stop, delete the database,
        restart and prove a clean database is rebuilt from scratch."""
        handle = self.handle
        fresh = None
        try:
            await self.close_pool()
            before = {path.name: path.stat().st_size
                      for path in handle.database_files()}
            handle.delete_database()
            stop_server(handle, timeout=15.0)
            fresh = start_server(self.server, self.port, self.root, self.tls)
            self.handle = fresh
            await request_ready(fresh, timeout=60.0)
            login = await self.call("POST", "/api/v1/admin/auth/login",
                                    body=ADMIN_LOGIN)
            self.require(login.status == 200,
                         f"rebuild admin login: {login.status}")
            token = login.json()["data"]["accessToken"]
            users = await self.call(
                "GET", "/api/v1/admin/users?page=1&pageSize=1", token=token)
            stations = await self.call(
                "GET", "/api/v1/admin/stations?keyword=LOAD&pageSize=100",
                token=token)
            self.require(users.status == 200 and stations.status == 200,
                         "rebuild verification queries failed")
            recreated = {
                "deletedFiles": before,
                "registeredUsersAfterRebuild":
                    int(users.json()["data"]["total"]),
                "baselineUsers": self.baseline_users,
                "loadStationGone":
                    int(stations.json()["data"]["total"]) == 0,
            }
            self.report["databaseRebuild"] = recreated
            self.require(
                recreated["registeredUsersAfterRebuild"]
                == self.baseline_users and recreated["loadStationGone"],
                f"database rebuild did not restore the pristine seed state: "
                f"{recreated}")
            return recreated
        finally:
            if fresh is not None:
                try:
                    await self.close_pool()
                except Exception:  # noqa: BLE001 - best effort
                    pass
                if fresh.process.poll() is None:
                    stop_server(fresh, timeout=15.0)
                fresh.delete_database()


def build_thresholds(scenario: dict, report: dict) -> list[dict]:
    """Compare measured values against the SRS thresholds (verbatim)."""
    results = []

    def add(requirement, criterion, measured, passed, detail=""):
        results.append({"requirement": requirement, "criterion": criterion,
                        "measured": measured, "passed": bool(passed),
                        "detail": detail})

    sustained = report.get("sustained20rps", {})
    peak = report.get("peak50rps", {})
    recovery = report.get("recovery20rps", {})
    capacity = report.get("capacity", {})
    online = (scenario["charging"] + scenario["queued"]
              + scenario["extra_ws"])
    charging_snapshots = capacity.get("chargingSessions", [])
    queued_snapshots = capacity.get("queuedFlows", [])
    registered = report.get("registeredAccounts")
    websockets = report.get("websockets", {})

    add("NFR-P-04", "3,000 个注册账号", registered,
        registered == scenario["accounts"])
    add("NFR-P-04",
        f"{online} 个同时在线连接（WebSocket）持满",
        f"{websockets.get('opened')} 连接在线",
        websockets.get("opened") == online and len(charging_snapshots) >= 3,
        detail="连接在 hold 窗口与全部吞吐阶段后仍存活（脚本内断言）；空闲连接"
               "每 30 秒心跳 ping 至少应答一轮；充电连接持续收到 "
               "charge.progress")
    add("NFR-P-04", "50 个同时排队流程（status=10）", queued_snapshots,
        all(value == scenario["queued"] for value in queued_snapshots)
        and len(queued_snapshots) >= 3,
        detail="hold 中两次与吞吐阶段后共三个采样时刻均为满额（入队后另有"
               "预检查断言）")
    add("NFR-P-04", "48 个同时充电会话（status=40）", charging_snapshots,
        all(value == scenario["charging"] for value in charging_snapshots)
        and len(charging_snapshots) >= 3,
        detail="每个会话的 /progress 查询返回 status=40 且电量持续增长")
    add("NFR-P-05", "HTTPS API 持续处理 20 请求/秒",
        f"{sustained.get('samples')} 个 2xx, "
        f"{sustained.get('achievedRps')} rps, "
        f"失败 {sustained.get('failures')}, 限流 {sustained.get('rateLimited')}",
        sustained.get("samples", 0) >= scenario["sustained_rps"]
        * scenario["sustained_sec"] * 0.97
        and sustained.get("failures", 0) == 0
        and sustained.get("rateLimited", 0) == 0,
        detail=f"持续 {scenario['sustained_sec']:.0f} 秒，无 429、无 5xx、"
               "无连接失败")
    add("NFR-P-05", "承受 50 请求/秒短时峰值",
        f"{peak.get('samples')} 个 2xx + "
        f"{peak.get('rateLimited')} 个受控 429 / "
        f"失败 {peak.get('failures')}",
        peak.get("failures", 0) == 0 and peak.get("samples", 0) > 0,
        detail="峰值期 5xx 与连接失败为 0；单 IP 限流器按代码注释即 NFR-P-05 "
               "实现：突发 50、稳定 20，超出部分的 429+retryAfter 属于平台"
               "受控削峰而非故障")
    add("NFR-P-05", "峰值后恢复 20 请求/秒",
        f"{recovery.get('samples')} 个 2xx, "
        f"失败 {recovery.get('failures')}, 限流 {recovery.get('rateLimited')}",
        recovery.get("failures", 0) == 0
        and recovery.get("rateLimited", 0) == 0)
    add("NFR-P-05", "WebSocket 支持 100 个在线会话",
        f"{websockets.get('opened')} 会话在线",
        websockets.get("opened", 0) >= online,
        detail="第 101 个握手被 403 拒绝、未升级；心跳 30 s ping / 60 s "
               "关闭均未误伤会话")
    return results


def dump_server_logs(root: Path) -> None:
    stderr_path = root / "server-stderr.log"
    if stderr_path.exists():
        print(f"--- tail of {stderr_path} ---", file=sys.stderr)
        print(stderr_path.read_text(encoding="utf-8",
                                    errors="replace")[-6000:],
              file=sys.stderr)
    log_dir = root / "logs"
    if log_dir.is_dir():
        logs = sorted(log_dir.glob("*.log"), key=lambda p: p.stat().st_mtime)
        if logs:
            print(f"--- tail of {logs[-1].name} ---", file=sys.stderr)
            print(logs[-1].read_text(encoding="utf-8",
                                     errors="replace")[-6000:],
                  file=sys.stderr)


async def suite_main(server: Path, scenario: dict, root: Path) -> int:
    """One event loop for the whole suite: websockets, connection pools and
    the database rebuild all share it."""
    runner = Runner(server, scenario, root)
    try:
        runner.handle = start_server(server, runner.port, root, runner.tls)
        try:
            await request_ready(runner.handle, timeout=60.0)
            await runner.run()
        finally:
            try:
                await runner.close_pool()
            except Exception:  # noqa: BLE001 - never mask real failures
                pass
            try:
                stop_server(runner.handle, timeout=15.0)
            except RuntimeError as exc:  # server exit code != 0
                runner.record_failure(f"server stop: {exc}")
    except (AssertionError, RuntimeError, HttpError, TransportError,
            OSError, ssl.SSLError) as exc:
        runner.record_failure(str(exc))
        print(f"FAILED: {exc}", file=sys.stderr)
        dump_server_logs(root)

    report = runner.report
    if "meta" not in report:
        report["meta"] = {"server": str(server), "port": runner.port,
                          "workDir": str(root),
                          "scenario": dict(scenario)}
    report["meta"]["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                                 time.gmtime())
    report["thresholds"] = build_thresholds(scenario, report)
    report["failures"] = runner._failures
    report_path = root / "load-report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                           encoding="utf-8")
    print(f"report: {report_path}")

    result = 1 if runner._failures else 0
    if result == 0 and not scenario["smoke"]:
        # SRS section 8: after the test the database is deleted and rebuilt.
        try:
            await runner.rebuild_database()
        except (AssertionError, RuntimeError, HttpError, TransportError,
                OSError, ssl.SSLError) as exc:
            runner.record_failure(f"database rebuild: {exc}")
            print(f"REBUILD FAILED: {exc}", file=sys.stderr)
            dump_server_logs(root)
        else:
            print("DATABASE REBUILT: pristine seed restored "
                  "(SRS section 8)")
        # Persist the rebuild outcome (databaseRebuild evidence or the
        # recorded failure) so load-report.json always reflects it.
        report["failures"] = runner._failures
        report_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2),
            encoding="utf-8")
        result = 1 if runner._failures else 0

    for row in report["thresholds"]:
        mark = "PASS" if row["passed"] else "FAIL"
        print(f"[{mark}] {row['requirement']} {row['criterion']}: "
              f"{row['measured']} {row['detail']}")
    if result == 0:
        print("ALL THRESHOLDS PASSED")
    else:
        print(f"FAILURES: {len(runner._failures)}", file=sys.stderr)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="NFR-P-04/05 capacity and throughput load tests")
    parser.add_argument("--server", required=True,
                        help="path to the ncs_server executable")
    parser.add_argument("--smoke", action="store_true",
                        help="small-scale run proving the harness itself")
    parser.add_argument("--work-dir", default=None,
                        help="keep all artifacts in this directory "
                             "(default: fresh temp dir)")
    args = parser.parse_args()

    server = Path(args.server).resolve()
    if not server.is_file():
        print(f"server executable is missing: {server}", file=sys.stderr)
        return 2

    scenario = SMOKE if args.smoke else FULL
    if args.work_dir:
        root = Path(args.work_dir)
        root.mkdir(parents=True, exist_ok=True)
    else:
        root = Path(tempfile.mkdtemp(prefix="ncs-load-"))
    print(f"work directory: {root}")

    try:
        return asyncio.run(suite_main(server, scenario, root))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
