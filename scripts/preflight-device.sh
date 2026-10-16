#!/system/bin/sh
set -eu
trap 'rm -f /data/local/tmp/ihg2-arm-preflight.sh' EXIT HUP INT TERM
printf 'abi='; getprop ro.product.cpu.abi
printf 'android_api='; getprop ro.build.version.sdk
printf 'kernel='; uname -m
printf 'mDNS_library='
if test -r /system/lib/libmdnssd.so; then printf 'present\n'; else printf 'missing\n'; fi
printf 'data_free='
df -h /data | tail -n 1
printf 'memory='
grep MemAvailable /proc/meminfo
printf 'bridge_process_memory:\n'
bridge_process_found=0
for status_file in /proc/[0-9]*/status; do
    test -r "$status_file" || continue
    process_name=$(sed -n 's/^Name:[[:space:]]*//p' "$status_file")
    test "$process_name" = ihg2-arm-bridge || continue
    process_pid=${status_file#/proc/}
    process_pid=${process_pid%/status}
    process_memory=$(grep -E '^(VmRSS|VmHWM|Threads):' "$status_file" | tr '\n' ' ')
    printf '  pid=%s %s\n' "$process_pid" "$process_memory"
    bridge_process_found=1
done
if test "$bridge_process_found" -eq 0; then
    printf '  not running\n'
fi
printf 'magisk_service_hook='
if test -d /data/adb/service.d; then printf 'present\n'; else printf 'absent\n'; fi
