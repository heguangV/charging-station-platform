"""NFR-P-05 throughput mixin: endpoint mix and the fixed-grid phases
(sustained 20 rps, strict 50 rps burst, 60 s overload probe, recovery).
"""

import asyncio
import time

from .scenario import ZGC_LAT, ZGC_LNG
from .virtual_client import HttpError, HttpResponse, LatencyStats

class ThroughputMixin:

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
                                   seconds: float, *,
                                   allow_429: bool = False,
                                   require_429: bool = False) -> dict:
        """Fire requests on a fixed grid without any client-side pacing:
        exactly the offered rate is what the platform must absorb.

        Expectations are asserted in-phase so a missed threshold is a hard
        failure (exit code and the rebuild gate both depend on it), not a
        soft report row.  ``allow_429`` permits controlled shedding;
        ``require_429`` additionally demands at least one 429.
        """
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
        result = stats.finish()
        self.report[name] = result
        self.require(stats.failures == 0,
                     f"{name}: {stats.failures} connection/5xx failures")
        if require_429:
            self.require(stats.rate_limited > 0,
                         f"{name}: expected controlled 429 shedding, got none")
        elif not allow_429:
            self.require(stats.rate_limited == 0,
                         f"{name}: {stats.rate_limited} unexpected 429 "
                         "responses")
        self.require(stats.samples + stats.rate_limited == count,
                     f"{name}: offered {count} requests, accounted "
                     f"{stats.samples + stats.rate_limited}")
        return result
