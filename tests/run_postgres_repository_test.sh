#!/usr/bin/env bash

set -euo pipefail

test_executable=${1:?missing PostgreSQL repository test executable}
source_directory=${2:?missing source directory}

source "$(dirname "${BASH_SOURCE[0]}")/postgres_test_tools.sh"

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/ncs-postgres-test.XXXXXX")
data_dir=${work_dir}/data
log_file=${work_dir}/postgres.log
backup_dir=${work_dir}/backups
port=${NCS_TEST_POSTGRES_PORT:-55439}
started=false

cleanup() {
    if [[ ${started} == true ]]; then
        "${pg_bin}/pg_ctl" -D "${data_dir}" -m immediate stop >/dev/null 2>&1 || true
    fi
    rm -rf "${work_dir}"
}
trap cleanup EXIT

"${pg_bin}/initdb" -D "${data_dir}" --no-locale --encoding=UTF8 --auth=trust >/dev/null
"${pg_bin}/pg_ctl" -D "${data_dir}" -l "${log_file}" \
    -o "-h 127.0.0.1 -p ${port} -k ${work_dir}" start >/dev/null
started=true
"${pg_bin}/createdb" -h 127.0.0.1 -p "${port}" ncs_repository_test

"${test_executable}" 127.0.0.1 "${port}" ncs_repository_test "${USER}" \
    "${source_directory}/infrastructure/postgres/migrations" "${backup_dir}" \
    "${pg_bin}/pg_dump" "${pg_bin}/pg_restore"

# Read every archive data block through an actual isolated restore, not just --list.
archives=("${backup_dir}"/*.dump)
[[ ${#archives[@]} -eq 1 && -f "${archives[0]}" ]]
"${pg_bin}/createdb" -h 127.0.0.1 -p "${port}" ncs_restore_test
"${pg_bin}/pg_restore" --exit-on-error --no-owner --no-privileges \
    -h 127.0.0.1 -p "${port}" -d ncs_restore_test "${archives[0]}"
"${pg_bin}/psql" -h 127.0.0.1 -p "${port}" -d ncs_restore_test -v ON_ERROR_STOP=1 -Atqc \
    "SELECT (SELECT COUNT(*) FROM schema_version)=10 AND
            (SELECT COUNT(*) FROM charging_order)>8000 AND
            EXISTS(SELECT 1 FROM user_account u JOIN wallet_account w ON w.user_id=u.id
                   WHERE u.username='postgres_contract_user' AND w.balance_cent=u.balance_cent)
    " | grep -qx t
