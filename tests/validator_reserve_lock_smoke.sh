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

$server 19190 "$base/node.dat" \
    --validator-set "$validator" \
    --validator-identity "$base/validator.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/node.log" 2>&1 &
echo $! > "$base/node.pid"
sleep 0.3

$client init-workdir "$base/work" 127.0.0.1 19190 > "$base/init.out"
$client add-mine-job "$base/work" --target 3 > "$base/add-3.out"
$client run-jobs "$base/work" > "$base/mine-3.out" 2>&1
grep -q '^JOB_COMPLETE target=3 frontier=3$' "$base/mine-3.out"

$send reserve-lock 127.0.0.1 19190 "$base/work/wallets/prime.wallet" "$validator" 3 500000 1 1 > "$base/reserve-lock.out"
grep -q '^TX_ACCEPTED ' "$base/reserve-lock.out"

$client add-mine-job "$base/work" --target 4 > "$base/add-4.out"
$client run-jobs "$base/work" > "$base/mine-4.out" 2>&1
grep -q '^JOB_COMPLETE target=4 frontier=4$' "$base/mine-4.out"

$client validator-reserve "$base/work/data/chain.dat" "$validator" > "$base/reserve.out"
grep -q '^VALIDATOR_RESERVE .* holdings=1 total_micro_units=500000$' "$base/reserve.out"
grep -q '^RESERVE_HOLDING .* prime=3 micro_units=500000$' "$base/reserve.out"

$client validator-eligibility "$base/work/data/chain.dat" "$validator" --reserve auto --observed 100 --total 100 > "$base/eligibility.out"
grep -q '^RESERVE locked_micro_units=500000 min=5000000 pass=0$' "$base/eligibility.out"

$client fee-pool "$base/work/data/chain.dat" > "$base/pool.out"
grep -q '^FEE_POOL_HOLDING epoch=0 prime=3 micro_units=1$' "$base/pool.out"

cat "$base/reserve.out"
cat "$base/eligibility.out"
cat "$base/pool.out"
