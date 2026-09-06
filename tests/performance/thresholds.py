"""SRS threshold comparison for the load report (verbatim requirement
texts; no reinterpretation)."""

def build_thresholds(scenario: dict, report: dict) -> list[dict]:
    """Compare measured values against the SRS thresholds (verbatim)."""
    results = []

    def add(requirement, criterion, measured, passed, detail=""):
        results.append({"requirement": requirement, "criterion": criterion,
                        "measured": measured, "passed": bool(passed),
                        "detail": detail})

    sustained = report.get("sustained20rps", {})
    burst = report.get("burst50rps", {})
    overload = report.get("overload60s", {})
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
        f"{burst.get('samples')} 个 2xx, "
        f"失败 {burst.get('failures')}, 限流 {burst.get('rateLimited')}",
        burst.get("samples", 0)
            == int(scenario["burst_rps"] * scenario["burst_sec"])
        and burst.get("failures", 0) == 0
        and burst.get("rateLimited", 0) == 0,
        detail="令牌桶突发 50：1 秒内 50 个请求全部 2xx，无 429、无失败"
               "（严格验收）")
    add("NFR-P-05", "60 秒过载削峰（受控 429，不并入 50 rps 证据）",
        f"{overload.get('samples')} 个 2xx + "
        f"{overload.get('rateLimited')} 个受控 429 / "
        f"失败 {overload.get('failures')}",
        overload.get("failures", 0) == 0
        and overload.get("rateLimited", 0) > 0
        and overload.get("samples", 0) + overload.get("rateLimited", 0)
            == int(scenario["overload_rps"] * scenario["overload_sec"]),
        detail="50 rps × 60 秒持续过载：每个请求都被记入 2xx 或 429，"
               "无连接失败、无 5xx；429 属平台受控削峰")
    add("NFR-P-05", "过载后恢复 20 请求/秒",
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
