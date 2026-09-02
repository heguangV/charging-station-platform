#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
environment="${1:-}"
confirmation="${2:-}"

if [[ "${environment}" != "test" && "${environment}" != "development" ]]; then
    echo "用法：$0 <test|development> --yes" >&2
    echo "脚本拒绝清理 production。" >&2
    exit 2
fi
if [[ "${confirmation}" != "--yes" ]]; then
    echo "将清理 runtime/${environment}；确认后追加 --yes。" >&2
    exit 3
fi

target_dir="${repo_dir}/runtime/${environment}"
case "${target_dir}" in
    "${repo_dir}/runtime/test"|"${repo_dir}/runtime/development") ;;
    *) echo "拒绝清理未验证路径。" >&2; exit 4 ;;
esac

if [[ -d "${target_dir}" ]]; then
    rm -rf -- "${target_dir}"
    echo "已删除 ${target_dir}；该运行数据不可从 Git 恢复。"
else
    echo "无需清理：${target_dir} 不存在。"
fi
