#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_dir}"

git diff --check

oversized="$(
    while IFS= read -r -d '' source_file; do
        line_count="$(wc -l < "${source_file}")"
        if (( line_count > 400 )); then
            printf '%s %s\n' "${line_count}" "${source_file}"
        fi
    done < <(find apps/user apps/admin core infrastructure server tests -type f \
        \( -name '*.cpp' -o -name '*.h' \) -print0 2>/dev/null)
)"
if [[ -n "${oversized}" ]]; then
    echo "以下手写源文件超过 400 行：" >&2
    echo "${oversized}" >&2
    exit 1
fi

if command -v clang-format >/dev/null 2>&1; then
    find apps/user apps/admin core infrastructure server tests -type f \
        \( -name '*.cpp' -o -name '*.h' \) -print0 \
        | xargs -0 -r clang-format --dry-run --Werror
else
    echo "提示：未安装 clang-format，已跳过格式检查。" >&2
fi
