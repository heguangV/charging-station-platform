#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_dir}/build/dev}"

QT_QPA_PLATFORM=offscreen "${build_dir}/apps/user/ncs_user" --smoke-test
QT_QPA_PLATFORM=offscreen "${build_dir}/apps/admin/ncs_admin" --smoke-test
"${build_dir}/server/ncs_server" --smoke-test
