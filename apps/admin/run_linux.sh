#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/build-linux/bin/ncs_admin"

if [[ ! -x "$APP" ]]; then
    echo "尚未构建管理端，请先运行 ./build_linux.sh"
    exit 1
fi

exec "$APP" "$@"
