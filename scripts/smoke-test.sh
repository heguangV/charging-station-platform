#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_dir}/build/dev}"

# 车主端（apps/user）与管理端（apps/admin）均已改造为 Vue 3 Web 客户端：
# 没有 ncs_user / ncs_admin 可执行目标，C++ 冒烟只剩服务端；
# Web 端冒烟在各自目录执行 npm run test 与 npm run build。

# ncs_server 没有一次性 --smoke-test 模式：以临时 PostgreSQL 实例在回环端口拉起进程，
# 就绪探活通过即视为冒烟成功，退出时清理两个进程与临时目录。
if [[ -n "${NCS_PG_BIN:-}" ]]; then
    pg_bin="${NCS_PG_BIN}"
elif command -v pg_config >/dev/null 2>&1; then
    pg_bin="$(pg_config --bindir)"
elif [[ -x /opt/homebrew/opt/postgresql@18/bin/initdb ]]; then
    pg_bin=/opt/homebrew/opt/postgresql@18/bin
else
    echo "PostgreSQL 18 tools are required for the server smoke test" >&2
    exit 8
fi

server_port="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"
database_port="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"
server_dir="$(mktemp -d)"
database_dir="${server_dir}/postgres"
socket_dir="${server_dir}/socket"
server_pid=""
cleanup() {
    if [[ -n "${server_pid}" ]]; then
        kill "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
    if [[ -f "${database_dir}/postmaster.pid" ]]; then
        "${pg_bin}/pg_ctl" -D "${database_dir}" -m immediate stop >/dev/null 2>&1 || true
    fi
    rm -rf "${server_dir}"
}
trap cleanup EXIT

mkdir -p "${socket_dir}"
"${pg_bin}/initdb" -D "${database_dir}" --auth=trust --encoding=UTF8 --no-locale >/dev/null
"${pg_bin}/pg_ctl" -D "${database_dir}" \
    -o "-h 127.0.0.1 -k ${socket_dir} -p ${database_port}" \
    -l "${server_dir}/postgres.log" start >/dev/null
"${pg_bin}/createdb" -h 127.0.0.1 -p "${database_port}" ncs_smoke

"${build_dir}/ncs_server" \
    --environment development \
    --listen-address 127.0.0.1 \
    --port "${server_port}" \
    --allow-insecure-http true \
    --database-driver postgresql \
    --database-host 127.0.0.1 \
    --database-port "${database_port}" \
    --database-name ncs_smoke \
    --database-user "$(id -un)" \
    --database-sslmode disable \
    --database-migrations "${repo_dir}/infrastructure/postgres/migrations" \
    --database-backup-directory "${server_dir}/backups" \
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
