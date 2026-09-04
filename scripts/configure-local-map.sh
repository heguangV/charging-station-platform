#!/usr/bin/env bash
set -euo pipefail
umask 077

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_file="${NCS_ENV_FILE:-${repo_dir}/.env}"

fail() {
    echo "$1" >&2
    exit 2
}

read_setting() {
    local wanted="$1"
    local line name value
    [[ -f "${env_file}" ]] || return 0
    while IFS= read -r line || [[ -n "${line}" ]]; do
        line="${line#export }"
        [[ "${line}" == *"="* ]] || continue
        name="${line%%=*}"
        name="${name//[[:space:]]/}"
        [[ "${name}" == "${wanted}" ]] || continue
        value="${line#*=}"
        value="${value#\"}"
        value="${value%\"}"
        printf '%s' "${value}"
        return 0
    done < "${env_file}"
}

check_api() {
    command -v curl >/dev/null 2>&1 || fail "缺少 curl，无法检查腾讯地图接口"
    command -v jq >/dev/null 2>&1 || fail "缺少 jq，无法安全解析腾讯地图响应"
    local server_key response status mode
    server_key="$(read_setting TENCENT_MAP_SERVER_KEY)"
    [[ -n "${server_key}" ]] || fail "${env_file} 中没有 TENCENT_MAP_SERVER_KEY"

    response="$(curl --silent --show-error --max-time 10 --get \
        'https://apis.map.qq.com/ws/geocoder/v1/' \
        --data-urlencode 'address=北京市海淀区中关村' \
        --data-urlencode "key=${server_key}")"
    status="$(printf '%s' "${response}" | jq -r '.status // -1')"
    [[ "${status}" == "0" ]] || fail "腾讯地图地理编码检查失败（status=${status}）"
    echo "地理编码：成功"

    for mode in driving walking transit; do
        response="$(curl --silent --show-error --max-time 10 --get \
            "https://apis.map.qq.com/ws/direction/v1/${mode}/" \
            --data-urlencode 'from=39.983700,116.315200' \
            --data-urlencode 'to=39.933700,116.478300' \
            --data-urlencode "key=${server_key}")"
        status="$(printf '%s' "${response}" | jq -r '.status // -1')"
        [[ "${status}" == "0" ]] || fail "腾讯地图 ${mode} 路线检查失败（status=${status}）"
        echo "路线 ${mode}：成功"
    done
}

if [[ "${1:-}" == "--check" ]]; then
    check_api
    exit 0
fi
[[ $# -eq 0 ]] || fail "用法：$0 [--check]"
[[ ! -L "${env_file}" ]] || fail "拒绝写入符号链接：${env_file}"
[[ ! -e "${env_file}" || -f "${env_file}" ]] || fail "配置路径不是普通文件：${env_file}"

read -r -s -p "腾讯地图 JavaScript Key: " js_key
echo
read -r -s -p "腾讯地图 WebService Server Key: " server_key
echo
read -r -p "JavaScript 允许来源 [http://localhost/]: " js_origin
js_origin="${js_origin:-http://localhost/}"

[[ "${js_key}" =~ ^[A-Za-z0-9_-]{16,128}$ ]] || fail "JavaScript Key 格式无效"
[[ "${server_key}" =~ ^[A-Za-z0-9_-]{16,128}$ ]] || fail "Server Key 格式无效"
[[ "${js_origin}" =~ ^https?://[A-Za-z0-9._:-]+/$ ]] || fail "JavaScript 来源必须是 http(s)://主机[:端口]/"

env_dir="$(dirname "${env_file}")"
env_name="$(basename "${env_file}")"
mkdir -p "${env_dir}"
temporary="$(mktemp "${env_dir}/.${env_name}.map.XXXXXX")"
cleanup() {
    rm -f -- "${temporary}"
}
trap cleanup EXIT

if [[ -f "${env_file}" ]]; then
    while IFS= read -r line || [[ -n "${line}" ]]; do
        case "${line}" in
            TENCENT_MAP_JS_KEY=*|TENCENT_MAP_SERVER_KEY=*|TENCENT_MAP_JS_ORIGIN=*|export\ TENCENT_MAP_JS_KEY=*|export\ TENCENT_MAP_SERVER_KEY=*|export\ TENCENT_MAP_JS_ORIGIN=*)
                continue
                ;;
        esac
        printf '%s\n' "${line}" >> "${temporary}"
    done < "${env_file}"
fi
printf 'TENCENT_MAP_JS_KEY=%s\n' "${js_key}" >> "${temporary}"
printf 'TENCENT_MAP_SERVER_KEY=%s\n' "${server_key}" >> "${temporary}"
printf 'TENCENT_MAP_JS_ORIGIN=%s\n' "${js_origin}" >> "${temporary}"
chmod 600 "${temporary}"
mv -- "${temporary}" "${env_file}"
trap - EXIT
echo "腾讯地图配置已安全写入 ${env_file}（Key 未回显，权限 600）"
