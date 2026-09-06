"""Provisioning mixin: admin login, real-API account registration,
wallet recharges and the dedicated LOAD station."""

import asyncio
import json
import time

from .scenario import ADMIN_LOGIN, STATION_BODY
from .virtual_client import HttpClient, HttpError, LatencyStats

class ProvisioningMixin:

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
                # 139 segment: the 13800001001..13800001300 range belongs to
                # the UC-D-02 demo seed's sim_owner_* accounts, so load-test
                # phones must never overlap it.
                phone = f"1390000{index:04d}"
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
