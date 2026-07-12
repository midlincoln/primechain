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

"$server" 19130 "$base/a.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"
sleep 0.3

"$server" 19131 "$base/b.dat" \
    --peer 127.0.0.1 19130 \
    --peer 127.0.0.1 19132 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"
sleep 0.4

"$server" 19132 "$base/c.dat" \
    --peer 127.0.0.1 19130 \
    --peer 127.0.0.1 19131 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/c.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/c.log" 2>&1 &
echo $! > "$base/c.pid"
sleep 0.6

"$client" init-workdir "$base/work-a" 127.0.0.1 19131 > "$base/init-a.out"
"$client" init-workdir "$base/work-b" 127.0.0.1 19132 > "$base/init-b.out"
"$client" add-mine-job "$base/work-a" --target 14 > "$base/add-a.out"
"$client" add-mine-job "$base/work-b" --target 14 > "$base/add-b.out"

"$client" run-jobs "$base/work-a" > "$base/run-a.out" 2>&1 &
echo $! > "$base/run-a.pid"
"$client" run-jobs "$base/work-b" > "$base/run-b.out" 2>&1 &
echo $! > "$base/run-b.pid"

wait "$(cat "$base/run-a.pid")" || {
    cat "$base/run-a.out"
    cat "$base/run-b.out"
    exit 1
}
wait "$(cat "$base/run-b.pid")" || {
    cat "$base/run-a.out"
    cat "$base/run-b.out"
    exit 1
}

cat "$base/run-a.out"
cat "$base/run-b.out"
grep -q '^JOB_COMPLETE target=14 frontier=14$' "$base/run-a.out"
grep -q '^JOB_COMPLETE target=14 frontier=14$' "$base/run-b.out"

for port in 19130 19131 19132; do
    "$client" status 127.0.0.1 "$port" > "$base/status-$port.out"
    cat "$base/status-$port.out"
    grep -q '^STATUS 13 6 7 1 12 14 ' "$base/status-$port.out"
done
