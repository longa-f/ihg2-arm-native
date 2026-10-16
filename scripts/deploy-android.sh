#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [adb-serial|panel-ip[:adb-port]] [--doctor|--configure-lock IP MAC]\n' "$0" >&2
    printf '       %s --doctor|--configure-lock IP MAC   (select the only connected ADB device)\n' "$0" >&2
}

target=
mode=install
lock_ip=
lock_mac=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --doctor)
            [[ "$mode" == install ]] || { usage; exit 2; }
            mode=doctor
            shift
            ;;
        --configure-lock)
            [[ "$mode" == install ]] || { usage; exit 2; }
            mode=configure
            shift
            [[ $# -ge 2 && "$1" != -* && "$2" != -* ]] || { usage; exit 2; }
            lock_ip=$1
            lock_mac=$2
            shift 2
            ;;
        -*) usage; exit 2 ;;
        *)
            [[ -z "$target" ]] || { usage; exit 2; }
            target=$1
            shift
            ;;
    esac
done

repo_root=$(cd "$(dirname "$0")/.." && pwd)
script_dir=$(cd "$(dirname "$0")" && pwd)
adb_bin=${ADB:-adb}

finish_deployment_log() {
    local result=$?
    trap - EXIT
    printf 'Deployment finished at %s with exit code %d\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$result"
    exit "$result"
}

if [[ "$mode" == install ]]; then
    log_dir="$script_dir/logs"
    if [[ -L "$log_dir" || ( -e "$log_dir" && ! -d "$log_dir" ) ]]; then
        printf 'Refusing unsafe deployment log directory: %s\n' "$log_dir" >&2
        exit 1
    fi
    if ! mkdir -p "$log_dir" || [[ -L "$log_dir" ]]; then
        printf 'Could not create deployment log directory: %s\n' "$log_dir" >&2
        exit 1
    fi
    chmod 0700 "$log_dir"
    log_file="$log_dir/install-$(date -u '+%Y%m%dT%H%M%SZ')-$$.log"
    if ! (umask 077; set -C; : > "$log_file"); then
        printf 'Could not create deployment log: %s\n' "$log_file" >&2
        exit 1
    fi
    exec > >(tee -a "$log_file") 2>&1
    printf 'Deployment log: %s\n' "$log_file"
    trap finish_deployment_log EXIT
fi

if ! command -v "$adb_bin" >/dev/null 2>&1; then
    printf 'Android platform-tools (adb) is required on this computer.\n' >&2
    exit 2
fi

if [[ -z "$target" ]]; then
    mapfile -t connected < <("$adb_bin" devices | awk 'NR > 1 && $2 == "device" { print $1 }')
    if [[ ${#connected[@]} -ne 1 ]]; then
        printf 'Expected exactly one connected ADB device; found %d. Pass panel-ip[:adb-port] or an ADB serial.\n' "${#connected[@]}" >&2
        exit 2
    fi
    target=${connected[0]}
elif [[ "$target" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}(:[0-9]+)?$ ]]; then
    if [[ "$target" != *:* ]]; then target="$target:1379"; fi
    "$adb_bin" connect "$target"
fi

if ! state=$("$adb_bin" -s "$target" get-state) || [[ "$state" != device ]]; then
    printf 'ADB target is unavailable: %s. Enable/authorize ADB and check network reachability.\n' "$target" >&2
    exit 1
fi
printf 'ADB target: %s\n' "$target"
root_identity=$("$adb_bin" -s "$target" shell su 0 id)
case "$root_identity" in
    *"uid=0"*) ;;
    *) printf 'ADB root access failed: %s\n' "$root_identity" >&2; exit 1 ;;
esac

if [[ "$mode" == doctor ]]; then
    exec "$adb_bin" -s "$target" shell su 0 /data/local/ihg2-arm-native/ihg2-arm-bridge --doctor
fi
if [[ "$mode" == configure ]]; then
    if [[ ! "$lock_ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ || ! "$lock_mac" =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]; then
        printf 'Lock configuration expects an IPv4 address and MAC AA:BB:CC:DD:EE:FF.\n' >&2
        exit 2
    fi
    exec "$adb_bin" -s "$target" shell su 0 /data/local/ihg2-arm-native/ihg2-arm-bridge --configure-lock "$lock_ip" "$lock_mac"
fi

if [[ -x "$script_dir/ihg2-arm-bridge" ]]; then
    asset_dir=$script_dir
    supervisor_file=$script_dir/supervisor.sh
    hook_file=$script_dir/install-recovery.sh
    device_install_file=$script_dir/install-device.sh
elif [[ -x "$repo_root/build/android-armv7/ihg2-arm-bridge" ]]; then
    asset_dir=$repo_root/build/android-armv7
    supervisor_file=$repo_root/scripts/supervisor.sh
    hook_file=$repo_root/scripts/install-recovery.sh
    device_install_file=$repo_root/scripts/install-device.sh
else
    printf 'ARM bridge binary not found. Build the project or extract a release bundle first.\n' >&2
    exit 2
fi

binary="$asset_dir/ihg2-arm-bridge"
if [[ -f "$script_dir/VERSION" ]]; then
    version_file="$script_dir/VERSION"
else
    version_file="$repo_root/VERSION"
fi
if [[ ! -f "$version_file" ]]; then
    printf 'Release VERSION file is missing.\n' >&2
    exit 2
fi
version=$(cat "$version_file")
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    printf 'Release VERSION must contain one SemVer MAJOR.MINOR.PATCH value.\n' >&2
    exit 2
fi
if ! LC_ALL=C grep -aFq 'IHG2_ARM_AUTO_ONLY_V1' "$binary"; then
    printf 'ARM bridge is stale or is not an automatic-only build. Rebuild before deployment.\n' >&2
    exit 2
fi
if ! LC_ALL=C grep -aFq "IHG2 ARM bridge version $version" "$binary"; then
    printf 'Release version file and ARM binary do not match (VERSION=%s). Rebuild the release.\n' "$version" >&2
    exit 2
fi
if [[ ! -f "$supervisor_file" || ! -f "$hook_file" || ! -f "$device_install_file" ]]; then
    printf 'Release bundle is incomplete (supervisor, boot hook, or device installer missing).\n' >&2
    exit 2
fi

remote_stage=/data/local/tmp/ihg2-arm-deploy-v2
printf 'Deploying IHG2 ARM bridge v%s\n' "$version"
"$adb_bin" -s "$target" shell "mkdir -p '$remote_stage' && chmod 0700 '$remote_stage'"
"$adb_bin" -s "$target" push "$binary" "$remote_stage/ihg2-arm-bridge"
"$adb_bin" -s "$target" push "$version_file" "$remote_stage/VERSION"
"$adb_bin" -s "$target" push "$supervisor_file" "$remote_stage/supervisor.sh"
"$adb_bin" -s "$target" push "$hook_file" "$remote_stage/install-recovery.sh"
"$adb_bin" -s "$target" push "$device_install_file" "$remote_stage/install-device.sh"
"$adb_bin" -s "$target" shell su 0 "$remote_stage/ihg2-arm-bridge" --check-panel-sdk
"$adb_bin" -s "$target" shell su 0 sh "$remote_stage/install-device.sh"
