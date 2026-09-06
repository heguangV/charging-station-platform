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

The suite is assembled from small modules (all run through this entry point,
``python3 -m tests.performance.run_load_tests``):

* ``scenario.py``      FULL/SMOKE sizing and shared constants
* ``provisioning.py``  ProvisioningMixin: login, registration, recharge, station
* ``capacity.py``      CapacityMixin: NFR-P-04 sessions/queue/WebSockets
* ``throughput.py``    ThroughputMixin: NFR-P-05 fixed-grid phases
* ``cleanup.py``       CleanupMixin: teardown + delete-and-recreate rebuild
* ``thresholds.py``    build_thresholds(): SRS threshold comparison
* ``virtual_client.py`` asyncio HTTP/WebSocket virtual client (no pip)
* ``server_env.py``    server process lifecycle and helpers
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

from .capacity import CapacityMixin
from .cleanup import CleanupMixin
from .provisioning import ProvisioningMixin
from .scenario import ADMIN_LOGIN, FULL, RNG_SEED, SMOKE
from .server_env import (ServerHandle, free_port, request_ready, start_server,
                         stop_server)
from .throughput import ThroughputMixin
from .thresholds import build_thresholds
from .virtual_client import (HttpClient, HttpError, HttpResponse,
                             TokenBucketPacer, TransportError,
                             default_tls_context)


class Runner(ProvisioningMixin, CapacityMixin, ThroughputMixin,
             CleanupMixin):
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

        # NFR-P-05 throughput while the capacity state stays up: sustained
        # first, then the strict 50 rps burst, then the 60 s overload probe,
        # then the post-overload recovery.
        await self.run_throughput_phase("sustained20rps",
                                        scenario["sustained_rps"],
                                        scenario["sustained_sec"])
        await asyncio.sleep(3.0)  # let the server token bucket refill fully
        await self.run_throughput_phase("burst50rps", scenario["burst_rps"],
                                        scenario["burst_sec"])
        await asyncio.sleep(1.0)
        await self.run_throughput_phase("overload60s",
                                        scenario["overload_rps"],
                                        scenario["overload_sec"],
                                        allow_429=True, require_429=True)
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
    # A FAIL row is a failure: the exit code and the rebuild gate must never
    # turn green on a soft threshold miss.
    failed_rows = [row for row in report["thresholds"] if not row["passed"]]
    if failed_rows:
        runner.record_failure(
            "threshold FAIL: " + "; ".join(
                f"{row['requirement']} {row['criterion']}"
                for row in failed_rows))
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
        if root.exists() and any(root.iterdir()):
            print(f"work directory must be empty: {root}", file=sys.stderr)
            return 2
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
