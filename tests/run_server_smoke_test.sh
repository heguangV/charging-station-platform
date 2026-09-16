#!/usr/bin/env bash
set -euo pipefail

python_executable="$1"
test_script="$2"
server_binary="$3"
migrations_directory="$4"

source "$(dirname "${BASH_SOURCE[0]}")/postgres_test_tools.sh"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/ncs-server-smoke-pg.XXXXXX")"
data_dir="${work_dir}/pgdata"
socket_dir="${work_dir}/socket"
port="${NCS_PG_SERVER_SMOKE_PORT:-55441}"
mkdir -p "${socket_dir}"

cleanup() {
    if [[ -f "${data_dir}/postmaster.pid" ]]; then
        "${pg_bin}/pg_ctl" -D "${data_dir}" -m immediate stop >/dev/null 2>&1 || true
    fi
    rm -rf -- "${work_dir}"
}
trap cleanup EXIT

"${pg_bin}/initdb" -D "${data_dir}" --auth=trust --encoding=UTF8 --no-locale >/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=127.0.0.1 \
    -addext subjectAltName=IP:127.0.0.1 -keyout "${data_dir}/server.key" \
    -out "${data_dir}/server.crt" >/dev/null 2>&1
chmod 600 "${data_dir}/server.key"
"${pg_bin}/pg_ctl" -D "${data_dir}" \
    -o "-h 127.0.0.1 -k ${socket_dir} -p ${port} -c ssl=on" \
    -l "${work_dir}/postgres.log" start >/dev/null
"${pg_bin}/createdb" -h 127.0.0.1 -p "${port}" ncs_server_smoke

unset NCS_DATABASE_PASSWORD
NCS_TEST_DATABASE_HOST=127.0.0.1 \
NCS_TEST_DATABASE_PORT="${port}" \
NCS_TEST_DATABASE_NAME=ncs_server_smoke \
NCS_TEST_DATABASE_USER="$(id -un)" \
NCS_TEST_DATABASE_CA="${data_dir}/server.crt" \
NCS_TEST_DATABASE_MIGRATIONS="${migrations_directory}" \
"${python_executable}" "${test_script}" "${server_binary}"
