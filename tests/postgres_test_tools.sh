#!/usr/bin/env bash
# Sourced by isolated PostgreSQL tests. CI must fail rather than silently skip.
missing_postgres() {
    echo "PostgreSQL 18 test prerequisite unavailable: $1" >&2
    if [[ "${NCS_REQUIRE_POSTGRES_TESTS:-0}" == 1 ]]; then
        exit 1
    fi
    exit 77
}

if [[ -n "${NCS_PG_BIN:-}" ]]; then
    pg_bin="${NCS_PG_BIN}"
elif command -v pg_config >/dev/null 2>&1; then
    pg_bin="$(pg_config --bindir)"
elif [[ -x /opt/homebrew/opt/postgresql@18/bin/initdb ]]; then
    pg_bin=/opt/homebrew/opt/postgresql@18/bin
else
    missing_postgres "server/client tools"
fi
for tool in initdb pg_ctl createdb pg_dump pg_restore psql; do
    [[ -x "${pg_bin}/${tool}" ]] || missing_postgres "${tool}"
    [[ "$("${pg_bin}/${tool}" --version)" == *"(PostgreSQL) 18."* ]] || \
        missing_postgres "${tool} must be version 18"
done
