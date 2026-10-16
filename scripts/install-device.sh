set -eu
STAGE=/data/local/tmp/ihg2-arm-deploy-v2
BASE=/data/local/ihg2-arm-native
HOOK=/system/bin/install-recovery.sh
HOOK_TMP=/system/bin/install-recovery.sh.ihg2-tmp
SYSTEM_RW=0
SERVICE_STOPPED=0
SERVICE_WAS_RUNNING=0
DEPLOYMENT_CHANGED=0
HAD_PREVIOUS_BRIDGE=0
ORIGINAL_HOOK_PRESENT=0
HOOK_TMP_CREATED=0
INSTALL_SUCCEEDED=0

hap_port=53760
digits=0123456789ABCDEF
port_number=$hap_port
port_hex=
while [ "$port_number" -gt 0 ]; do
    remainder=$((port_number % 16))
    digit=$(echo "$digits" | cut -c "$((remainder + 1))")
    port_hex=$digit$port_hex
    port_number=$((port_number / 16))
done
while [ "${#port_hex}" -lt 4 ]; do port_hex=0$port_hex; done

port_is_listening() {
    table=$1
    [ -r "$table" ] || return 1
    while read -r slot local_addr remote_addr state rest; do
        case "$local_addr:$state" in
            *":$port_hex:0A") return 0 ;;
        esac
    done < "$table"
    return 1
}

ensure_managed_directory() {
    path=$1
    if [ -L "$path" ] || { [ -e "$path" ] && [ ! -d "$path" ]; }; then
        echo "Refusing unsafe bridge directory path: $path" >&2
        exit 1
    fi
}

ensure_managed_file_slot() {
    path=$1
    if [ -L "$path" ] || { [ -e "$path" ] && [ ! -f "$path" ]; }; then
        echo "Refusing unsafe bridge file path: $path" >&2
        exit 1
    fi
}

wait_for_listener() {
    attempts=0
    stable_checks=0
    while [ "$attempts" -lt 30 ]; do
        if port_is_listening /proc/net/tcp || port_is_listening /proc/net/tcp6; then
            stable_checks=$((stable_checks + 1))
            if [ "$stable_checks" -ge 5 ]; then return 0; fi
        else
            stable_checks=0
        fi
        sleep 1
        attempts=$((attempts + 1))
    done
    return 1
}

wait_for_service_stop() {
    attempts=0
    service_state=$(getprop init.svc.flash_recovery)
    while [ -n "$service_state" ] && [ "$service_state" != "stopped" ] && [ "$attempts" -lt 15 ]; do
        sleep 1
        attempts=$((attempts + 1))
        service_state=$(getprop init.svc.flash_recovery)
    done
    [ -z "$service_state" ] || [ "$service_state" = "stopped" ]
}

restore_root_file() {
    source=$1
    destination=$2
    mode=$3
    temporary="$destination.ihg2-restore"
    cp "$source" "$temporary" || return 1
    chown 0:0 "$temporary" || return 1
    chmod "$mode" "$temporary" || return 1
    mv -f "$temporary" "$destination"
}

restore_original_hook_state() {
    [ "$ORIGINAL_HOOK_PRESENT" = 0 ] || return 0
    remove_managed_hook
}

remove_managed_hook() {
    if [ ! -f "$HOOK" ] || ! grep -q 'IHG2_ARM_NATIVE_BOOT_HOOK_V1' "$HOOK"; then
        return 0
    fi
    SYSTEM_RW=1
    mount -o rw,remount /system || return 1
    rm -f "$HOOK" || return 1
    mount -o ro,remount /system || return 1
    SYSTEM_RW=0
}

rollback_failed_install() {
    echo "Deployment failed; restoring the previous bridge state." >&2
    setprop ctl.stop flash_recovery >/dev/null 2>&1 || true
    if ! wait_for_service_stop; then
        echo "Could not stop the failed bridge supervisor; rollback files were left untouched." >&2
        return 1
    fi

    if [ "$HAD_PREVIOUS_BRIDGE" = 1 ] && [ -f "$BASE/ihg2-arm-bridge.rollback" ]; then
        if ! restore_root_file "$BASE/ihg2-arm-bridge.rollback" "$BASE/ihg2-arm-bridge" 0755; then
            echo "Could not restore the previous bridge executable." >&2
            return 1
        fi
        if [ -f "$BASE/VERSION.rollback" ]; then
            restore_root_file "$BASE/VERSION.rollback" "$BASE/VERSION" 0600 || return 1
        else
            rm -f "$BASE/VERSION"
        fi
        if [ -f "$BASE/supervisor.sh.rollback" ]; then
            restore_root_file "$BASE/supervisor.sh.rollback" "$BASE/supervisor.sh" 0700 || return 1
        else
            rm -f "$BASE/supervisor.sh"
        fi
        if ! restore_original_hook_state; then
            echo "Previous bridge files were restored, but the newly installed boot hook could not be removed." >&2
            return 1
        fi

        if [ "$SERVICE_WAS_RUNNING" = 1 ]; then
            setprop ctl.start mdnsd >/dev/null 2>&1 || true
            setprop ctl.start flash_recovery >/dev/null 2>&1 || true
            if wait_for_listener; then
                echo "Previous bridge restored and HomeKit listener is ready." >&2
            else
                setprop ctl.stop flash_recovery >/dev/null 2>&1 || true
                wait_for_service_stop || true
                echo "Previous bridge files were restored, but its HomeKit listener did not become ready; supervisor was stopped." >&2
                return 1
            fi
        else
            echo "Previous bridge files restored; service remains stopped as it was before deployment." >&2
        fi
        return 0
    fi

    hook_restore_failed=0
    remove_managed_hook || hook_restore_failed=1
    rm -f "$BASE/ihg2-arm-bridge"
    if [ -f "$BASE/VERSION.rollback" ]; then
        restore_root_file "$BASE/VERSION.rollback" "$BASE/VERSION" 0600 || return 1
    else
        rm -f "$BASE/VERSION"
    fi
    if [ -f "$BASE/supervisor.sh.rollback" ]; then
        restore_root_file "$BASE/supervisor.sh.rollback" "$BASE/supervisor.sh" 0700 || return 1
    else
        rm -f "$BASE/supervisor.sh"
    fi
    if [ "$hook_restore_failed" = 1 ]; then
        echo "No previous bridge executable was available; failed service files were removed, but its boot hook could not be removed." >&2
        return 1
    fi
    echo "No previous bridge executable was available; failed service files were removed and the IHG2 boot hook was removed." >&2
    return 0
}

finish() {
    result=$?
    trap - EXIT HUP INT TERM
    set +e
    if [ "$SYSTEM_RW" = 1 ]; then
        mount -o ro,remount /system >/dev/null 2>&1 || true
        SYSTEM_RW=0
    fi
    if [ "$INSTALL_SUCCEEDED" != 1 ] && [ "$DEPLOYMENT_CHANGED" = 1 ]; then
        rollback_failed_install
    elif [ "$SERVICE_STOPPED" = 1 ]; then
        setprop ctl.start mdnsd >/dev/null 2>&1 || true
        setprop ctl.start flash_recovery >/dev/null 2>&1 || true
    fi
    if [ "$SYSTEM_RW" = 1 ]; then
        mount -o ro,remount /system >/dev/null 2>&1 || true
        SYSTEM_RW=0
    fi
    rm -f "$BASE/ihg2-arm-bridge.ihg2-new" "$BASE/VERSION.ihg2-new" \
        "$BASE/supervisor.sh.ihg2-new" "$BASE/ihg2-arm-bridge.ihg2-restore" \
        "$BASE/VERSION.ihg2-restore" "$BASE/supervisor.sh.ihg2-restore" >/dev/null 2>&1 || true
    if [ "$HOOK_TMP_CREATED" = 1 ]; then rm -f "$HOOK_TMP" >/dev/null 2>&1 || true; fi
    rm -rf "$STAGE" >/dev/null 2>&1 || true
    exit "$result"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

test "$(id -u)" = 0
test "$(getprop ro.product.cpu.abi)" = armeabi-v7a
test "$(getprop ro.build.version.sdk)" -ge 25
test -r /system/lib/libmdnssd.so
grep -q 'service flash_recovery /system/bin/install-recovery.sh' /init.rc
test -f "$STAGE/ihg2-arm-bridge"
test -f "$STAGE/VERSION"
chmod 0700 "$STAGE/ihg2-arm-bridge"
test -x "$STAGE/ihg2-arm-bridge"
version=$(cat "$STAGE/VERSION")
test "$version" = "$("$STAGE/ihg2-arm-bridge" --version | sed 's/^IHG2 ARM bridge version //')"
test -f "$STAGE/supervisor.sh"
test -f "$STAGE/install-recovery.sh"

if [ -e "$BASE" ] || [ -L "$BASE" ]; then
    test -d "$BASE" && test ! -L "$BASE"
fi
ensure_managed_directory "$BASE"
ensure_managed_directory "$BASE/config"
ensure_managed_directory "$BASE/.hap_store"
ensure_managed_file_slot "$BASE/ihg2-arm-bridge"
ensure_managed_file_slot "$BASE/VERSION"
ensure_managed_file_slot "$BASE/supervisor.sh"
ensure_managed_file_slot "$BASE/ihg2-arm-bridge.rollback"
ensure_managed_file_slot "$BASE/VERSION.rollback"
ensure_managed_file_slot "$BASE/supervisor.sh.rollback"
ensure_managed_file_slot "$BASE/ihg2-arm-bridge.ihg2-new"
ensure_managed_file_slot "$BASE/VERSION.ihg2-new"
ensure_managed_file_slot "$BASE/supervisor.sh.ihg2-new"
ensure_managed_file_slot "$BASE/ihg2-arm-bridge.ihg2-restore"
ensure_managed_file_slot "$BASE/VERSION.ihg2-restore"
ensure_managed_file_slot "$BASE/supervisor.sh.ihg2-restore"
if [ -e "$HOOK" ] || [ -L "$HOOK" ]; then
    ORIGINAL_HOOK_PRESENT=1
    if [ -L "$HOOK" ] || [ ! -f "$HOOK" ] || ! grep -q 'IHG2_ARM_NATIVE_BOOT_HOOK_V1' "$HOOK"; then
        echo "Refusing to replace an unrelated recovery hook." >&2
        exit 1
    fi
fi
if [ -e "$HOOK_TMP" ] || [ -L "$HOOK_TMP" ]; then
    echo "Refusing to overwrite an existing temporary hook path." >&2
    exit 1
fi
service_state=$(getprop init.svc.flash_recovery)
if [ -n "$service_state" ] && [ "$service_state" != "stopped" ]; then
    SERVICE_STOPPED=1
    SERVICE_WAS_RUNNING=1
    setprop ctl.stop flash_recovery
    if ! wait_for_service_stop; then
        echo "Could not stop the existing native bridge service." >&2
        exit 1
    fi
fi

mkdir -p "$BASE/config" "$BASE/.hap_store"
chown 0:0 "$BASE" "$BASE/config" "$BASE/.hap_store"
chmod 0700 "$BASE" "$BASE/config" "$BASE/.hap_store"

copy_root_file() {
    source=$1
    destination=$2
    mode=$3
    temporary="$destination.ihg2-new"
    rm -f "$temporary"
    cp "$source" "$temporary"
    chown 0:0 "$temporary"
    chmod "$mode" "$temporary"
    mv -f "$temporary" "$destination"
}

if [ -e "$BASE/ihg2-arm-bridge" ] && [ -f "$BASE/ihg2-arm-bridge" ] && [ ! -L "$BASE/ihg2-arm-bridge" ]; then
    copy_root_file "$BASE/ihg2-arm-bridge" "$BASE/ihg2-arm-bridge.rollback" 0700
    HAD_PREVIOUS_BRIDGE=1
else
    rm -f "$BASE/ihg2-arm-bridge.rollback"
fi
if [ -e "$BASE/VERSION" ] && [ -f "$BASE/VERSION" ] && [ ! -L "$BASE/VERSION" ]; then
    copy_root_file "$BASE/VERSION" "$BASE/VERSION.rollback" 0600
else
    rm -f "$BASE/VERSION.rollback"
fi
if [ -e "$BASE/supervisor.sh" ] && [ -f "$BASE/supervisor.sh" ] && [ ! -L "$BASE/supervisor.sh" ]; then
    copy_root_file "$BASE/supervisor.sh" "$BASE/supervisor.sh.rollback" 0700
else
    rm -f "$BASE/supervisor.sh.rollback"
fi
DEPLOYMENT_CHANGED=1
copy_root_file "$STAGE/ihg2-arm-bridge" "$BASE/ihg2-arm-bridge" 0755
copy_root_file "$STAGE/VERSION" "$BASE/VERSION" 0600
copy_root_file "$STAGE/supervisor.sh" "$BASE/supervisor.sh" 0700

SYSTEM_RW=1
mount -o rw,remount /system
if [ ! -e "$HOOK" ]; then
    HOOK_TMP_CREATED=1
    cp "$STAGE/install-recovery.sh" "$HOOK_TMP"
    chown 0:0 "$HOOK_TMP"
    chmod 0755 "$HOOK_TMP"
    mv "$HOOK_TMP" "$HOOK"
    HOOK_TMP_CREATED=0
fi
chown 0:0 "$HOOK"
chmod 0755 "$HOOK"
if ! restorecon "$HOOK" >/dev/null 2>&1; then
    echo "Could not apply the Android init executable SELinux label to $HOOK." >&2
    exit 1
fi
hook_context=$(ls -Z "$HOOK" 2>/dev/null || true)
case "$hook_context" in
    *u:object_r:install_recovery_exec:s0*) ;;
    *)
        echo "Unexpected SELinux label on $HOOK: ${hook_context:-unavailable}; expected install_recovery_exec." >&2
        exit 1
        ;;
esac
mount -o ro,remount /system
SYSTEM_RW=0

setprop ctl.start mdnsd
setprop ctl.start flash_recovery
attempts=0
while [ "$(getprop init.svc.flash_recovery)" != "running" ] && [ "$attempts" -lt 15 ]; do
    sleep 1
    attempts=$((attempts + 1))
done
if [ "$(getprop init.svc.mdnsd)" != running ]; then
    echo "Android mdnsd service did not start; HomeKit discovery will not work." >&2
    exit 1
fi
if [ "$(getprop init.svc.flash_recovery)" != running ]; then
    echo "Android did not start the bridge boot hook. Check the SELinux label on $HOOK (expected install_recovery_exec)." >&2
    exit 1
fi
test -x "$BASE/ihg2-arm-bridge"

if wait_for_listener; then
    rm -f "$BASE/bridge.json" "$BASE/gateway.key" "$BASE/config/inventory.json"
    INSTALL_SUCCEEDED=1
    SERVICE_STOPPED=0
    echo "IHG2 ARM native bridge v$version installed with panel auto-discovery; HomeKit TCP port $hap_port is listening. Pairing data was preserved."
    exit 0
fi
echo "Bridge service started, but HomeKit TCP port $hap_port did not become ready." >&2
exit 1
