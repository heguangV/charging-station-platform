#!/usr/bin/env bash
# Re-establish the USB development connection without changing server data.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sdk_dir="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
adb_bin="${ADB:-$sdk_dir/platform-tools/adb}"
serial="${ANDROID_SERIAL:-}"
if [[ ! -x "$adb_bin" ]]; then
    echo "未找到 adb，请设置 ANDROID_SDK_ROOT 或 ADB。" >&2
    exit 1
fi
device_args=()
if [[ -n "$serial" ]]; then device_args=(-s "$serial"); fi
if ! "$adb_bin" "${device_args[@]}" get-state >/dev/null 2>&1; then
    echo "请将测试手机连接到虚拟机、解锁并允许 USB 调试；多设备时设置 ANDROID_SERIAL。" >&2
    "$adb_bin" devices -l
    exit 1
fi
curl --fail --silent --max-time 5 http://127.0.0.1:18443/api/v1/system/health/ready >/dev/null || {
    echo "本机服务未就绪，请先启动 ncs-server.service。" >&2
    exit 1
}
"$adb_bin" "${device_args[@]}" reverse tcp:18443 tcp:18443
if [[ "${1:-}" == "--install" ]]; then
    apk_path="${2:-$repo_dir/build-mobile-android/android-build/build/outputs/apk/debug/android-build-debug.apk}"
    [[ -f "$apk_path" ]] || { echo "请先构建安卓 APK。" >&2; exit 1; }
    "$adb_bin" "${device_args[@]}" install -r "$apk_path"
    "$adb_bin" "${device_args[@]}" shell am force-stop com.ncs.charging
elif [[ -n "${1:-}" ]]; then
    echo "用法：apps/mobile/run_android.sh [--install [APK路径]]" >&2
    exit 1
fi
"$adb_bin" "${device_args[@]}" shell am start -n com.ncs.charging/org.qtproject.qt.android.bindings.QtActivity
echo "USB 联调连接已恢复。应用服务器地址：http://127.0.0.1:18443/api/v1"
