set -eu
STAGE=/data/local/tmp/ihg2-arm-deploy-v2
BASE=/data/local/ihg2-arm-native
HOOK=/system/bin/install-recovery.sh
HOOK_TMP=/system/bin/install-recovery.sh.ihg2-tmp
SYSTEM_RW=0
SERVICE_STOPPED=0

finish() {
    if [ "$SYSTEM_RW" = 1 ]; then
        mount -o ro,remount /system >/dev/null 2>&1 || true
    fi
    if [ "$SERVICE_STOPPED" = 1 ]; then
        setprop ctl.start flash_recovery >/dev/null 2>&1 || true
    fi
    rm -rf "$STAGE" >/dev/null 2>&1 || true
}
trap finish EXIT HUP INT TERM

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
if [ -e "$HOOK" ] || [ -L "$HOOK" ]; then
    if [ -L "$HOOK" ] || [ ! -f "$HOOK" ] || ! grep -q 'IHG2_ARM_NATIVE_BOOT_HOOK_V1' "$HOOK"; then
        echo "Refusing to replace an unrelated recovery hook." >&2
        exit 1
    fi
fi
if [ -e "$HOOK_TMP" ] || [ -L "$HOOK_TMP" ]; then
    echo "Refusing to overwrite an existing temporary hook path." >&2
    exit 1
fi
if [ "$(getprop init.svc.flash_recovery)" = "running" ]; then
    SERVICE_STOPPED=1
    setprop ctl.stop flash_recovery
    attempts=0
    while [ "$(getprop init.svc.flash_recovery)" = "running" ] && [ "$attempts" -lt 15 ]; do
        sleep 1
        attempts=$((attempts + 1))
    done
    if [ "$(getprop init.svc.flash_recovery)" = "running" ]; then
        echo "Could not stop the existing native bridge service." >&2
        exit 1
    fi
fi

mkdir -p "$BASE/config" "$BASE/.hap_store"
chown -R 0:0 "$BASE"
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
fi
if [ -e "$BASE/VERSION" ] && [ -f "$BASE/VERSION" ] && [ ! -L "$BASE/VERSION" ]; then
    copy_root_file "$BASE/VERSION" "$BASE/VERSION.rollback" 0600
fi
copy_root_file "$STAGE/ihg2-arm-bridge" "$BASE/ihg2-arm-bridge" 0755
copy_root_file "$STAGE/VERSION" "$BASE/VERSION" 0600
copy_root_file "$STAGE/supervisor.sh" "$BASE/supervisor.sh" 0700

mount -o rw,remount /system
SYSTEM_RW=1
if [ ! -e "$HOOK" ]; then
    cp "$STAGE/install-recovery.sh" "$HOOK_TMP"
    chown 0:0 "$HOOK_TMP"
    chmod 0755 "$HOOK_TMP"
    mv "$HOOK_TMP" "$HOOK"
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

# Confirm the automatic HomeKit listener is live before reporting success.
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
attempts=0
while [ "$attempts" -lt 20 ]; do
    if port_is_listening /proc/net/tcp || port_is_listening /proc/net/tcp6; then
        rm -f "$BASE/bridge.json" "$BASE/gateway.key" "$BASE/config/inventory.json"
        SERVICE_STOPPED=0
        echo "IHG2 ARM native bridge v$version installed with panel auto-discovery; HomeKit TCP port $hap_port is listening. Pairing data was preserved."
        exit 0
    fi
    sleep 1
    attempts=$((attempts + 1))
done
echo "Bridge service started, but HomeKit TCP port $hap_port did not become ready." >&2
exit 1
