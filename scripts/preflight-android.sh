#!/bin/sh
set -eu

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

state=$("$adb_bin" "$@" get-state 2>/dev/null || true)
if [ "$state" != device ]; then
    printf 'ADB target is unavailable. Enable/authorize ADB and check network reachability.\n' >&2
    exit 1
fi
root_identity=$("$adb_bin" "$@" shell su 0 id)
case "$root_identity" in
    *"uid=0"*) ;;
    *) printf 'root access check failed: %s\n' "$root_identity" >&2; exit 1 ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
device_script="$script_dir/preflight-device.sh"
if [ ! -f "$device_script" ]; then
    printf 'preflight device script is missing from this release bundle.\n' >&2
    exit 2
fi
remote_script=/data/local/tmp/ihg2-arm-preflight.sh
"$adb_bin" "$@" push "$device_script" "$remote_script" >/dev/null
"$adb_bin" "$@" shell su 0 sh "$remote_script"
