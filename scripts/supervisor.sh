#!/system/bin/sh

BASE=/data/local/ihg2-arm-native
LOG="$BASE/bridge.log"
ROTATED_LOG="$BASE/bridge.log.1"
LOG_PIPE="$BASE/.bridge-log.pipe"
LOG_WRITER_PID=
BRIDGE_PID=

cleanup() {
    if [ -n "$BRIDGE_PID" ]; then
        kill "$BRIDGE_PID" >/dev/null 2>&1 || true
        wait "$BRIDGE_PID" >/dev/null 2>&1 || true
    fi
    if [ -n "$LOG_WRITER_PID" ]; then
        kill "$LOG_WRITER_PID" >/dev/null 2>&1 || true
        wait "$LOG_WRITER_PID" >/dev/null 2>&1 || true
    fi
    rm -f "$LOG_PIPE" >/dev/null 2>&1 || true
}

if [ "$(id -u)" != "0" ]; then
    exit 1
fi

trap cleanup EXIT
trap 'exit 0' HUP INT TERM

umask 077
mkdir -p "$BASE/config" "$BASE/.hap_store"
chmod 0700 "$BASE" "$BASE/config" "$BASE/.hap_store"

rm -f "$LOG_PIPE"
"$BASE/ihg2-arm-bridge" --log-writer "$LOG" "$ROTATED_LOG" "$LOG_PIPE" &
LOG_WRITER_PID=$!

attempts=0
while [ ! -p "$LOG_PIPE" ] && [ "$attempts" -lt 10 ]; do
    sleep 1
    attempts=$((attempts + 1))
done
if [ ! -p "$LOG_PIPE" ]; then
    echo "$(date '+%Y-%m-%d %H:%M:%S') bounded log pipe did not become ready" >&2
    exit 1
fi

log_message() {
    printf '%s\n' "$1" > "$LOG_PIPE"
}

retry_delay=3
while :; do
    if [ ! -x "$BASE/ihg2-arm-bridge" ]; then
        sleep 10
        continue
    fi

    setprop ctl.start mdnsd
    attempts=0
    while [ "$(getprop init.svc.mdnsd)" != "running" ] && [ "$attempts" -lt 15 ]; do
        sleep 1
        attempts=$((attempts + 1))
    done
    if [ "$(getprop init.svc.mdnsd)" != "running" ]; then
        log_message "$(date '+%Y-%m-%d %H:%M:%S') mdnsd did not start"
        sleep 5
        continue
    fi

    cd "$BASE" || exit 1
    log_message "$(date '+%Y-%m-%d %H:%M:%S') starting native bridge"
    bridge_started_at=$(date +%s)
    ./ihg2-arm-bridge > "$LOG_PIPE" 2>&1 &
    BRIDGE_PID=$!
    wait "$BRIDGE_PID"
    result=$?
    BRIDGE_PID=
    log_message "$(date '+%Y-%m-%d %H:%M:%S') bridge exited with status $result; retrying"
    bridge_stopped_at=$(date +%s)
    if [ -n "$bridge_started_at" ] && [ -n "$bridge_stopped_at" ]; then
        bridge_uptime=$((bridge_stopped_at - bridge_started_at))
        if [ "$bridge_uptime" -ge 60 ]; then retry_delay=3; fi
    fi
    sleep "$retry_delay"
    if [ "$retry_delay" -lt 60 ]; then
        retry_delay=$((retry_delay * 2))
        if [ "$retry_delay" -gt 60 ]; then retry_delay=60; fi
    fi
done
