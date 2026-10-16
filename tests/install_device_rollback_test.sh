#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/ihg2-install-test.XXXXXX")
mock_bin="$test_root/mock-bin"
mkdir -p "$mock_bin"
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

fail() {
    printf 'install_device_rollback_test: %s\n' "$1" >&2
    exit 1
}

assert_eq() {
    expected=$1
    actual=$2
    label=$3
    [ "$expected" = "$actual" ] || fail "$label: expected '$expected', got '$actual'"
}

cat > "$mock_bin/getprop" <<'MOCK'
#!/bin/sh
case "${1:-}" in
    ro.product.cpu.abi) printf 'armeabi-v7a\n' ;;
    ro.build.version.sdk) printf '25\n' ;;
    init.svc.flash_recovery)
        if [ -f "$TEST_ROOT/flash_recovery" ]; then cat "$TEST_ROOT/flash_recovery"; else printf 'stopped\n'; fi
        ;;
    init.svc.mdnsd)
        if [ -f "$TEST_ROOT/mdnsd" ]; then cat "$TEST_ROOT/mdnsd"; else printf 'stopped\n'; fi
        ;;
    *) exit 1 ;;
esac
MOCK

cat > "$mock_bin/setprop" <<'MOCK'
#!/bin/sh
case "${1:-}:${2:-}" in
    ctl.stop:flash_recovery)
        printf 'stopped\n' > "$TEST_ROOT/flash_recovery"
        rm -f "$TEST_ROOT/proc/net/tcp"
        ;;
    ctl.start:mdnsd)
        printf 'running\n' > "$TEST_ROOT/mdnsd"
        ;;
    ctl.start:flash_recovery)
        printf 'running\n' > "$TEST_ROOT/flash_recovery"
        version=$(cat "$TEST_ROOT/data/local/ihg2-arm-native/VERSION" 2>/dev/null || true)
        if [ "$version" != "${TEST_FAIL_VERSION:-}" ]; then
            printf '  0: 00000000:D200 00000000:0000 0A 00000000:00000000 00:00000000 00000000 0 0 0 1\n' > "$TEST_ROOT/proc/net/tcp"
        else
            rm -f "$TEST_ROOT/proc/net/tcp"
        fi
        ;;
    *) exit 1 ;;
esac
MOCK

cat > "$mock_bin/id" <<'MOCK'
#!/bin/sh
if [ "${1:-}" = -u ]; then printf '0\n'; else exec /usr/bin/id "$@"; fi
MOCK

cat > "$mock_bin/mount" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >> "$TEST_ROOT/mount.log"
MOCK

cat > "$mock_bin/chown" <<'MOCK'
#!/bin/sh
exit 0
MOCK

cat > "$mock_bin/restorecon" <<'MOCK'
#!/bin/sh
exit 0
MOCK

cat > "$mock_bin/ls" <<'MOCK'
#!/bin/sh
if [ "${1:-}" = -Z ]; then printf 'u:object_r:install_recovery_exec:s0 %s\n' "${2:-}"; else exec /bin/ls "$@"; fi
MOCK

cat > "$mock_bin/sleep" <<'MOCK'
#!/bin/sh
exit 0
MOCK
chmod 0755 "$mock_bin"/*

make_binary() {
    binary_path=$1
    binary_version=$2
    cat > "$binary_path" <<EOF
#!/bin/sh
printf 'IHG2 ARM bridge version $binary_version\\n'
EOF
    chmod 0755 "$binary_path"
}

prepare_stage() {
    fixture_root=$1
    stage="$fixture_root/data/local/tmp/ihg2-arm-deploy-v2"
    mkdir -p "$stage"
    make_binary "$stage/ihg2-arm-bridge" 1.1.4
    printf '1.1.4\n' > "$stage/VERSION"
    printf 'new supervisor\n' > "$stage/supervisor.sh"
    printf '#!/system/bin/sh\n# IHG2_ARM_NATIVE_BOOT_HOOK_V1\n' > "$stage/install-recovery.sh"
}

make_fixture() {
    fixture_root=$1
    mkdir -p "$fixture_root/system/bin" "$fixture_root/system/lib" \
        "$fixture_root/data/local/tmp/ihg2-arm-deploy-v2" \
        "$fixture_root/data/local/ihg2-arm-native/config" \
        "$fixture_root/data/local/ihg2-arm-native/.hap_store" \
        "$fixture_root/proc/net"
    touch "$fixture_root/system/lib/libmdnssd.so"
    printf 'service flash_recovery %s/bin/install-recovery.sh\n' "$fixture_root/system" > "$fixture_root/init.rc"
    sed \
        -e "s|/proc/net/tcp6|$fixture_root/proc/net/tcp6|g" \
        -e "s|/proc/net/tcp|$fixture_root/proc/net/tcp|g" \
        -e "s|/data/local/tmp/ihg2-arm-deploy-v2|$fixture_root/data/local/tmp/ihg2-arm-deploy-v2|g" \
        -e "s|/data/local/ihg2-arm-native|$fixture_root/data/local/ihg2-arm-native|g" \
        -e "s|/system|$fixture_root/system|g" \
        -e "s|/init.rc|$fixture_root/init.rc|g" \
        "$repo_root/scripts/install-device.sh" > "$fixture_root/install-device.sh"
    chmod 0755 "$fixture_root/install-device.sh"
    prepare_stage "$fixture_root"
}

run_installer() {
    fixture_root=$1
    fail_version=$2
    output_file=$3
    if TEST_ROOT="$fixture_root" TEST_FAIL_VERSION="$fail_version" PATH="$mock_bin:$PATH" \
        sh -x "$fixture_root/install-device.sh" > "$output_file" 2>&1; then
        return 0
    else
        return $?
    fi
}

upgrade_root="$test_root/upgrade"
make_fixture "$upgrade_root"
base="$upgrade_root/data/local/ihg2-arm-native"
make_binary "$base/ihg2-arm-bridge" 1.1.2
printf '1.1.2\n' > "$base/VERSION"
printf 'old supervisor\n' > "$base/supervisor.sh"
printf '#!/system/bin/sh\n# IHG2_ARM_NATIVE_BOOT_HOOK_V1\n' > "$upgrade_root/system/bin/install-recovery.sh"
printf 'pairing-preserved\n' > "$base/.hap_store/pairing.db"
printf 'running\n' > "$upgrade_root/flash_recovery"
printf 'running\n' > "$upgrade_root/mdnsd"
printf '  0: 00000000:D200 00000000:0000 0A 00000000:00000000 00:00000000 00000000 0 0 0 1\n' > "$upgrade_root/proc/net/tcp"

upgrade_output="$test_root/upgrade.out"
if run_installer "$upgrade_root" 1.1.4 "$upgrade_output"; then
    fail 'failed upgrade unexpectedly returned success'
fi
assert_eq '1.1.2' "$(cat "$base/VERSION")" 'previous version restored'
assert_eq 'IHG2 ARM bridge version 1.1.2' "$("$base/ihg2-arm-bridge" --version)" 'previous executable restored'
assert_eq 'old supervisor' "$(cat "$base/supervisor.sh")" 'previous supervisor restored'
assert_eq 'pairing-preserved' "$(cat "$base/.hap_store/pairing.db")" 'pairing data preserved'
assert_eq 'running' "$(cat "$upgrade_root/flash_recovery")" 'previous running state restored'
test -s "$upgrade_root/proc/net/tcp" || fail 'previous HomeKit listener was not restored'
grep -q 'Previous bridge restored and HomeKit listener is ready' "$upgrade_output" || fail 'rollback success was not reported'
case "$(tail -n 1 "$upgrade_root/mount.log")" in *'-o ro,remount '*) ;; *) fail '/system was not remounted read-only after rollback' ;; esac

prepare_stage "$upgrade_root"
success_output="$test_root/success.out"
if ! run_installer "$upgrade_root" '' "$success_output"; then
    cat "$success_output" >&2
    fail 'healthy upgrade unexpectedly failed'
fi
assert_eq '1.1.4' "$(cat "$base/VERSION")" 'new version installed'
assert_eq 'IHG2 ARM bridge version 1.1.4' "$("$base/ihg2-arm-bridge" --version)" 'new executable installed'
assert_eq 'pairing-preserved' "$(cat "$base/.hap_store/pairing.db")" 'pairing data preserved on success'

fresh_root="$test_root/fresh"
make_fixture "$fresh_root"
fresh_output="$test_root/fresh.out"
if run_installer "$fresh_root" 1.1.4 "$fresh_output"; then
    fail 'failed first install unexpectedly returned success'
fi
test ! -e "$fresh_root/system/bin/install-recovery.sh" || fail 'failed first install left a boot hook behind'
test ! -e "$fresh_root/data/local/ihg2-arm-native/ihg2-arm-bridge" || fail 'failed first install left a broken executable behind'
test ! -e "$fresh_root/data/local/ihg2-arm-native/VERSION" || fail 'failed first install left a misleading version file behind'
assert_eq 'stopped' "$(cat "$fresh_root/flash_recovery")" 'failed first install service stopped'
case "$(tail -n 1 "$fresh_root/mount.log")" in *'-o ro,remount '*) ;; *) fail '/system was not remounted read-only after first-install rollback' ;; esac

unsafe_root="$test_root/unsafe"
make_fixture "$unsafe_root"
mkdir -p "$unsafe_root/data/local/ihg2-arm-native"
printf 'keep-this-file\n' > "$test_root/external-target"
ln -s "$test_root/external-target" "$unsafe_root/data/local/ihg2-arm-native/ihg2-arm-bridge"
unsafe_output="$test_root/unsafe.out"
if run_installer "$unsafe_root" '' "$unsafe_output"; then
    fail 'installer accepted a symlink in its managed service path'
fi
assert_eq 'keep-this-file' "$(cat "$test_root/external-target")" 'symlink target preserved'
assert_eq '' "$(cat "$unsafe_root/flash_recovery" 2>/dev/null || true)" 'unsafe path rejected before service start'

printf 'install_device_rollback_test: rollback, success, first-install cleanup, and unsafe-path refusal passed\n'
