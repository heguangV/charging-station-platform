#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_dir}/build/dev}"

QT_QPA_PLATFORM=offscreen "${build_dir}/apps/user/ncs_user" --smoke-test
QT_QPA_PLATFORM=offscreen "${build_dir}/apps/admin/ncs_admin" --smoke-test

# ncs_server 没有一次性 --smoke-test 模式：以临时数据库在回环端口拉起进程，
# 就绪探活通过即视为冒烟成功，退出时清理进程与临时目录。
server_port="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"
server_dir="$(mktemp -d)"
server_pid=""
cleanup() {
    if [[ -n "${server_pid}" ]]; then
        kill "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
    rm -rf "${server_dir}"
}
trap cleanup EXIT

"${build_dir}/ncs_server" \
    --environment development \
    --listen-address 127.0.0.1 \
    --port "${server_port}" \
    --allow-insecure-http true \
    --database-path "${server_dir}/smoke.db" \
    --log-directory "${server_dir}" \
    >"${server_dir}/stdout.log" 2>&1 &
server_pid=$!

server_ready=false
for _ in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:${server_port}/api/v1/system/health/ready" >/dev/null 2>&1; then
        server_ready=true
        break
    fi
    kill -0 "${server_pid}" 2>/dev/null || break
    sleep 0.5
done
if [[ "${server_ready}" != true ]]; then
    echo "ncs_server smoke test failed; see ${server_dir}/stdout.log" >&2
    exit 7
fi
echo "ncs_server smoke test ok (127.0.0.1:${server_port})"
