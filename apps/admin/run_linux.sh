#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${NCS_BUILD_DIR:-$repo_dir/build/dev}"
exec "$build_dir/apps/admin/ncs_admin" "$@"
