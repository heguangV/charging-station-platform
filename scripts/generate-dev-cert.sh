#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cert_dir="${repo_dir}/runtime/certs"
cert_path="${cert_dir}/dev-cert.pem"
key_path="${cert_dir}/dev-key.pem"

if ! command -v openssl >/dev/null 2>&1; then
    echo "未找到 openssl。" >&2
    exit 2
fi
if [[ ( -e "${cert_path}" || -e "${key_path}" ) && "${1:-}" != "--force" ]]; then
    echo "开发证书已存在；如需替换，请显式传入 --force。" >&2
    exit 3
fi

mkdir -p "${cert_dir}"
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 30 \
    -keyout "${key_path}" -out "${cert_path}" \
    -subj "/CN=localhost/O=NCS Development" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
chmod 600 "${key_path}"

echo "开发证书：${cert_path}"
echo "开发私钥：${key_path}"
echo "仅限本地/受控 VM 开发使用；不要分发或提交。"
