#!/usr/bin/env bash
#
# Closed-loop verification for BE-I-01.
#
# It starts the four real processes (API, outbox publisher, worker, mock charger gateway)
# against a disposable PostgreSQL and Redis, drives a business flow through the HTTP API, and
# asserts the result in the database, in Redis and in the logs. Everything it checks is a
# property the module claims: the outbox is published, the worker consumes and applies, the
# device outcome comes back through the gateway and closes the command, the consumption records
# are terminal, and the entries are acknowledged.
#
# This script is DESTRUCTIVE by design: it completes every active order, resets the chargers it
# uses and deletes whole streams in the Redis database it is pointed at. One wrong DSN would then
# clean up data that is not disposable, so it refuses to run unless the caller confirms it and the
# targets look like a test environment:
#
#   NCS_E2E_ALLOW_DESTRUCTIVE=true   required, no default
#   NCS_POSTGRES_DSN=...             database name must contain test/dev/ci/scratch, or be named in
#                                    NCS_E2E_ALLOWED_DATABASE
#   NCS_E2E_ALLOWED_DATABASE=<name>  explicit opt-in for a differently named test database
#   NCS_REDIS_ADDR=127.0.0.1:6379    default
#   NCS_REDIS_DB=14                  default; database 0 is refused
#
# The charger gateway service token is generated per run: the API refuses to start without one,
# because the receipt endpoint advances orders and starts billing.
#
# Usage:  NCS_E2E_ALLOW_DESTRUCTIVE=true NCS_POSTGRES_DSN=... backend/scripts/verify-closed-loop.sh
set -euo pipefail

backend_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${NCS_POSTGRES_DSN:?set NCS_POSTGRES_DSN to a disposable database}"
redis_addr="${NCS_REDIS_ADDR:-127.0.0.1:6379}"
redis_db="${NCS_REDIS_DB:-14}"

# --- destructive-operation gate -----------------------------------------------------------------
if [[ "${NCS_E2E_ALLOW_DESTRUCTIVE:-}" != "true" ]]; then
    cat >&2 <<'GATE'
refusing to run: this script completes active orders, resets chargers and deletes Redis streams.

Confirm you are pointing it at a disposable environment:
  NCS_E2E_ALLOW_DESTRUCTIVE=true NCS_POSTGRES_DSN=postgres://.../ncs_test \
      backend/scripts/verify-closed-loop.sh
GATE
    exit 2
fi

# The shared Redis default database is where every other tool keeps its data.
if [[ "${redis_db}" == "0" ]]; then
    echo "refusing to run: NCS_REDIS_DB=0 is the shared default database; use a dedicated test database" >&2
    exit 2
fi

# The database name must look disposable, so a production DSN cannot be cleaned up by accident.
database_name="$(python3 - "$NCS_POSTGRES_DSN" <<'READDB'
import sys, urllib.parse
parsed = urllib.parse.urlparse(sys.argv[1])
print(parsed.path.lstrip('/') or '')
READDB
)"
if [[ -z "${database_name}" ]]; then
    echo "refusing to run: could not read a database name from NCS_POSTGRES_DSN" >&2
    exit 2
fi
if [[ -n "${NCS_E2E_ALLOWED_DATABASE:-}" ]]; then
    if [[ "${database_name}" != "${NCS_E2E_ALLOWED_DATABASE}" ]]; then
        echo "refusing to run: database ${database_name} is not NCS_E2E_ALLOWED_DATABASE=${NCS_E2E_ALLOWED_DATABASE}" >&2
        exit 2
    fi
else
    case "${database_name}" in
        *test*|*dev*|*ci*|*scratch*|*sandbox*) ;;
        *)
            echo "refusing to run: database \"${database_name}\" does not look like a test database." >&2
            echo "if it really is disposable, name it explicitly:" >&2
            echo "  NCS_E2E_ALLOWED_DATABASE=${database_name} NCS_E2E_ALLOW_DESTRUCTIVE=true ..." >&2
            exit 2
            ;;
    esac
fi
echo "destructive run confirmed: database=${database_name} redis_db=${redis_db}"

work_dir="$(mktemp -d)"
api_port="$(python3 -c 'import socket
s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
gateway_port="$(python3 -c 'import socket
s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
api_url="http://127.0.0.1:${api_port}"

pids=()
cleanup() {
    local status=$?
    for pid in "${pids[@]:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    if [[ ${status} -ne 0 ]]; then
        echo "--- logs (${work_dir}) ---" >&2
        for log in "${work_dir}"/*.log; do
            [[ -f "${log}" ]] || continue
            echo "--- ${log} ---" >&2
            tail -40 "${log}" >&2 || true
        done
    else
        rm -rf "${work_dir}"
    fi
    exit "${status}"
}
trap cleanup EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
step() { printf '\n=== %s ===\n' "$*"; }

psql_dsn="${NCS_POSTGRES_DSN}"
psql_q() { psql "${psql_dsn}" -tAc "$1"; }
# psql_fields splits a multi-column row on spaces instead of the default "|", so `read` can take it.
psql_fields() { psql "${psql_dsn}" -tA -F' ' -c "$1"; }

step "build"
(cd "${backend_dir}" && go build -o "${work_dir}/ncs-api" ./cmd/api \
    && go build -o "${work_dir}/ncs-worker" ./cmd/worker \
    && go build -o "${work_dir}/ncs-outbox-publisher" ./cmd/outbox-publisher \
    && go build -o "${work_dir}/ncs-mock-gateway" ./cmd/mock-gateway)

run_marker="$(python3 -c 'import time; print(time.time_ns())')"
gateway_token="e2e-gateway-token-${run_marker}"
step "prepare the disposable database"
NCS_POSTGRES_DSN="${psql_dsn}" NCS_CHARGER_GATEWAY_TOKEN="${gateway_token}" \
    "${work_dir}/ncs-api" --migrate-only
psql "${psql_dsn}" -v ON_ERROR_STOP=1 -q -f "${backend_dir}/seeds/dev_seed.sql"
# Two chargers: one carries the order flow, the other one carries the device command. They must be
# different, because a charger held by an active order refuses a restart command - that guard is
# deliberate, so the verification uses a charger that is genuinely free for the command.
# The seeded station carries two chargers (the order flow and the successful command); the failure
# path uses any other idle charger, because the command flow is not tied to a station.
read -r charger_id command_charger_id <<<"$(psql_q "SELECT string_agg(t.id::text, ' ' ORDER BY t.id) FROM (SELECT c.id AS id FROM chargers c JOIN stations s ON s.id = c.station_id WHERE s.code = 'ST-DEV-01' ORDER BY c.id LIMIT 2) t")"
failing_charger_id="$(psql_q "SELECT id FROM chargers WHERE id NOT IN (${charger_id:-0}, ${command_charger_id:-0}) ORDER BY id LIMIT 1")"
[[ -n "${charger_id}" && -n "${command_charger_id}" && -n "${failing_charger_id}" ]] || fail "expected at least three chargers in the database"
[[ "${charger_id}" != "${command_charger_id}" && "${command_charger_id}" != "${failing_charger_id}" && "${charger_id}" != "${failing_charger_id}" ]] || fail "expected three distinct chargers"
echo "order charger: ${charger_id}, command charger: ${command_charger_id}, failing charger: ${failing_charger_id}"

# The loop is asserted from scratch: the database keeps rows from earlier runs, and the order
# rules allow one active flow per user, so a leftover order would make this run fail for a
# reason that has nothing to do with the loop.
psql "${psql_dsn}" -q -c "UPDATE charging_orders SET status = 'COMPLETED', payment_status = 'PAID' WHERE status IN ('CREATED','STARTING','CHARGING','STOPPING')"
# Leftover pending bills are cleared as well: an unsettled order blocks its user from starting a new
# flow, which would make this run fail for a reason that has nothing to do with the loop.
psql "${psql_dsn}" -q -c "UPDATE charging_orders SET payment_status = 'PAID', paid_cents = amount_cents WHERE payment_status = 'PENDING'"
psql "${psql_dsn}" -q -c "UPDATE chargers SET status = 'IDLE' WHERE id IN (${charger_id}, ${command_charger_id}, ${failing_charger_id})"

redis_cli=(redis-cli -h "${redis_addr%:*}" -p "${redis_addr##*:}" -n "${redis_db}")
# The streams are reset so the run is self-contained: a parked entry from an earlier run would
# otherwise be read as a failure of this one. What must be empty is the dead-letter stream at the
# END of this run, which is asserted below.
leftover_dead_letters="$("${redis_cli[@]}" xlen ncs:stream:dead-letter 2>/dev/null || echo 0)"
"${redis_cli[@]}" del ncs:stream:dead-letter >/dev/null
"${redis_cli[@]}" del ncs:stream:charge-event ncs:stream:order-event ncs:stream:charger-command >/dev/null
echo "cleared ${leftover_dead_letters} dead letter(s) left by earlier runs"

common_env=(
    "NCS_POSTGRES_DSN=${psql_dsn}"
    "NCS_REDIS_ADDR=${redis_addr}"
    "NCS_REDIS_DB=${redis_db}"
    "NCS_REDIS_REQUIRED=true"
    "NCS_METRICS_ADDR=127.0.0.1:0"
)

step "start the mock gateway, the API, the publisher and the worker"
# The third charger is configured to answer FAILED, which is how the failure outcome is driven
# through the real chain rather than asserted on a fake.
env "${common_env[@]}" NCS_MOCK_GATEWAY_ADDR="127.0.0.1:${gateway_port}" \
    NCS_MOCK_GATEWAY_FAILING_CHARGERS="${failing_charger_id}" \
    "${work_dir}/ncs-mock-gateway" >"${work_dir}/gateway.log" 2>&1 &
pids+=("$!")

env "${common_env[@]}" NCS_HTTP_ADDR="127.0.0.1:${api_port}" NCS_SMS_MOCK=true \
    NCS_CHARGER_GATEWAY_TOKEN="${gateway_token}" \
    "${work_dir}/ncs-api" >"${work_dir}/api.log" 2>&1 &
pids+=("$!")

env "${common_env[@]}" NCS_OUTBOX_INTERVAL=100ms NCS_OUTBOX_BATCH=50 \
    "${work_dir}/ncs-outbox-publisher" >"${work_dir}/publisher.log" 2>&1 &
pids+=("$!")

env "${common_env[@]}" NCS_WORKER_CONSUMER="worker-e2e" NCS_WORKER_SAMPLE_INTERVAL=1s \
    NCS_CHARGER_GATEWAY_URL="http://127.0.0.1:${gateway_port}" \
    NCS_CHARGER_GATEWAY_TIMEOUT=5s \
    "${work_dir}/ncs-worker" >"${work_dir}/worker.log" 2>&1 &
worker_pid=$!
pids+=("${worker_pid}")

for _ in $(seq 1 100); do
    if curl -fsS "${api_url}/healthz" >/dev/null 2>&1; then break; fi
    sleep 0.1
done
curl -fsS "${api_url}/healthz" >/dev/null || fail "the API did not become healthy"

# API readiness does not imply the separately launched worker is ready.
for _ in $(seq 1 100); do
    if grep -q "command dispatcher configured" "${work_dir}/worker.log"; then break; fi
    kill -0 "${worker_pid}" 2>/dev/null || fail "the worker exited during startup"
    sleep 0.1
done

# The worker must not have fallen back to a placeholder: the memory consumption store is gone.
if grep -q "in-memory consumption store" "${work_dir}/worker.log"; then
    fail "the worker is still using the in-memory consumption store"
fi
if grep -q "domain adapters are not wired" "${work_dir}/worker.log"; then
    fail "the worker still refuses to start because its domain adapters are missing"
fi
grep -q "consumption store ready" "${work_dir}/worker.log" || fail "the worker did not report its consumption store"
grep -q "command dispatcher configured" "${work_dir}/worker.log" || fail "the worker did not configure the command dispatcher"

step "log in and create an order through the API"
login_body="$(curl -fsS -X POST "${api_url}/api/v1/auth/user/login" \
    -H 'Content-Type: application/json' \
    -d '{"account":"13800000001","password":"Dev-Password-01"}')"
token="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["accessToken"])' <<<"${login_body}")"
[[ -n "${token}" ]] || fail "no access token in ${login_body}"

order_body="$(curl -fsS -X POST "${api_url}/api/v1/orders" \
    -H 'Content-Type: application/json' -H "Authorization: Bearer ${token}" \
    -H "Idempotency-Key: e2e-order-${run_marker}" \
    -d "{\"chargerId\": ${charger_id}}")"
order_no="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["orderNo"])' <<<"${order_body}")"
[[ -n "${order_no}" ]] || fail "no order number in ${order_body}"
echo "order created: ${order_no}"

step "assert the order event reached the worker through the outbox and the stream"
for _ in $(seq 1 100); do
    consumed="$(psql_q "SELECT count(*) FROM event_consumptions WHERE aggregate_id = '${order_no}' AND outcome IN ('SUCCEEDED','DEAD_LETTERED')")"
    [[ "${consumed}" -ge 1 ]] && break
    sleep 0.1
done
consumed="$(psql_q "SELECT count(*) FROM event_consumptions WHERE aggregate_id = '${order_no}' AND outcome = 'SUCCEEDED'")"
[[ "${consumed}" -ge 1 ]] || fail "no ORDER_CREATED consumption record reached a terminal state"
published="$(psql_q "SELECT count(*) FROM outbox_events WHERE aggregate_id = '${order_no}' AND published_at IS NULL")"
[[ "${published}" -eq 0 ]] || fail "${published} outbox rows for ${order_no} were never published"

step "start charging and consume the lifecycle events"
curl -fsS -X POST "${api_url}/api/v1/orders/${order_no}/start" \
    -H "Authorization: Bearer ${token}" -H "Idempotency-Key: e2e-start-${run_marker}" >/dev/null
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no}'")"
[[ "${status}" == "STARTING" ]] || fail "expected the order to be STARTING after the start request, got ${status}"

# The order lifecycle events are consumed as notifications: the transaction that produced them
# already committed the state change, so the worker must not apply it a second time. The
# verification asserts both halves of that - the events reach a terminal consumption state, and
# the order is still in the state the API put it in.
for _ in $(seq 1 100); do
    consumed="$(psql_q "SELECT count(*) FROM event_consumptions WHERE aggregate_id = '${order_no}' AND outcome = 'SUCCEEDED'")"
    [[ "${consumed}" -ge 2 ]] && break
    sleep 0.1
done
consumed="$(psql_q "SELECT count(*) FROM event_consumptions WHERE aggregate_id = '${order_no}' AND outcome = 'SUCCEEDED'")"
[[ "${consumed}" -ge 2 ]] || fail "expected the order events to be consumed, got ${consumed}"
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no}'")"
[[ "${status}" == "STARTING" ]] || fail "a notification must not change the order state, got ${status}"

step "the order state machine stays the authority (negative check)"
# A second start on the same order must be refused by the domain rather than silently applied by
# the worker.
second_status="$(curl -s -o /dev/null -w '%{http_code}' -X POST "${api_url}/api/v1/orders/${order_no}/start" \
    -H "Authorization: Bearer ${token}" -H "Idempotency-Key: e2e-start-again-${run_marker}")"
[[ "${second_status}" == "409" ]] || fail "expected a repeated start to be refused with 409, got ${second_status}"

# Cancelling a STARTING order is the furthest the state machine can be driven over HTTP without a
# device receipt: it moves to CANCELLED and queues the revocation. The states after it
# (CHARGING -> STOPPING -> COMPLETED) are applied by the device receipt, whose contract BE-I-02
# owns, so this script does NOT claim to verify them.
curl -fsS -X POST "${api_url}/api/v1/orders/${order_no}/cancel" \
    -H "Authorization: Bearer ${token}" -H "Idempotency-Key: e2e-cancel-${run_marker}" >/dev/null
for _ in $(seq 1 100); do
    status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no}'")"
    [[ "${status}" == "CANCELLED" ]] && break
    sleep 0.1
done
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no}'")"
[[ "${status}" == "CANCELLED" ]] || fail "expected the cancel to move the order to CANCELLED, got ${status}"
echo "order state machine verified over HTTP: CREATED -> STARTING -> CANCELLED"

# The charger of the cancelled order is released only after the device confirms the revocation: a
# STARTING order that was cancelled may physically still be charging, so the platform keeps the
# device occupied until the reversal is acknowledged. The chain below needs it back.
for _ in $(seq 1 150); do
    released="$(psql_q "SELECT status FROM chargers WHERE id = ${charger_id}")"
    [[ "${released}" == "IDLE" ]] && break
    sleep 0.1
done
released="$(psql_q "SELECT status FROM chargers WHERE id = ${charger_id}")"
[[ "${released}" == "IDLE" ]] || fail "charger ${charger_id} was never released after the revocation, got ${released}"
echo "charger ${charger_id} released after the device confirmed the revocation"

step "drive the whole order chain with real device receipts"
# BE-I-02: the transitions after STARTING are applied by the device's receipts, so this step drives
# them through the internal endpoint the charger gateway calls, with a fresh order (the previous one
# ended in CANCELLED, which is terminal).
chain_body="$(curl -fsS -X POST "${api_url}/api/v1/orders" \
    -H 'Content-Type: application/json' -H "Authorization: Bearer ${token}" \
    -H "Idempotency-Key: e2e-chain-order-${run_marker}" \
    -d "{\"chargerId\": ${charger_id}}")"
order_no_chain="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["orderNo"])' <<<"${chain_body}")"
[[ -n "${order_no_chain}" ]] || fail "no order number in ${chain_body}"
echo "order created for the receipt chain: ${order_no_chain}"

curl -fsS -X POST "${api_url}/api/v1/orders/${order_no_chain}/start" \
    -H "Authorization: Bearer ${token}" -H "Idempotency-Key: e2e-chain-start-${run_marker}" >/dev/null
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${status}" == "STARTING" ]] || fail "expected the chain order to be STARTING, got ${status}"

# The device accepts the start command, and accepting it must not move the order: only the receipt
# reports what physically happened.
for _ in $(seq 1 150); do
    grep -q '"action":"START_CHARGING"' "${work_dir}/gateway.log" && break
    sleep 0.1
done
grep -q '"action":"START_CHARGING"' "${work_dir}/gateway.log" || fail "the start command never reached the device through the gateway"
# An accepted command changes no row and therefore emits no completion event: the state is the
# device's business until the receipt reports what actually happened.
accepted_events="$(psql_q "SELECT count(*) FROM outbox_events WHERE event_type = 'CHARGER_COMMAND_COMPLETED' AND aggregate_id = '${charger_id}' AND payload->>'action' = 'START_CHARGING'")"
[[ "${accepted_events}" == "0" ]] || fail "an accepted charge command emitted ${accepted_events} completion event(s); it changes no state"
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${status}" == "STARTING" ]] || fail "an accepted command must not advance the order, got ${status}"
echo "the device accepted START_CHARGING and the order is still STARTING"

# The receipt endpoint is authenticated: without the service token nothing may reach it.
unauthenticated="$(curl -s -o /dev/null -w '%{http_code}' -X POST "${api_url}/api/v1/internal/charger-events" \
    -H 'Content-Type: application/json' \
    -d '{"eventId":"evt_unauthorized_'"${run_marker}"'","eventType":"CHARGE_STARTED","orderNo":"'"${order_no_chain}"'","chargerId":'"${charger_id}"',"occurredAt":"'"$(date -u +%Y-%m-%dT%H:%M:%SZ)"'"}')"
[[ "${unauthenticated}" == "401" ]] || fail "expected an unauthenticated receipt to be refused with 401, got ${unauthenticated}"

# The fact times sit inside the seeded charger's off-peak window (23:00-07:00 in NCS_BILLING_TZ), so the bill
# proves that the device's own timestamps - not the server clock - picked the tariff window. The
# window is entered by an explicit past hour, and both ends stay inside one hour, so no segment is
# split across the boundary.
read -r fact_start fact_stop <<<"$(python3 - <<'PYFACT'
from datetime import datetime, timedelta, timezone
import os
from zoneinfo import ZoneInfo
zone = ZoneInfo(os.environ.get("NCS_BILLING_TZ", "Asia/Shanghai"))
now = datetime.now(zone).replace(minute=0, second=0, microsecond=0)
window = {23, 0, 1, 2, 3, 4, 5, 6}
moment = now - timedelta(hours=2)
while moment.hour not in window:
    moment -= timedelta(hours=1)
start = moment + timedelta(minutes=10)
print(start.astimezone(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), (start + timedelta(minutes=30)).astimezone(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))
PYFACT
)"
[[ -n "${fact_start}" && -n "${fact_stop}" ]] || fail "could not derive the device fact times"

start_receipt() {
    curl -fsS -X POST "${api_url}/api/v1/internal/charger-events" \
        -H 'Content-Type: application/json' -H "Authorization: Bearer ${gateway_token}" \
        -d "$1"
}
started_receipt="{\"eventId\":\"evt_chain_start_${run_marker}\",\"eventType\":\"CHARGE_STARTED\",\"orderNo\":\"${order_no_chain}\",\"chargerId\":${charger_id},\"occurredAt\":\"${fact_start}\",\"traceId\":\"trace-chain-${run_marker}\"}"
started_body="$(start_receipt "${started_receipt}")"
started_state="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["status"])' <<<"${started_body}")"
[[ "${started_state}" == "CHARGING" ]] || fail "the start receipt did not move the order to CHARGING: ${started_body}"
started_at="$(psql_q "SELECT to_char(started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${started_at}" == "${fact_start}" ]] || fail "started_at = ${started_at}, want the device fact time ${fact_start}"
echo "CHARGE_STARTED applied: status=${started_state}, started_at=${started_at} (the device's own fact time)"

# A duplicate receipt is the same fact: it must be answered with the first result and applied once.
started_again="$(start_receipt "${started_receipt}")"
started_again_state="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["status"])' <<<"${started_again}")"
[[ "${started_again_state}" == "CHARGING" ]] || fail "a duplicate receipt must replay the first result: ${started_again}"
start_events="$(psql_q "SELECT count(*) FROM outbox_events WHERE event_type = 'CHARGE_STARTED' AND aggregate_id = '${order_no_chain}'")"
[[ "${start_events}" == "1" ]] || fail "a duplicate receipt produced ${start_events} CHARGE_STARTED events, want 1"

# The same receipt id with a different payload is a different fact under a used id: refused, not
# replayed and not applied.
conflict_body="{\"eventId\":\"evt_chain_start_${run_marker}\",\"eventType\":\"CHARGE_STARTED\",\"orderNo\":\"${order_no_chain}\",\"chargerId\":${charger_id},\"occurredAt\":\"${fact_stop}\",\"traceId\":\"trace-chain-${run_marker}\"}"
conflict_status="$(curl -s -o /dev/null -w '%{http_code}' -X POST "${api_url}/api/v1/internal/charger-events" \
    -H 'Content-Type: application/json' -H "Authorization: Bearer ${gateway_token}" -d "${conflict_body}")"
[[ "${conflict_status}" == "409" ]] || fail "expected a reused receipt id with a different fact to conflict, got ${conflict_status}"

# Stopping: the command again does not move the order, the receipt does.
curl -fsS -X POST "${api_url}/api/v1/orders/${order_no_chain}/stop" \
    -H "Authorization: Bearer ${token}" -H "Idempotency-Key: e2e-chain-stop-${run_marker}" >/dev/null
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${status}" == "STOPPING" ]] || fail "expected the chain order to be STOPPING, got ${status}"
for _ in $(seq 1 150); do
    grep -q '"action":"STOP_CHARGING"' "${work_dir}/gateway.log" && break
    sleep 0.1
done
grep -q '"action":"STOP_CHARGING"' "${work_dir}/gateway.log" || fail "the stop command never reached the device through the gateway"
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${status}" == "STOPPING" ]] || fail "an accepted stop command must not complete the order, got ${status}"
echo "the device accepted STOP_CHARGING and the order is still STOPPING"

stopped_receipt="{\"eventId\":\"evt_chain_stop_${run_marker}\",\"eventType\":\"CHARGE_STOPPED\",\"orderNo\":\"${order_no_chain}\",\"chargerId\":${charger_id},\"occurredAt\":\"${fact_stop}\",\"energyWh\":1000,\"meterStartWh\":0,\"meterEndWh\":1000,\"traceId\":\"trace-chain-${run_marker}\"}"
stopped_body="$(start_receipt "${stopped_receipt}")"
stopped_state="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["status"])' <<<"${stopped_body}")"
[[ "${stopped_state}" == "COMPLETED" ]] || fail "the stop receipt did not complete the order: ${stopped_body}"

read -r stopped_at amount_cents energy_wh payment_status charger_status <<<"$(psql_fields "SELECT to_char(o.stopped_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), o.amount_cents, o.energy_wh, o.payment_status, c.status FROM charging_orders o JOIN chargers c ON c.id = o.charger_id WHERE o.order_no = '${order_no_chain}'")"
[[ "${stopped_at}" == "${fact_stop}" ]] || fail "stopped_at = ${stopped_at}, want the device fact time ${fact_stop}"
[[ "${energy_wh}" == "1000" ]] || fail "energy_wh = ${energy_wh}, want the metered 1000"
[[ "${charger_status}" == "IDLE" ]] || fail "the charger was not released after the device reported the stop, got ${charger_status}"
# Completing a charge freezes the bill and waits for the user's confirmation (UC-U-09): the
# platform must not collect the money on its own.
[[ "${payment_status}" == "PENDING" ]] || fail "payment_status = ${payment_status}, want PENDING until the user confirms"

# 1 kWh billed in the off-peak window: the snapshot prices come from the order the start
# transaction froze, and the fact time is what decides that the cheap window applies.
read -r off_peak_price peak_price service_price <<<"$(psql_fields "SELECT off_peak_price_per_kwh_cents, price_per_kwh_cents, service_price_per_kwh_cents FROM charging_orders WHERE order_no = '${order_no_chain}'")"
expected_amount=$(( off_peak_price + service_price ))
[[ "${amount_cents}" == "${expected_amount}" ]] || fail "amount = ${amount_cents} cents, want ${expected_amount} (off-peak ${off_peak_price} + service ${service_price}) for the fact time inside the off-peak window"
echo "CHARGE_STOPPED applied: status=${stopped_state}, stopped_at=${stopped_at}, energy=${energy_wh}Wh, amount=${amount_cents} cents (off-peak ${off_peak_price} + service ${service_price}), charger=${charger_status}"

# A duplicated stop must not bill the charge twice.
start_receipt "${stopped_receipt}" >/dev/null
after_amount="$(psql_q "SELECT amount_cents FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${after_amount}" == "${amount_cents}" ]] || fail "a duplicate stop receipt changed the bill from ${amount_cents} to ${after_amount}"
complete_events="$(psql_q "SELECT count(*) FROM outbox_events WHERE event_type = 'ORDER_COMPLETED' AND aggregate_id = '${order_no_chain}'")"
[[ "${complete_events}" == "1" ]] || fail "a duplicate stop receipt produced ${complete_events} ORDER_COMPLETED events, want 1"

# The lifecycle events the receipts produced are consumed by the B-line worker as notifications.
for _ in $(seq 1 150); do
    chain_consumed="$(psql_q "SELECT count(DISTINCT event_type) FROM event_consumptions WHERE aggregate_id = '${order_no_chain}' AND outcome = 'SUCCEEDED'")"
    [[ "${chain_consumed}" -ge 4 ]] && break
    sleep 0.1
done
chain_consumed="$(psql_q "SELECT count(DISTINCT event_type) FROM event_consumptions WHERE aggregate_id = '${order_no_chain}' AND outcome = 'SUCCEEDED'")"
[[ "${chain_consumed}" -ge 4 ]] || fail "expected ORDER_CREATED, CHARGE_START_REQUESTED, CHARGE_STARTED, CHARGE_STOP_REQUESTED, CHARGE_STOPPED and ORDER_COMPLETED to be consumed, got ${chain_consumed} distinct types"
status="$(psql_q "SELECT status FROM charging_orders WHERE order_no = '${order_no_chain}'")"
[[ "${status}" == "COMPLETED" ]] || fail "the notification consumption changed the order state to ${status}"
echo "order chain verified with real receipts: CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED (${chain_consumed} event types consumed)"

step "close a device command through the gateway (real device outcome, real state writeback)"
admin_body="$(curl -fsS -X POST "${api_url}/api/v1/auth/admin/login" \
    -H 'Content-Type: application/json' \
    -d '{"account":"admin","password":"Dev-Password-01"}')"
admin_token="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["accessToken"])' <<<"${admin_body}")"

restart_body="$(curl -fsS -X POST "${api_url}/api/v1/admin/chargers/${command_charger_id}/restart" \
    -H 'Content-Type: application/json' -H "Authorization: Bearer ${admin_token}" \
    -H "Idempotency-Key: e2e-restart-${run_marker}" \
    -d '{"reason":"closed loop verification"}')"
command_no="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["commandId"])' <<<"${restart_body}")"
echo "device command issued: ${command_no}"

# The dispatcher reaches the mock gateway, the outcome is recorded with the completion event in one
# transaction, and the completion is consumed and applied - which releases the charger.
for _ in $(seq 1 150); do
    completion="$(psql_q "SELECT count(*) FROM event_consumptions e JOIN outbox_events o ON o.event_id = e.event_id WHERE e.event_type = 'CHARGER_COMMAND_COMPLETED' AND e.outcome = 'SUCCEEDED' AND o.payload->>'command_id' = '${command_no}'")"
    status="$(psql_q "SELECT status FROM chargers WHERE id = ${command_charger_id}")"
    if [[ "${completion}" -ge 1 && "${status}" == "IDLE" ]]; then break; fi
    sleep 0.1
done
completion="$(psql_q "SELECT count(*) FROM event_consumptions e JOIN outbox_events o ON o.event_id = e.event_id WHERE e.event_type = 'CHARGER_COMMAND_COMPLETED' AND e.outcome = 'SUCCEEDED' AND o.payload->>'command_id' = '${command_no}'")"
[[ "${completion}" -ge 1 ]] || fail "the command completion was never consumed"
status="$(psql_q "SELECT status FROM chargers WHERE id = ${command_charger_id}")"
[[ "${status}" == "IDLE" ]] || fail "expected the charger to be released after the command completed, got ${status}"
grep -q "device command handled" "${work_dir}/gateway.log" || fail "the mock gateway never answered a command"
echo "command ${command_no} completed and the charger was released"

step "a FAILED device outcome must not return the charger to IDLE"
# The review reproduced this with NCS_MOCK_GATEWAY_RESULT=FAILED and found the charger released as
# IDLE, which would put a possibly broken device back into the allocation pool. Here the failure is
# driven through the same four processes, on a charger the gateway is configured to fail.
failed_body="$(curl -fsS -X POST "${api_url}/api/v1/admin/chargers/${failing_charger_id}/restart" \
    -H 'Content-Type: application/json' -H "Authorization: Bearer ${admin_token}" \
    -H "Idempotency-Key: e2e-restart-failed-${run_marker}" \
    -d '{"reason":"failure path verification"}')"
failed_command_no="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["commandId"])' <<<"${failed_body}")"
echo "device command issued for the failing charger: ${failed_command_no}"

for _ in $(seq 1 150); do
    failed_completion="$(psql_q "SELECT payload->>'result' FROM outbox_events WHERE event_type = 'CHARGER_COMMAND_COMPLETED' AND payload->>'command_id' = '${failed_command_no}' ORDER BY id DESC LIMIT 1")"
    failed_status="$(psql_q "SELECT status FROM chargers WHERE id = ${failing_charger_id}")"
    consumed_failed="$(psql_q "SELECT count(*) FROM event_consumptions e JOIN outbox_events o ON o.event_id = e.event_id WHERE e.event_type = 'CHARGER_COMMAND_COMPLETED' AND e.outcome = 'SUCCEEDED' AND o.payload->>'command_id' = '${failed_command_no}'")"
    if [[ "${failed_completion}" == "FAILED" && "${failed_status}" == "FAULT" && "${consumed_failed}" -ge 1 ]]; then break; fi
    sleep 0.1
done
failed_completion="$(psql_q "SELECT payload->>'result' FROM outbox_events WHERE event_type = 'CHARGER_COMMAND_COMPLETED' AND payload->>'command_id' = '${failed_command_no}' ORDER BY id DESC LIMIT 1")"
failed_status="$(psql_q "SELECT status FROM chargers WHERE id = ${failing_charger_id}")"
[[ "${failed_completion}" == "FAILED" ]] || fail "expected a FAILED completion event for charger ${failing_charger_id}, got '${failed_completion}'"
[[ "${failed_status}" == "FAULT" ]] || fail "a restart the device reported as FAILED must park the charger in FAULT, got ${failed_status}"
consumed_failed="$(psql_q "SELECT count(*) FROM event_consumptions e JOIN outbox_events o ON o.event_id = e.event_id WHERE e.event_type = 'CHARGER_COMMAND_COMPLETED' AND e.outcome = 'SUCCEEDED' AND o.payload->>'command_id' = '${failed_command_no}'")"
[[ "${consumed_failed}" -ge 1 ]] || fail "the failure completion was never consumed"
echo "charger ${failing_charger_id}: event result=FAILED, status=${failed_status} (not IDLE), completion consumed"

step "assert the streams are drained and nothing was parked"
# Every entry delivered to this worker must be acknowledged: a pending entry would mean a
# delivery that was neither applied nor parked.
for stream_group in "order-event:order-event-workers" "charge-event:charge-event-workers" "charger-command:charger-command-workers"; do
    stream="${stream_group%%:*}"
    group="${stream_group##*:}"
    pending_count="$("${redis_cli[@]}" xpending "ncs:stream:${stream}" "${group}" 2>/dev/null | head -1)"
    pending_count="${pending_count:-0}"
    echo "stream ${stream}: ${pending_count} pending for ${group}"
    [[ "${pending_count}" == "0" ]] || fail "stream ${stream} still has ${pending_count} pending entries"
done
dead_letters="$("${redis_cli[@]}" xlen ncs:stream:dead-letter)"
[[ "${dead_letters}" == "0" ]] || fail "expected no dead letters, got ${dead_letters}"
# Scoped to this run: the shared database keeps records from other suites.
failed="$(psql_q "SELECT count(*) FROM event_consumptions WHERE outcome IN ('FAILED','DEAD_LETTERING','DEAD_LETTERED') AND (aggregate_id IN ('${order_no}', '${order_no_chain}') OR aggregate_id = '${command_charger_id}')")"
[[ "${failed}" == "0" ]] || fail "expected every consumption record for this run to be terminal-success, found ${failed} failed/parked"

step "concurrent publishers: only one may be active"
env "${common_env[@]}" NCS_OUTBOX_INTERVAL=100ms NCS_OUTBOX_BATCH=50 \
    "${work_dir}/ncs-outbox-publisher" >"${work_dir}/publisher-second.log" 2>&1 &
second_publisher_pid=$!
pids+=("${second_publisher_pid}")
for _ in $(seq 1 50); do
    grep -q "standing by without publishing" "${work_dir}/publisher-second.log" && break
    sleep 0.1
done
grep -q "standing by without publishing" "${work_dir}/publisher-second.log" \
    || fail "the second publisher did not stand by: $(tail -3 "${work_dir}/publisher-second.log")"
if grep -q "lock acquired; this process is publishing" "${work_dir}/publisher-second.log"; then
    fail "two publishers both acquired the advisory lock"
fi
echo "second publisher stood by; the advisory lock kept publishing single-active"

step "crash recovery: an entry delivered to a consumer that died is reclaimed"
# The worker is killed outright, and an entry is delivered to a consumer that never acknowledges
# it - which is exactly the state a crashed process leaves behind. The restarted worker must
# reclaim it, apply it once and acknowledge it.
kill -9 "${worker_pid}" 2>/dev/null || true
wait "${worker_pid}" 2>/dev/null || true
sleep 0.3

recovery_event_id="evt_recovery_${run_marker}"
"${redis_cli[@]}" xadd ncs:stream:charge-event '*' \
    event_id "${recovery_event_id}" event_type CHARGE_START_REQUESTED aggregate_type order \
    aggregate_id "ORD-RECOVERY-${run_marker}" occurred_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    trace_id "trace-recovery-${run_marker}" payload '{"userId":1}' >/dev/null
delivered="$("${redis_cli[@]}" xreadgroup group charge-event-workers dead-consumer count 1 streams ncs:stream:charge-event '>' | grep -c "${recovery_event_id}" || true)"
[[ "${delivered}" -ge 1 ]] || fail "could not create the pending entry the recovery test needs"
pending_before="$("${redis_cli[@]}" xpending ncs:stream:charge-event charge-event-workers | head -1)"
[[ "${pending_before}" -ge 1 ]] || fail "expected a pending entry before the restart"
echo "entry delivered to a consumer that died: pending=${pending_before}"

env "${common_env[@]}" NCS_WORKER_CONSUMER="worker-e2e" NCS_WORKER_SAMPLE_INTERVAL=1s \
    NCS_CHARGER_GATEWAY_URL="http://127.0.0.1:${gateway_port}" \
    NCS_CHARGER_GATEWAY_TIMEOUT=5s \
    "${work_dir}/ncs-worker" >"${work_dir}/worker-restarted.log" 2>&1 &
pids+=("$!")

for _ in $(seq 1 150); do
    recovered="$(psql_q "SELECT count(*) FROM event_consumptions WHERE event_id = '${recovery_event_id}' AND outcome = 'SUCCEEDED'")"
    [[ "${recovered}" -ge 1 ]] && break
    sleep 0.1
done
recovered="$(psql_q "SELECT count(*) FROM event_consumptions WHERE event_id = '${recovery_event_id}' AND outcome = 'SUCCEEDED'")"
[[ "${recovered}" -eq 1 ]] || fail "the restarted worker did not reclaim the pending entry (records: ${recovered})"
grep -q "recovered pending entries" "${work_dir}/worker-restarted.log" || fail "the worker did not report the recovery pass"
pending_after="$("${redis_cli[@]}" xpending ncs:stream:charge-event charge-event-workers | head -1)"
[[ "${pending_after}" == "0" ]] || fail "expected the reclaimed entry to be acknowledged, ${pending_after} still pending"
echo "entry reclaimed and acknowledged after the crash: records=${recovered}, pending=${pending_after}"

step "logs"
grep -c "published outbox rows" "${work_dir}/publisher.log" >/dev/null || fail "the publisher never reported a published batch"
echo "publisher batches: $(grep -c 'published outbox rows' "${work_dir}/publisher.log")"
echo "worker consumed:   $(cat "${work_dir}/worker.log" "${work_dir}/worker-restarted.log" | grep -c 'event consumed')"

echo
cat <<'SCOPE'
PASS: verified with real processes against real PostgreSQL and Redis -

  * event infrastructure: API -> PostgreSQL outbox -> Redis Stream -> worker -> consumption
    record -> ACK, with pending = 0 and no dead letters;
  * the complete P0 order chain driven by real device receipts through the internal endpoint:
    CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED, with the device's own fact time in
    started_at / stopped_at, the off-peak tariff window selected by that fact time, the charger
    released on the stop receipt, duplicate receipts replaying the first result instead of billing
    twice, and a reused receipt id with a different fact refused with 409;
  * the HTTP-only path beside it: a repeated start refused with 409, and
    CREATED -> STARTING -> CANCELLED;
  * the RESTART command loop end to end: admin command -> dispatcher -> gateway -> device outcome
    recorded with CHARGER_COMMAND_COMPLETED in one transaction -> consumed -> charger released;
  * both device outcomes: COMPLETED returns the charger to IDLE, FAILED parks it in FAULT;
  * single-active publishing, and recovery of an entry left behind by a consumer that died.

NOT verified here: the real charger gateway (Modbus/OCPP), payment and settlement, and the H5 line.
The device is the mock gateway this script starts, so the receipts it posts are the contract the real
gateway has to meet, not the gateway itself.
SCOPE
