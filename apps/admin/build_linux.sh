#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${NCS_BUILD_DIR:-$repo_dir/build/dev}"
cmake -S "$repo_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" --target ncs_admin --parallel 2
