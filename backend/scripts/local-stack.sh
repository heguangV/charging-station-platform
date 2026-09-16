#!/usr/bin/env bash
#
# Local stack (B-06): one command to bring up the whole backend for integration work.
#
# This is the development entry point, not a verification gate: it starts PostgreSQL-backed
# processes, waits until they are actually ready, prints what is running and how to talk to it, and
# stays in the foreground until Ctrl-C. The strict assertions live in verify-closed-loop.sh and
# fault-drill.sh; this script exists so a developer does not have to remember the environment
# variables and the startup order.
#
# Prerequisites: a reachable PostgreSQL and Redis (the script does not install them; a deployment
# provisions them, and the verification host runs them as system services). For container-based
# deployments see backend/deploy/ (systemd units and an optional compose file).
#
# Usage:
#   NCS_POSTGRES_DSN=postgres://... backend/scripts/local-stack.sh [--with-nginx] [--static-root DIR] [--no-build]
#
# Environment (defaults shown):
#   NCS_REDIS_ADDR=127.0.0.1:6379     NCS_REDIS_DB=14
#   NCS_HTTP_ADDR=127.0.0.1:8080      NCS_CHARGER_GATEWAY_TOKEN=dev-gateway-token
#   NCS_MOCK_GATEWAY_RECEIPTS=true     NCS_MOCK_GATEWAY_API_URL=http://127.0.0.1:8080
#   NCS_MOCK_GATEWAY_ENERGY_WH=1000    NCS_MOCK_GATEWAY_METER_INTERVAL=3s
#   NCS_MOCK_GATEWAY_CHARGE_SECONDS=60
#   NCS_API_METRICS_ADDR=127.0.0.1:9090 NCS_WORKER_METRICS_ADDR=127.0.0.1:9091
#   NCS_PUBLISHER_METRICS_ADDR=127.0.0.1:9092
#   NCS_STATIC_ROOT=apps/dashboard    (only needed with --with-nginx)
#   NCS_ENV_FILE=backend/.env.local   (map/model credentials; loaded before defaults)
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
backend_dir="$(cd "${script_dir}/.." && pwd)"
repo_root="$(cd "${backend_dir}/.." && pwd)"

# Keep local credentials out of source control while making restarts reproducible. Previously the
# assistant only worked when the shell that launched this script happened to inherit the map/model
# variables; a restart from another terminal silently dropped both providers.
env_file="${NCS_ENV_FILE:-${backend_dir}/.env.local}"
if [[ -f "${env_file}" ]]; then
    set -a
    # shellcheck disable=SC1090 -- NCS_ENV_FILE is an explicit local operator setting.
    source "${env_file}"
    set +a
fi

: "${NCS_POSTGRES_DSN:?set NCS_POSTGRES_DSN to a disposable database}"
redis_addr="${NCS_REDIS_ADDR:-127.0.0.1:6379}"
redis_db="${NCS_REDIS_DB:-14}"
http_addr="${NCS_HTTP_ADDR:-127.0.0.1:8080}"
gateway_token="${NCS_CHARGER_GATEWAY_TOKEN:-dev-gateway-token}"
mock_receipts="${NCS_MOCK_GATEWAY_RECEIPTS:-true}"
mock_api_url="${NCS_MOCK_GATEWAY_API_URL:-http://${http_addr}}"
mock_energy_wh="${NCS_MOCK_GATEWAY_ENERGY_WH:-1000}"
mock_meter_interval="${NCS_MOCK_GATEWAY_METER_INTERVAL:-3s}"
mock_charge_seconds="${NCS_MOCK_GATEWAY_CHARGE_SECONDS:-60}"
api_metrics_addr="${NCS_API_METRICS_ADDR:-127.0.0.1:9090}"
worker_metrics_addr="${NCS_WORKER_METRICS_ADDR:-127.0.0.1:9091}"
publisher_metrics_addr="${NCS_PUBLISHER_METRICS_ADDR:-127.0.0.1:9092}"
with_nginx="false"
static_root="${NCS_STATIC_ROOT:-${repo_root}/apps/dashboard}"
build="true"
seed="false"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-nginx) with_nginx="true"; shift ;;
        --static-root) static_root="$2"; shift 2 ;;
        --no-build) build="false"; shift ;;
        --seed) seed="true"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

run_dir="${NCS_STACK_RUN_DIR:-/tmp/ncs-stack}"
gateway_port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
nginx_port="$(( ${http_addr##*:} + 43 ))"
pids=()

cleanup() {
    local status=$?
    if [[ "${with_nginx}" == "true" ]]; then
        nginx -s stop -c "${run_dir}/nginx.conf" -p "${run_dir}/nginx-prefix" 2>/dev/null || true
    fi
    for pid in "${pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    done
    echo
    echo "stack stopped (logs kept in ${run_dir})"
    exit "${status}"
}
trap cleanup EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
step() { printf '\n=== %s ===\n' "$*"; }

mkdir -p "${run_dir}"
if [[ "${build}" == "true" ]]; then
    step "build"
    (cd "${backend_dir}" && go build -o "${run_dir}/ncs-api" ./cmd/api \
        && go build -o "${run_dir}/ncs-worker" ./cmd/worker \
        && go build -o "${run_dir}/ncs-outbox-publisher" ./cmd/outbox-publisher \
        && go build -o "${run_dir}/ncs-mock-gateway" ./cmd/mock-gateway)
fi

common_env=(
    "NCS_POSTGRES_DSN=${NCS_POSTGRES_DSN}"
    "NCS_REDIS_ADDR=${redis_addr}"
    "NCS_REDIS_DB=${redis_db}"
    "NCS_REDIS_REQUIRED=true"
    # The migration gate runs the API binary, which validates its configuration before it does
    # anything - including the gateway token that has no default. Leaving it out of this array meant
    # `-migrate-only` refused to start unless the caller happened to export the token, so the stack
    # could not migrate a fresh database at all; the documented default never reached it.
    "NCS_CHARGER_GATEWAY_TOKEN=${gateway_token}"
)

step "migration gate"
env "${common_env[@]}" "${run_dir}/ncs-api" -migrate-only 2>&1 | tail -1

if [[ "${seed}" == "true" ]]; then
    # The seed runs AFTER the migration gate on purpose. Loading it first is what a fresh disposable
    # database used to hit: dev_seed.sql inserts into user_accounts, wallet_accounts, stations and the
    # rest, so on a database whose schema does not exist yet every statement fails with "relation ...
    # does not exist" and the stack never comes up. The schema has to exist before the rows.
    #
    # The seed is idempotent and refuses nothing, so the guard is here: seeding a database whose name
    # does not look disposable is how a production database ends up with demo users.
    database_name="$(python3 - "$NCS_POSTGRES_DSN" <<'READDB'
import sys, urllib.parse
print(urllib.parse.urlparse(sys.argv[1]).path.lstrip('/') or '')
READDB
)"
    case "${database_name}" in
        *test*|*dev*|*ci*|*scratch*|*sandbox*|ncs_*) ;;
        *) fail "--seed refused: database \"${database_name}\" does not look disposable" ;;
    esac
    step "seed development data into ${database_name}"
    # ON_ERROR_STOP is what makes a failed seed visible: without it psql prints the error, exits 0 and
    # the stack starts against a half-seeded database.
    psql "${NCS_POSTGRES_DSN}" -q -v ON_ERROR_STOP=1 -f "${backend_dir}/seeds/dev_seed.sql"
fi

step "start mock gateway, API, publisher and worker"
env "${common_env[@]}" NCS_MOCK_GATEWAY_ADDR="127.0.0.1:${gateway_port}" \
    NCS_MOCK_GATEWAY_RECEIPTS="${mock_receipts}" NCS_MOCK_GATEWAY_API_URL="${mock_api_url}" \
    NCS_MOCK_GATEWAY_ENERGY_WH="${mock_energy_wh}" \
    NCS_MOCK_GATEWAY_METER_INTERVAL="${mock_meter_interval}" \
    NCS_MOCK_GATEWAY_CHARGE_SECONDS="${mock_charge_seconds}" \
    "${run_dir}/ncs-mock-gateway" >"${run_dir}/gateway.log" 2>&1 &
pids+=("$!")

env "${common_env[@]}" NCS_HTTP_ADDR="${http_addr}" \
    NCS_METRICS_ADDR="${api_metrics_addr}" \
    NCS_CHARGER_GATEWAY_TOKEN="${gateway_token}" NCS_SMS_MOCK=true \
    "${run_dir}/ncs-api" >"${run_dir}/api.log" 2>&1 &
pids+=("$!")

env "${common_env[@]}" NCS_OUTBOX_INTERVAL=100ms NCS_OUTBOX_BATCH=50 \
    NCS_METRICS_ADDR="${publisher_metrics_addr}" \
    "${run_dir}/ncs-outbox-publisher" >"${run_dir}/publisher.log" 2>&1 &
pids+=("$!")

env "${common_env[@]}" NCS_WORKER_CONSUMER="worker-local" \
    NCS_METRICS_ADDR="${worker_metrics_addr}" \
    NCS_CHARGER_GATEWAY_URL="http://127.0.0.1:${gateway_port}" \
    NCS_CHARGER_GATEWAY_TIMEOUT=5s \
    "${run_dir}/ncs-worker" >"${run_dir}/worker.log" 2>&1 &
pids+=("$!")

api_url="http://${http_addr}"
for _ in $(seq 1 100); do
    curl -fsS "${api_url}/healthz" >/dev/null 2>&1 && break
    sleep 0.2
done
curl -fsS "${api_url}/healthz" >/dev/null || fail "the API did not become healthy; see ${run_dir}/api.log"
for _ in $(seq 1 100); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' "${api_url}/readyz")" == "200" ]] && break
    sleep 0.2
done
[[ "$(curl -s -o /dev/null -w '%{http_code}' "${api_url}/readyz")" == "200" ]] || fail "the API never reported ready; see ${run_dir}/api.log"

step "smoke"
curl -fsS "http://${api_metrics_addr}/metrics" >/dev/null && echo "metrics reachable"
login_body="$(curl -s -X POST "${api_url}/api/v1/auth/user/login" -H 'Content-Type: application/json' \
    -d '{"account":"13800000001","password":"Dev-Password-01"}')"
if [[ "$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("success"))' <<<"${login_body}" 2>/dev/null)" == "True" ]]; then
    token="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["accessToken"])' <<<"${login_body}")"
    stations="$(curl -fsS "${api_url}/api/v1/stations" -H "Authorization: Bearer ${token}")"
    station_count="$(python3 -c 'import json,sys; print(len(json.load(sys.stdin)["data"]["items"]))' <<<"${stations}")"
    echo "login ok as the seeded user, ${station_count} station(s) visible"
else
    echo "note: the development user is not present in this database; start with --seed to load backend/seeds/dev_seed.sql"
fi

if [[ "${with_nginx}" == "true" ]]; then
    step "render and start nginx"
    mkdir -p "${run_dir}/nginx-prefix/logs" "${run_dir}/nginx-prefix/temp" "${run_dir}/certs"
    [[ -f "${run_dir}/certs/ncs.crt" ]] || openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
        -subj "/CN=localhost" -keyout "${run_dir}/certs/ncs.key" -out "${run_dir}/certs/ncs.crt" >/dev/null 2>&1
    # Local development allows loopback for the ops endpoints; the restrictive ranges are what the
    # nginx drill proves (backend/scripts/nginx-render.sh --drill).
    export NCS_PUBLIC_HOST="localhost" NCS_HTTP_PORT="$(( nginx_port + 1 ))" NCS_HTTPS_PORT="${nginx_port}"
    export NCS_STATIC_ROOT="${static_root}" NCS_API_UPSTREAM="${http_addr}" NCS_METRICS_UPSTREAM="${api_metrics_addr}"
    export NCS_TLS_CERT="${run_dir}/certs/ncs.crt" NCS_TLS_KEY="${run_dir}/certs/ncs.key"
    export NCS_GATEWAY_ALLOW="127.0.0.1" NCS_OPS_ALLOW="127.0.0.1"
    export NCS_ACCESS_LOG="${run_dir}/nginx-prefix/logs/access.log" NCS_ERROR_LOG="${run_dir}/nginx-prefix/logs/error.log"
    bash "${script_dir}/nginx-render.sh" --output "${run_dir}/site.conf" >/dev/null
    cat > "${run_dir}/nginx.conf" <<CONF
worker_processes 1;
error_log ${run_dir}/nginx-prefix/logs/error.log warn;
pid ${run_dir}/nginx-prefix/logs/nginx.pid;
events { worker_connections 128; }
http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;
    client_body_temp_path ${run_dir}/nginx-prefix/temp/client;
    proxy_temp_path ${run_dir}/nginx-prefix/temp/proxy;
    fastcgi_temp_path ${run_dir}/nginx-prefix/temp/fastcgi;
    uwsgi_temp_path ${run_dir}/nginx-prefix/temp/uwsgi;
    scgi_temp_path ${run_dir}/nginx-prefix/temp/scgi;
    access_log ${run_dir}/nginx-prefix/logs/access.log;
    include ${run_dir}/site.conf;
}
CONF
    nginx -t -c "${run_dir}/nginx.conf" -p "${run_dir}/nginx-prefix" || fail "the rendered nginx site does not validate"
    nginx -c "${run_dir}/nginx.conf" -p "${run_dir}/nginx-prefix"
    sleep 0.5
    static_status="$(curl -sk --resolve localhost:${nginx_port}:127.0.0.1 -o /dev/null -w '%{http_code}' "https://localhost:${nginx_port}/")"
    proxy_status="$(curl -sk --resolve localhost:${nginx_port}:127.0.0.1 -o /dev/null -w '%{http_code}' "https://localhost:${nginx_port}/healthz")"
    echo "nginx: static=${static_status} /healthz via proxy=${proxy_status}"
    [[ "${proxy_status}" == "200" ]] || fail "the API is not reachable through nginx"
fi

cat <<INFO

=== local stack is up ===

API          ${api_url}
metrics      ${api_url}/metrics
readyz       ${api_url}/readyz
mock gateway http://127.0.0.1:${gateway_port}   (NCS_CHARGER_GATEWAY_URL for the worker)
gateway-ip   NCS_CHARGER_GATEWAY_URL is set; the internal callback needs the token below
gw token     ${gateway_token}
logs         ${run_dir}/*.log
INFO
if [[ "${with_nginx}" == "true" ]]; then
    echo "nginx        https://localhost:${nginx_port}/  (static root ${static_root})"
fi
cat <<'INFO'

handy commands:
  backend/scripts/loadtest-orders.sh          # concurrency and duplicate-submission load
  backend/scripts/fault-drill.sh              # dependency outages and readiness
  backend/scripts/verify-closed-loop.sh       # the full order chain with real receipts
  backend/scripts/drill-backup-restore.sh     # measured RPO/RTO

Press Ctrl-C to stop the stack.
INFO

wait
