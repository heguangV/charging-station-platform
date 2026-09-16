#!/usr/bin/env bash
set -euo pipefail

fixture_binary="$1"
migrator_binary="$2"
migrations_directory="$3"

source "$(dirname "${BASH_SOURCE[0]}")/postgres_test_tools.sh"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/ncs-sqlite-pg-migration.XXXXXX")"
data_dir="${work_dir}/pgdata"
socket_dir="${work_dir}/socket"
sqlite_path="${work_dir}/source.db"
port="${NCS_PG_TEST_PORT:-55440}"
mkdir -p "${socket_dir}"

cleanup() {
    if [[ -f "${data_dir}/postmaster.pid" ]]; then
        "${pg_bin}/pg_ctl" -D "${data_dir}" -m immediate stop >/dev/null 2>&1 || true
    fi
    rm -rf -- "${work_dir}"
}
trap cleanup EXIT

"${pg_bin}/initdb" -D "${data_dir}" --auth=trust --encoding=UTF8 --no-locale >/dev/null
"${pg_bin}/pg_ctl" -D "${data_dir}" -o "-h 127.0.0.1 -k ${socket_dir} -p ${port}" \
    -l "${work_dir}/postgres.log" start >/dev/null
"${pg_bin}/createdb" -h 127.0.0.1 -p "${port}" ncs_migration
"${pg_bin}/createdb" -h 127.0.0.1 -p "${port}" ncs_migration_rollback

"${fixture_binary}" "${sqlite_path}" --add-user
NCS_DATABASE_HOST=127.0.0.1 \
NCS_DATABASE_PORT="${port}" \
NCS_DATABASE_NAME=ncs_migration \
NCS_DATABASE_USER="$(id -un)" \
NCS_DATABASE_SSLMODE=disable \
NCS_DATABASE_MIGRATIONS="${migrations_directory}" \
"${migrator_binary}" --sqlite "${sqlite_path}" --confirm-fresh-target

"${pg_bin}/psql" -h 127.0.0.1 -p "${port}" -d ncs_migration -v ON_ERROR_STOP=1 -Atqc \
    "SELECT CASE WHEN
       (SELECT COUNT(*) FROM user_account)=301 AND
       (SELECT COUNT(*) FROM station)=5 AND
       (SELECT COUNT(*) FROM charger)=48 AND
       (SELECT COUNT(*) FROM charging_order)>8000 AND
       (SELECT checksum FROM schema_version WHERE version=10)='ncs-v10-order-confirmation'
     THEN 'ok' ELSE 'bad' END" | grep -qx ok

# A migrated target must never be overwritten, even when all users are demo users.
if NCS_DATABASE_HOST=127.0.0.1 \
    NCS_DATABASE_PORT="${port}" \
    NCS_DATABASE_NAME=ncs_migration \
    NCS_DATABASE_USER="$(id -un)" \
    NCS_DATABASE_SSLMODE=disable \
    NCS_DATABASE_MIGRATIONS="${migrations_directory}" \
    "${migrator_binary}" --sqlite "${sqlite_path}" --confirm-fresh-target; then
    echo "migration unexpectedly overwrote a non-fresh target" >&2
    exit 9
fi

# A row rejected by PostgreSQL must roll back schema creation too, leaving the target empty.
invalid_sqlite="${work_dir}/invalid-source.db"
"${fixture_binary}" "${invalid_sqlite}" --pg-constraint-violation
if NCS_DATABASE_HOST=127.0.0.1 \
    NCS_DATABASE_PORT="${port}" \
    NCS_DATABASE_NAME=ncs_migration_rollback \
    NCS_DATABASE_USER="$(id -un)" \
    NCS_DATABASE_SSLMODE=disable \
    NCS_DATABASE_MIGRATIONS="${migrations_directory}" \
    "${migrator_binary}" --sqlite "${invalid_sqlite}" --confirm-fresh-target; then
    echo "migration unexpectedly accepted a PostgreSQL constraint violation" >&2
    exit 10
fi
"${pg_bin}/psql" -h 127.0.0.1 -p "${port}" -d ncs_migration_rollback \
    -v ON_ERROR_STOP=1 -Atqc "SELECT COUNT(*) FROM pg_tables WHERE schemaname='public'" | grep -qx 0

# Identity values must advance beyond the imported IDs.
"${pg_bin}/psql" -h 127.0.0.1 -p "${port}" -d ncs_migration -v ON_ERROR_STOP=1 -Atqc \
    "SELECT nextval(pg_get_serial_sequence('user_account','id')) > 301" | grep -qx t

# Refuse even unrelated existing objects, without touching their data.
"${pg_bin}/psql" -h 127.0.0.1 -p "${port}" -d ncs_migration_rollback -v ON_ERROR_STOP=1 -qc \
    "CREATE TABLE sentinel(value integer); INSERT INTO sentinel VALUES(42)"
if NCS_DATABASE_HOST=127.0.0.1 NCS_DATABASE_PORT="${port}" \
    NCS_DATABASE_NAME=ncs_migration_rollback NCS_DATABASE_USER="$(id -un)" \
    NCS_DATABASE_SSLMODE=disable NCS_DATABASE_MIGRATIONS="${migrations_directory}" \
    "${migrator_binary}" --sqlite "${sqlite_path}" --confirm-fresh-target; then
    echo "migration unexpectedly accepted a nonempty database" >&2
    exit 11
fi
"${pg_bin}/psql" -h 127.0.0.1 -p "${port}" -d ncs_migration_rollback \
    -v ON_ERROR_STOP=1 -Atqc "SELECT value FROM sentinel" | grep -qx 42
