"""NFR-P-04 capacity mixin: charging sessions, FIFO queueing and the
100-session WebSocket fabric."""

from .virtual_client import WebSocketClient

class CapacityMixin:

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
