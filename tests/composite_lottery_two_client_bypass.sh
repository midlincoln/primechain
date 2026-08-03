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

for spec in \
    "19160 $base/a.dat $base/a.wallet $base/a.log 19161 19162" \
    "19161 $base/b.dat $base/b.wallet $base/b.log 19160 19162" \
    "19162 $base/c.dat $base/c.wallet $base/c.log 19160 19161"
do
    set -- $spec
    port=$1; dat=$2; wallet=$3; log=$4; peer1=$5; peer2=$6
    "$server" "$port" "$dat" \
        --peer 127.0.0.1 "$peer1" \
        --peer 127.0.0.1 "$peer2" \
        --validator-set "$a" "$b" "$c" \
        --validator-identity "$wallet" \
        --finalization-timeout-ms 500 \
        --composite-lottery-window-ms 100 \
        --composite-lottery-win-bps 10000 \
        > "$log" 2>&1 &
    echo $! > "$base/$port.pid"
    sleep 0.35
done

"$client" init-workdir "$base/miner-a" 127.0.0.1 19160 > "$base/init-a.out"
"$client" init-workdir "$base/miner-b" 127.0.0.1 19160 > "$base/init-b.out"
"$client" add-mine-job "$base/miner-a" --target 18 > "$base/add-a.out"
"$client" add-mine-job "$base/miner-b" --target 18 > "$base/add-b.out"

timeout 12 "$client" run-jobs "$base/miner-a" > "$base/miner-a.log" 2>&1 &
echo $! > "$base/miner-a.pid"
sleep 0.2
timeout 12 "$client" run-jobs "$base/miner-b" > "$base/miner-b.log" 2>&1 &
echo $! > "$base/miner-b.pid"
wait "$(cat "$base/miner-a.pid")" 2>/dev/null || true
wait "$(cat "$base/miner-b.pid")" 2>/dev/null || true

cat "$base/miner-a.log"
cat "$base/miner-b.log"

"$client" status 127.0.0.1 19160 > "$base/status.out"
cat "$base/status.out"
frontier=$(awk '/^STATUS / { print $7 }' "$base/status.out")
if [ -z "$frontier" ] || [ "$frontier" -lt 4 ]; then
    echo "validator did not advance enough for bypass regression: frontier=$frontier" >&2
    exit 1
fi

"$client" sync 127.0.0.1 19160 2 "$frontier" "$base/check.dat" > "$base/sync.out" 2>&1
cat "$base/sync.out"
"$client" launch-report "$base/check.dat" > "$base/report.out"
cat "$base/report.out"

# The test is about exercising the bypass attempt: both clients should produce
# accepted work or lottery/cooldown rejections, while the chain remains replayable.
grep -Eq 'COMPOSITE_ACCEPTED|PRIME_ACCEPTED|composite lottery|provider is in winner cooldown' "$base/miner-a.log"
grep -Eq 'COMPOSITE_ACCEPTED|PRIME_ACCEPTED|composite lottery|provider is in winner cooldown|local proof store behind validator frontier|SUBMIT_SIGNED_REVEAL must target next integer' "$base/miner-b.log"
grep -q '^CHAIN has_genesis=1 ' "$base/report.out"
