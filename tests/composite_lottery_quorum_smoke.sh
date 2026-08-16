#!/bin/sh
set -eu

client=$1
server=$2
base=$3

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
b=$(new_miner "$base/b.wallet")
c=$(new_miner "$base/c.wallet")

"$server" 19150 "$base/a.dat" \
    --peer 127.0.0.1 19151 \
    --peer 127.0.0.1 19152 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    --finalization-timeout-ms 500 \
    --composite-lottery-window-ms 25 \
    --composite-lottery-win-bps 10000 \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"
sleep 0.3

"$server" 19151 "$base/b.dat" \
    --peer 127.0.0.1 19150 \
    --peer 127.0.0.1 19152 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    --finalization-timeout-ms 500 \
    --composite-lottery-window-ms 25 \
    --composite-lottery-win-bps 10000 \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"
sleep 0.4

"$server" 19152 "$base/c.dat" \
    --peer 127.0.0.1 19150 \
    --peer 127.0.0.1 19151 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/c.wallet" \
    --finalization-timeout-ms 500 \
    --composite-lottery-window-ms 25 \
    --composite-lottery-win-bps 10000 \
    > "$base/c.log" 2>&1 &
echo $! > "$base/c.pid"
sleep 0.8

"$client" init-workdir "$base/work" 127.0.0.1 19150 > "$base/init.out"
"$client" add-mine-job "$base/work" --target 6 > "$base/add.out"
"$client" run-jobs "$base/work" > "$base/run.out" 2>&1
cat "$base/run.out"
grep -q '^JOB_COMPLETE target=6 frontier=6$' "$base/run.out"

for port in 19150 19151 19152; do
    "$client" sync 127.0.0.1 "$port" 2 6 "$base/check-$port.dat" > "$base/sync-$port.out"
    "$client" launch-report "$base/check-$port.dat" > "$base/report-$port.out"
    grep -q '^VALIDATOR_STATE epoch=0 active_validators=3 ' "$base/report-$port.out"
done
