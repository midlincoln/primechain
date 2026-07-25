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

"$server" 19110 "$base/a.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"
sleep 0.3

"$server" 19111 "$base/b.dat" \
    --peer 127.0.0.1 19110 \
    --peer 127.0.0.1 19112 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"
sleep 0.4

"$server" 19112 "$base/c.dat" \
    --peer 127.0.0.1 19110 \
    --peer 127.0.0.1 19111 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/c.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/c.log" 2>&1 &
echo $! > "$base/c.pid"
sleep 0.6

"$client" init-workdir "$base/work" 127.0.0.1 19111 > "$base/init.out"
"$client" add-mine-job "$base/work" --target 10 > "$base/add.out"
"$client" run-jobs "$base/work" > "$base/run.out" 2>&1 || {
    cat "$base/run.out"
    exit 1
}
cat "$base/run.out"
grep -q '^JOB_COMPLETE target=10 frontier=10$' "$base/run.out"

for port in 19110 19111 19112; do
    "$client" status 127.0.0.1 "$port" > "$base/status-$port.out"
    cat "$base/status-$port.out"
    grep -q '^STATUS 9 4 5 1 8 10 ' "$base/status-$port.out"
done

"$client" launch-report "$base/work/data/chain.dat" > "$base/launch-report.out"
cat "$base/launch-report.out"
grep -q '^VALIDATOR_EVIDENCE_SUMMARY active=3 historical=0 bootstrap_dev=2$' "$base/launch-report.out"
active_evidence_count=$(grep -c '^VALIDATOR_EVIDENCE pcpq1_.* class=active ' "$base/launch-report.out")
[ "$active_evidence_count" -eq 3 ]
