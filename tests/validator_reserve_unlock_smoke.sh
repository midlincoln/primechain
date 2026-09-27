#!/bin/sh
set -eu

client=$1
server=$2
send=$3
base=$4

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

validator=$($client new-miner "$base/validator.wallet")
candidate=$($client new-miner "$base/candidate.wallet")

$server 19193 "$base/node.dat"     --validator-set "$validator"     --validator-identity "$base/validator.wallet"     --finalization-timeout-ms 500     > "$base/node.log" 2>&1 &
echo $! > "$base/node.pid"
sleep 0.3

$client init-workdir "$base/work" 127.0.0.1 19193 > "$base/init.out"
receiver=$($client address "$base/work/wallets/prime.wallet")

$client add-mine-job "$base/work" --target 3 > "$base/add-3.out"
$client run-jobs "$base/work" > "$base/mine-3.out" 2>&1
grep -q '^JOB_COMPLETE target=3 frontier=3$' "$base/mine-3.out"

# Active validators cannot unlock their own reserve while voting.
$send reserve-lock 127.0.0.1 19193 "$base/work/wallets/prime.wallet" "$validator" 3 500000 1 1 > "$base/active-lock.out"
grep -q '^TX_ACCEPTED ' "$base/active-lock.out"
$client add-mine-job "$base/work" --target 4 > "$base/add-4.out"
$client run-jobs "$base/work" > "$base/mine-4.out" 2>&1
grep -q '^JOB_COMPLETE target=4 frontier=4$' "$base/mine-4.out"

if $send reserve-unlock 127.0.0.1 19193 "$base/validator.wallet" "$receiver" 3 499999 1 1 > "$base/active-unlock.out" 2>&1; then
    echo "active validator reserve unlock unexpectedly succeeded"
    cat "$base/active-unlock.out"
    exit 1
fi
grep -q 'active validator reserve cannot be unlocked' "$base/active-unlock.out"

# A non-active candidate can unlock reserve it controls.
$send reserve-lock 127.0.0.1 19193 "$base/work/wallets/prime.wallet" "$candidate" 3 100000 1 2 > "$base/candidate-lock.out"
grep -q '^TX_ACCEPTED ' "$base/candidate-lock.out"
$client add-mine-job "$base/work" --target 5 > "$base/add-5.out"
$client run-jobs "$base/work" > "$base/mine-5.out" 2>&1
grep -q '^JOB_COMPLETE target=5 frontier=5$' "$base/mine-5.out"

$client validator-reserve "$base/work/data/chain.dat" "$candidate" > "$base/candidate-reserve-before.out"
grep -q '^VALIDATOR_RESERVE .* total_micro_units=100000$' "$base/candidate-reserve-before.out"

$send reserve-unlock 127.0.0.1 19193 "$base/candidate.wallet" "$receiver" 3 99999 1 1 > "$base/candidate-unlock.out"
grep -q '^TX_ACCEPTED ' "$base/candidate-unlock.out"
$client add-mine-job "$base/work" --target 6 > "$base/add-6.out"
$client run-jobs "$base/work" > "$base/mine-6.out" 2>&1
grep -q '^JOB_COMPLETE target=6 frontier=6$' "$base/mine-6.out"

$client validator-reserve "$base/work/data/chain.dat" "$candidate" > "$base/candidate-reserve-after.out"
grep -q '^VALIDATOR_RESERVE .* holdings=0 total_micro_units=0$' "$base/candidate-reserve-after.out"

cat "$base/active-unlock.out"
cat "$base/candidate-reserve-before.out"
cat "$base/candidate-reserve-after.out"
