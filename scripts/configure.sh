#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-dev}"

if [[ -n "${QT_CMAKE:-}" ]]; then
    qt_cmake="${QT_CMAKE}"
elif command -v qt-cmake >/dev/null 2>&1; then
    qt_cmake="$(command -v qt-cmake)"
else
    echo "未找到 qt-cmake。请设置 QT_CMAKE=/path/to/Qt/6.2.x/.../bin/qt-cmake。" >&2
    exit 2
fi

"${qt_cmake}" --preset "${preset}" -S "${repo_dir}"
