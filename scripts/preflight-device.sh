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
printf 'magisk_service_hook='
if test -d /data/adb/service.d; then printf 'present\n'; else printf 'absent\n'; fi
