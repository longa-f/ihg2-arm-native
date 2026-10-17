#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/ihg2-deploy-log-test.XXXXXX")
bundle="$test_root/bundle"
mock_bin="$test_root/mock-bin"
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

fail() {
    printf 'deploy_logging_test: %s\n' "$1" >&2
    exit 1
}

mkdir -p "$bundle" "$mock_bin"
cp "$repo_root/scripts/deploy-android.sh" "$bundle/install.sh"
cp "$repo_root/scripts/preflight-android.sh" "$bundle/preflight.sh"
cp "$repo_root/scripts/preflight-device.sh" "$bundle/preflight-device.sh"
cp "$repo_root/scripts/supervisor.sh" "$bundle/supervisor.sh"
cp "$repo_root/scripts/install-recovery.sh" "$bundle/install-recovery.sh"
cp "$repo_root/scripts/install-device.sh" "$bundle/install-device.sh"
printf '1.2.0\n' > "$bundle/VERSION"
cat > "$bundle/ihg2-arm-bridge" <<'BINARY'
#!/bin/sh
printf 'IHG2_ARM_AUTO_ONLY_V1\n'
printf 'IHG2 ARM bridge version 1.2.0\n'
BINARY
chmod 0755 "$bundle/install.sh" "$bundle/preflight.sh" "$bundle/ihg2-arm-bridge"

cat > "$mock_bin/adb" <<'ADB'
#!/bin/sh
set -eu
if [ "${1:-}" = connect ]; then
    printf 'connected to %s\n' "$2"
    exit 0
fi
if [ "${1:-}" = -s ]; then shift 2; fi
case "${1:-}" in
    get-state)
        if [ "${FAKE_ADB_FAIL_STATE:-0}" = 1 ]; then
            printf 'simulated ADB target connection failure\n' >&2
            exit 5
        fi
        printf 'device\n'
        ;;
    push)
        if [ "${FAKE_ADB_FAIL_PUSH:-0}" = 1 ]; then
            printf 'simulated ADB push failure\n' >&2
            exit 23
        fi
        printf 'pushed %s to %s\n' "$2" "$3"
        ;;
    shell)
        shift
        case "${1:-} ${2:-} ${3:-}" in
            'su 0 id') printf 'uid=0(root) gid=0(root)\n' ;;
            'su 0 /data/local/tmp/ihg2-arm-deploy-v2/ihg2-arm-bridge')
                printf 'Panel SDK auto-discovery ready; supported accessories: 31\n' ;;
            'su 0 sh') printf 'simulated device installation completed\n' ;;
            *) : ;;
        esac
        ;;
    *) printf 'unexpected fake adb invocation: %s\n' "$*" >&2; exit 90 ;;
esac
ADB
chmod 0755 "$mock_bin/adb"

ADB="$mock_bin/adb" "$bundle/preflight.sh" 192.0.2.11 > "$test_root/preflight.stdout" 2>&1
preflight_log=$(find "$bundle/logs" -maxdepth 1 -type f -name 'preflight-*.log' -print -quit)
[ -n "$preflight_log" ] || fail 'preflight log was not created'
grep -Fq 'ADB target: 192.0.2.11:1379' "$preflight_log" || fail 'preflight target missing from log'
grep -Fq 'pushed ' "$preflight_log" || fail 'preflight ADB transfer missing from log'
grep -Fq 'exit code 0' "$preflight_log" || fail 'preflight exit code missing from log'

ADB="$mock_bin/adb" "$bundle/install.sh" 192.0.2.11 > "$test_root/install.stdout" 2>&1
success_log=$(find "$bundle/logs" -maxdepth 1 -type f -name 'install-*.log' -print -quit)
[ -n "$success_log" ] || fail 'success log was not created'
for expected in \
    'Deployment log:' \
    'ADB target: 192.0.2.11:1379' \
    'Deploying IHG2 ARM bridge v1.2.0' \
    'pushed ' \
    'simulated device installation completed' \
    'exit code 0'; do
    grep -Fq "$expected" "$success_log" || fail "success log does not contain '$expected'"
done
[ "$(stat -c '%a' "$bundle/logs")" = 700 ] || fail 'log directory permissions are not 0700'
[ "$(stat -c '%a' "$success_log")" = 600 ] || fail 'log file permissions are not 0600'

set +e
FAKE_ADB_FAIL_PUSH=1 ADB="$mock_bin/adb" "$bundle/install.sh" 192.0.2.11 > "$test_root/install-failed.stdout" 2>&1
failure_status=$?
set -e
[ "$failure_status" -eq 23 ] || fail "push failure exit status changed: $failure_status"
failure_log=$(find "$bundle/logs" -maxdepth 1 -type f -name 'install-*.log' -print | sort | tail -n 1)
grep -Fq 'simulated ADB push failure' "$failure_log" || fail 'ADB stderr missing from failure log'
grep -Fq 'exit code 23' "$failure_log" || fail 'failure exit code missing from log'

set +e
FAKE_ADB_FAIL_STATE=1 ADB="$mock_bin/adb" "$bundle/install.sh" 192.0.2.11 > "$test_root/state-failed.stdout" 2>&1
state_status=$?
set -e
[ "$state_status" -eq 1 ] || fail "unavailable ADB target exit status changed: $state_status"
state_log=$(find "$bundle/logs" -maxdepth 1 -type f -name 'install-*.log' -print | sort | tail -n 1)
grep -Fq 'simulated ADB target connection failure' "$state_log" || fail 'ADB connection diagnostic missing from log'
grep -Fq 'exit code 1' "$state_log" || fail 'ADB connection failure exit code missing from log'

logs_before_doctor=$(find "$bundle/logs" -maxdepth 1 -type f -name 'install-*.log' | wc -l)
ADB="$mock_bin/adb" "$bundle/install.sh" 192.0.2.11 --doctor > "$test_root/doctor.stdout" 2>&1
logs_after_doctor=$(find "$bundle/logs" -maxdepth 1 -type f -name 'install-*.log' | wc -l)
[ "$logs_before_doctor" = "$logs_after_doctor" ] || fail '--doctor unexpectedly created a transcript'

printf 'deploy_logging_test: install/preflight output, ADB failures, exit codes, permissions, and doctor-code exclusion passed\n'
