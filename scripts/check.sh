#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_dir}"

source_roots=(apps/user apps/admin core infrastructure server tests)
base_ref="${NCS_CHECK_BASE_REF:-}"

if [[ "${base_ref}" =~ ^0+$ ]]; then
    base_ref=""
fi
if [[ -n "${base_ref}" ]]; then
    if [[ "${base_ref}" == -* ]] ||
        ! base_commit="$(git rev-parse --verify --quiet "${base_ref}^{commit}")"; then
        echo "NCS_CHECK_BASE_REF 不是可用的 Git 提交：${base_ref}" >&2
        exit 2
    fi
    git diff --check "${base_commit}...HEAD"
fi
git diff --check HEAD

declare -A seen_sources=()
changed_sources=()

add_source() {
    local source_file="$1"
    case "${source_file}" in
        apps/user/*.cpp | apps/user/*.h | apps/admin/*.cpp | apps/admin/*.h | core/*.cpp | core/*.h | \
            infrastructure/*.cpp | infrastructure/*.h | server/*.cpp | server/*.h | tests/*.cpp | tests/*.h)
            ;;
        *) return 0 ;;
    esac
    [[ -f "${source_file}" ]] || return 0
    [[ -z "${seen_sources[${source_file}]+present}" ]] || return 0
    seen_sources["${source_file}"]=true
    changed_sources+=("${source_file}")
}

if [[ -n "${base_ref}" ]]; then
    while IFS= read -r -d '' source_file; do
        add_source "${source_file}"
    done < <(git diff --name-only --diff-filter=ACMR -z "${base_commit}...HEAD" -- "${source_roots[@]}")
fi
while IFS= read -r -d '' source_file; do
    add_source "${source_file}"
done < <(git diff --name-only --diff-filter=ACMR -z HEAD -- "${source_roots[@]}")
while IFS= read -r -d '' source_file; do
    add_source "${source_file}"
done < <(git ls-files --others --exclude-standard -z -- "${source_roots[@]}")

# 行数上限与需求基线 NFR-M-01 保持一致；存量超限文件属技术债，拆分完成前豁免。
line_limit=700
exempt_files=(
    "core/application/charge_flow_service.cpp"
    "infrastructure/sqlite/sqlite_repository.cpp"
    "server/controller/admin_routes.cpp"
)

oversized="$(
    for source_file in "${changed_sources[@]}"; do
        exempt=false
        for exempt_file in "${exempt_files[@]}"; do
            if [[ "${source_file}" == "${exempt_file}" ]]; then
                exempt=true
                break
            fi
        done
        if [[ "${exempt}" == true ]]; then
            continue
        fi
        line_count="$(wc -l < "${source_file}")"
        if (( line_count > line_limit )); then
            printf '%s %s\n' "${line_count}" "${source_file}"
        fi
    done
)"
if [[ -n "${oversized}" ]]; then
    echo "以下手写源文件超过 ${line_limit} 行：" >&2
    echo "${oversized}" >&2
    exit 1
fi

if (( ${#changed_sources[@]} == 0 )); then
    echo "没有需要检查的 C/C++ 变更。"
elif command -v clang-format >/dev/null 2>&1; then
    echo "检查 ${#changed_sources[@]} 个已变更的 C/C++ 文件。"
    clang-format --dry-run --Werror "${changed_sources[@]}"
else
    echo "提示：未安装 clang-format，已跳过格式检查。" >&2
fi
