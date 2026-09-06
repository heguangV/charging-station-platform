"""NFR-P-04 / NFR-P-05 scenario sizing and shared constants.

FULL matches the SRS numbers exactly; SMOKE exercises the same code
paths at tiny scale so script errors surface before the full run.
"""

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
    "burst_rps": 50.0,    # NFR-P-05: 50 rps short burst, all 2xx (strict)
    "burst_sec": 1.0,
    "overload_rps": 50.0,  # overload probe: 50 rps past the refill rate
    "overload_sec": 60.0,
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
    "burst_rps": 50.0,
    "burst_sec": 1.0,
    "overload_rps": 50.0,
    "overload_sec": 10.0,
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
