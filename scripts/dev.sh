#!/usr/bin/env bash
# 本地联调一键联动：服务端常驻（systemd --user），三个端按需拉起。
# 用法：scripts/dev.sh [up|down|status|restart]
# 默认参数与当前联调环境一致：127.0.0.1:18443，开发库 /tmp/ncs-dev/platform.db。
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${NCS_DEV_BIN_DIR:-$REPO_DIR/build-ncs}"
DEV_DIR="${NCS_DEV_DIR:-/tmp/ncs-dev}"
HOST="${NCS_SERVER_HOST:-127.0.0.1}"
PORT="${NCS_SERVER_PORT:-18443}"
HEALTH_URL="http://$HOST:$PORT/api/v1/system/health/ready"

client_env() {
    # 与仓库根目录 .env 中的腾讯地图 Key 共用；NCS_ 网络参数指向联调服务端。
    # 从桌面终端或无图形变量的 shell 中均可启动 GUI：缺会话变量时按本机 XWayland 默认值补齐。
    local display="${DISPLAY:-:0}"
    local xauthority="${XAUTHORITY:-$(ls /run/user/$(id -u)/.mutter-Xwaylandauth.* 2>/dev/null | head -1)}"
    env NCS_ALLOW_INSECURE_HTTP=true NCS_ENV=development \
        NCS_SERVER_HOST="$HOST" NCS_SERVER_PORT="$PORT" \
        NCS_ENV_FILE="${NCS_ENV_FILE:-$REPO_DIR/.env}" \
        DISPLAY="$display" \
        XAUTHORITY="$xauthority" \
        DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}" \
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
        QT_IM_MODULE="${QT_IM_MODULE:-fcitx}" \
        "$@"
}

wait_ready() {
    for _ in $(seq 1 30); do
        if curl -sf --max-time 2 "$HEALTH_URL" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "服务端就绪检查超时：$HEALTH_URL" >&2
    return 1
}

start_server() {
    systemctl --user enable --now ncs-server.service >/dev/null 2>&1 || true
    wait_ready
    echo "服务端：http://$HOST:$PORT（systemd --user ncs-server，常驻后台）"
}

start_client() { # name binary extra-args...
    local name="$1" bin="$2"
    shift 2
    if pgrep -x "$name" >/dev/null 2>&1; then
        echo "$name：已在运行（pid $(pgrep -x "$name" | head -1)），跳过"
        return
    fi
    client_env nohup "$bin" "$@" >>"$DEV_DIR/$name.log" 2>&1 &
    disown
    sleep 1
    if pgrep -x "$name" >/dev/null 2>&1; then
        echo "$name：已启动（日志 $DEV_DIR/$name.log）"
    else
        echo "$name：启动失败，见 $DEV_DIR/$name.log" >&2
        return 1
    fi
}

start_dashboard() {
    if pgrep -f 'vite --config vite.config.local.ts' >/dev/null 2>&1; then
        echo "Web 大屏：已在运行（pid $(pgrep -f 'vite --config vite.config.local.ts' | head -1)），跳过"
        return
    fi
    if ss -tln 2>/dev/null | grep -q ':3000 '; then
        echo "Web 大屏：3000 被其他进程占用（$(ss -tlnp 2>/dev/null | grep ':3000 ' | grep -oE 'pid=[0-9]+' | head -1)），未启动" >&2
        return 1
    fi
    # 直接使用项目本地 vite，不依赖 pnpm 是否在 PATH 中
    (cd "$REPO_DIR/apps/dashboard" &&
        nohup node node_modules/vite/bin/vite.js --config vite.config.local.ts >>"$DEV_DIR/dashboard.log" 2>&1 &
        disown)
    for _ in $(seq 1 15); do
        ss -tln 2>/dev/null | grep -q ':3000 ' && break
        sleep 1
    done
    if ss -tln 2>/dev/null | grep -q ':3000 '; then
        echo "Web 大屏：http://127.0.0.1:3000（日志 $DEV_DIR/dashboard.log）"
    else
        echo "Web 大屏：启动失败，见 $DEV_DIR/dashboard.log" >&2
        return 1
    fi
}

cmd_up() {
    mkdir -p "$DEV_DIR"
    start_server
    start_client ncs_admin "$BIN_DIR/apps/admin/ncs_admin" --config "$DEV_DIR/admin.env"
    start_client ncs_user "$BIN_DIR/apps/user/ncs_user"
    start_dashboard
}

cmd_down() {
    pkill -x ncs_user 2>/dev/null || true
    pkill -x ncs_admin 2>/dev/null || true
    # 同时命中 pnpm 包装进程与 node vite.js 子进程（子进程命令行里没有 "vite --config" 字面量）
    pkill -f 'vite\.config\.local\.ts' 2>/dev/null || true
    # 兜底：包装进程被杀后 vite 子进程可能成为孤儿残留，按端口清理
    if ss -tln 2>/dev/null | grep -q ':3000 '; then
        local port_pid
        port_pid=$(ss -tlnp 2>/dev/null | grep ':3000 ' | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
        if [ -n "$port_pid" ]; then
            kill "$port_pid" 2>/dev/null || true
            sleep 1
        fi
    fi
    systemctl --user stop ncs-server.service 2>/dev/null || true
    echo "服务端与三端已停止"
}

cmd_status() {
    if curl -sf --max-time 2 "$HEALTH_URL" >/dev/null 2>&1; then
        echo "服务端：运行中 http://$HOST:$PORT（$(systemctl --user is-active ncs-server.service)）"
    else
        echo "服务端：不可达（$(systemctl --user is-active ncs-server.service 2>/dev/null || echo unknown)）"
    fi
    for c in ncs_admin ncs_user; do
        if pgrep -x "$c" >/dev/null 2>&1; then
            echo "$c：运行中（pid $(pgrep -x "$c" | tr '\n' ' ')）"
        else
            echo "$c：未运行"
        fi
    done
    if ss -tln 2>/dev/null | grep -q ':3000 '; then
        echo "Web 大屏：运行中 http://127.0.0.1:3000"
    else
        echo "Web 大屏：未运行"
    fi
}

case "${1:-status}" in
    up) cmd_up ;;
    down) cmd_down ;;
    status) cmd_status ;;
    restart) cmd_down; cmd_up ;;
    *) echo "用法：$0 [up|down|status|restart]" >&2; exit 2 ;;
esac
