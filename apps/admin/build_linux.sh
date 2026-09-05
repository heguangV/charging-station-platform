#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

QMAKE_BIN="${QMAKE:-qmake}"
BUILD_DIR="$SCRIPT_DIR/build-linux"

command -v "$QMAKE_BIN" >/dev/null 2>&1 || {
    echo "未找到 qmake，请先安装 Qt 开发环境，例如：sudo apt install qtbase5-dev qt5-qmake"
    exit 1
}

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
"$QMAKE_BIN" "$SCRIPT_DIR/admin.pro"
make -j"$(nproc)"

echo "构建完成：$BUILD_DIR/bin/ncs_admin"
