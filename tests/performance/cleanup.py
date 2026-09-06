"""Cleanup mixin: scenario teardown and the SRS section-8
delete-and-recreate database rebuild (stop first, then delete)."""

from .scenario import ADMIN_LOGIN
from .server_env import request_ready, start_server, stop_server

class CleanupMixin:

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
    async def rebuild_database(self) -> dict:
        """SRS section 8 delete-and-recreate: stop, delete the database,
        restart and prove a clean database is rebuilt from scratch."""
        handle = self.handle
        fresh = None
        try:
            await self.close_pool()
            before = {path.name: path.stat().st_size
                      for path in handle.database_files()}
            # Stop first: deleting a live server's database (WAL) under it
            # would not prove the production delete-and-recreate order.
            stop_server(handle, timeout=15.0)
            handle.delete_database()
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
