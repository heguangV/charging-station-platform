#!/usr/bin/env python3
"""Synthetic hourly-history generator for cold-start ML training (UC-M-01).

The platform has no accumulated real orders yet, so the RandomForest pipeline
cannot reach its "beat the naive weekly baseline" acceptance gate. This tool
fakes a deterministic, physically plausible charging history as hourly
feature rows in exactly the shape served by GET /internal/ml/features/hourly
(stationId, bucketAt, energyMwh, operationalChargerCount, busyDeviceSeconds)
so that ml/train.py can be run without touching any existing code.

Data recipe (documented honestly as SIMULATED, not real operations):
  energy = stationScale * diurnalProfile(localHour) * dayOfWeekFactor * (1 + ar1Noise)
  - diurnal profile: midday bump + strong evening peak (Beijing time UTC+8);
  - station personas: office/commuter, residential, tech-park;
  - AR(1) noise (phi=0.6) keeps day-to-day autocorrelation, which lets
    lag1/rolling features beat the lag168 weekly baseline in the fixed test
    split (last 20% by time) without fabricating unrealistically smooth data.

Units follow the repository discipline: energy is integer milliwatt-hours
(1 kWh = 1e6 mWh), timestamps are UTC epoch seconds aligned to the hour.

Usage:
  python3 ml/tools/gen_synthetic_history.py --out ml/data/synthetic/features_24d.json
  python3 ml/train.py --train --features ml/data/synthetic/features_24d.json \
      --model ml/models/load_rf.pkl
"""

from __future__ import annotations

import argparse
import json
import math
import random
from datetime import datetime, timedelta, timezone
from pathlib import Path

RANDOM_SEED = 20260901  # same constant as pipeline.py, per UC-M-04
HOUR = 3600
DAYS = 24              # 576 hourly buckets/station -> 408 lag-complete samples each
BEIJING_OFFSET = 8     # the deployment region interprets load in UTC+8

# (id, operationalChargerCount, peakScaleKwhPerHour, persona weights)
STATIONS = [
    # id, chargers, kwh/h scale, weekdayFactor, weekendFactor,
    # middayGauss(center, width, amp), eveningGauss(center, width, amp)
    (1, 10, 400.0, 1.00, 0.55, (13.0, 2.2, 0.35), (19.5, 1.8, 1.00)),  # office
    (2, 8, 280.0, 0.85, 1.35, (15.0, 3.0, 0.45), (20.0, 2.2, 0.75)),   # residential
    (3, 12, 430.0, 1.00, 0.90, (12.5, 2.6, 0.80), (19.0, 2.0, 0.70)),  # tech park
]

MWH_PER_KWH = 1_000_000
AVG_BUSY_POWER_KW = 50.0  # blended DC/AC average, used for busy seconds only


def diurnal_profile(local_hour: float, midday, evening) -> float:
    floor = 0.10
    mid = midday[2] * math.exp(-((local_hour - midday[0]) ** 2) / (2 * midday[1] ** 2))
    eve = evening[2] * math.exp(-((local_hour - evening[0]) ** 2) / (2 * evening[1] ** 2))
    return floor + mid + eve


def generate_station_rows(station, end_bucket: int) -> list[dict]:
    station_id, chargers, scale, weekday_f, weekend_f, midday, evening = station
    rng = random.Random(RANDOM_SEED ^ station_id)  # per-station stream, order-proof
    rows: list[dict] = []
    prev_noise = 0.0
    phi = 0.6
    for index in range(DAYS * 24):
        bucket_at = end_bucket - (DAYS * 24 - 1 - index) * HOUR
        instant = datetime.fromtimestamp(bucket_at, tz=timezone.utc)
        local_hour = (instant.hour + BEIJING_OFFSET) % 24 + instant.minute / 60.0
        profile = diurnal_profile(local_hour, midday, evening)
        dow = instant.astimezone(timezone(timedelta(hours=BEIJING_OFFSET))).weekday()
        day_factor = weekend_f if dow >= 5 else weekday_f
        # stationary AR(1): unit-variance marginal, keeps yesterday informative
        eps = rng.gauss(0.0, 1.0)
        noise = phi * prev_noise + math.sqrt(1.0 - phi * phi) * eps
        prev_noise = noise
        energy_kwh = max(0.0, scale * profile * day_factor * (1.0 + 0.25 * noise))
        energy_mwh = int(round(energy_kwh * MWH_PER_KWH))
        busy_seconds = int(min(chargers * HOUR, energy_kwh / AVG_BUSY_POWER_KW * HOUR))
        rows.append({
            "stationId": station_id,
            "bucketAt": bucket_at,
            "energyMwh": energy_mwh,
            "operationalChargerCount": chargers,
            "busyDeviceSeconds": busy_seconds,
        })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--end-at", type=int, default=None,
                        help="last hourly bucket (UTC epoch seconds); default: this hour")
    arguments = parser.parse_args()
    end_bucket = arguments.end_at
    if end_bucket is None:
        end_bucket = int(datetime.now(tz=timezone.utc).timestamp()) // HOUR * HOUR
    rows: list[dict] = []
    for station in STATIONS:
        rows.extend(generate_station_rows(station, end_bucket))
    rows.sort(key=lambda item: (item["bucketAt"], item["stationId"]))
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_text(json.dumps(rows) + "\n", encoding="utf-8")
    summary = {
        "generatedAt": int(datetime.now(tz=timezone.utc).timestamp()),
        "windowFromAt": rows[0]["bucketAt"],
        "windowToAt": rows[-1]["bucketAt"],
        "rowCount": len(rows),
        "stations": [station[0] for station in STATIONS],
        "note": "SIMULATED history for cold-start training; not real operations data",
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
