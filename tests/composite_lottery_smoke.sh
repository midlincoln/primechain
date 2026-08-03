#!/bin/sh
set -eu

client=$1
server=$2
base=$3
mode=${4:-win}

rm -rf "$base"
mkdir -p "$base"

cleanup() {
    for pidfile in "$base"/*.pid; do
        [ -f "$pidfile" ] || continue
        pid=$(cat "$pidfile")
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

new_miner() {
    "$client" new-miner "$1"
}

a=$(new_miner "$base/a.wallet")

case "$mode" in
    win) win_bps=10000 ; target=6 ;;
    lose) win_bps=0 ; target=6 ;;
    *) echo "unknown mode: $mode" >&2 ; exit 2 ;;
esac

"$server" 19140 "$base/a.dat" \
    --validator-set "$a" \
    --validator-identity "$base/a.wallet" \
    --finalization-timeout-ms 500 \
    --composite-lottery-window-ms 25 \
    --composite-lottery-win-bps "$win_bps" \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"
sleep 0.3

"$client" init-workdir "$base/work" 127.0.0.1 19140 > "$base/init.out"
"$client" add-mine-job "$base/work" --target "$target" > "$base/add.out"

if [ "$mode" = win ]; then
    "$client" run-jobs "$base/work" > "$base/run.out" 2>&1
    cat "$base/run.out"
    grep -q "^JOB_COMPLETE target=$target frontier=$target$" "$base/run.out"
    grep -q "composite lottery winner" "$base/a.log"
else
    "$client" run-jobs "$base/work" > "$base/run.out" 2>&1
    cat "$base/run.out"
    grep -q "^JOB_COMPLETE target=$target frontier=$target$" "$base/run.out"
    grep -q "composite lottery winner" "$base/a.log"
fi
