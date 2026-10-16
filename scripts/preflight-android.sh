#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

finish_preflight_log() {
    local result=$?
    trap - EXIT
    printf 'Preflight finished at %s with exit code %d\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$result"
    exit "$result"
}

log_dir="$script_dir/logs"
if [[ -L "$log_dir" || ( -e "$log_dir" && ! -d "$log_dir" ) ]]; then
    printf 'Refusing unsafe preflight log directory: %s\n' "$log_dir" >&2
    exit 1
fi
if ! mkdir -p "$log_dir" || [[ -L "$log_dir" ]]; then
    printf 'Could not create preflight log directory: %s\n' "$log_dir" >&2
    exit 1
fi
chmod 0700 "$log_dir"
log_file="$log_dir/preflight-$(date -u '+%Y%m%dT%H%M%SZ')-$$.log"
if ! (umask 077; set -C; : > "$log_file"); then
    printf 'Could not create preflight log: %s\n' "$log_file" >&2
    exit 1
fi
exec > >(tee -a "$log_file") 2>&1
printf 'Preflight log: %s\n' "$log_file"
trap finish_preflight_log EXIT

if [ "$#" -gt 1 ]; then
    printf 'usage: %s [adb-serial|panel-ip[:adb-port]]\n' "$0" >&2
    exit 2
fi

adb_bin=${ADB:-adb}
serial=${1:-}
if [ -n "$serial" ]; then
    case "$serial" in
        *.*.*.*)
            case "$serial" in
                *:*) ;;
                *) serial="$serial:1379" ;;
            esac
            "$adb_bin" connect "$serial"
            ;;
    esac
    set -- -s "$serial"
else
    connected=$(
        "$adb_bin" devices |
            awk 'NR > 1 && $2 == "device" { print $1 }'
    )
    count=$(printf '%s\n' "$connected" | awk 'NF { n++ } END { print n+0 }')
    if [ "$count" -ne 1 ]; then
        printf 'expected exactly one connected ADB device; found %s. Pass panel-ip[:adb-port] or an ADB serial.\n' "$count" >&2
        exit 2
    fi
    serial=$(printf '%s\n' "$connected" | awk 'NF { print; exit }')
    set -- -s "$serial"
fi
printf 'ADB target: %s\n' "$serial"

state=$("$adb_bin" "$@" get-state || true)
if [ "$state" != device ]; then
    printf 'ADB target is unavailable. Enable/authorize ADB and check network reachability.\n' >&2
    exit 1
fi
root_identity=$("$adb_bin" "$@" shell su 0 id)
case "$root_identity" in
    *"uid=0"*) ;;
    *) printf 'root access check failed: %s\n' "$root_identity" >&2; exit 1 ;;
esac

device_script="$script_dir/preflight-device.sh"
if [ ! -f "$device_script" ]; then
    printf 'preflight device script is missing from this release bundle.\n' >&2
    exit 2
fi
remote_script=/data/local/tmp/ihg2-arm-preflight.sh
"$adb_bin" "$@" push "$device_script" "$remote_script"
"$adb_bin" "$@" shell su 0 sh "$remote_script"
