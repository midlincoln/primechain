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

a=$($client new-miner "$base/a.wallet")
b=$($client new-miner "$base/b.wallet")
c=$($client new-miner "$base/c.wallet")
sorted=$(printf '%s\n%s\n%s\n' "$a" "$b" "$c" | sort | tr '\n' ' ')
set -- $sorted
v1=$1
v2=$2
v3=$3

$server 19187 "$base/a.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"
sleep 0.3

$server 19188 "$base/b.dat" \
    --peer 127.0.0.1 19187 \
    --peer 127.0.0.1 19189 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"
sleep 0.4

$server 19189 "$base/c.dat" \
    --peer 127.0.0.1 19187 \
    --peer 127.0.0.1 19188 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/c.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/c.log" 2>&1 &
echo $! > "$base/c.pid"
sleep 0.6

$client init-workdir "$base/work" 127.0.0.1 19188 > "$base/init.out"
$client add-mine-job "$base/work" --target 3 > "$base/add-3.out"
$client run-jobs "$base/work" > "$base/mine-3.out" 2>&1
grep -q '^JOB_COMPLETE target=3 frontier=3$' "$base/mine-3.out"

receiver=$($client address "$base/work/wallets/composite.wallet")
$send submit 127.0.0.1 19188 "$base/work/wallets/prime.wallet" "$receiver" 3 1000 1 1 > "$base/transfer-1.out"
grep -q '^TX_ACCEPTED ' "$base/transfer-1.out"
$send submit 127.0.0.1 19188 "$base/work/wallets/prime.wallet" "$receiver" 3 1000 1 2 > "$base/transfer-2.out"
grep -q '^TX_ACCEPTED ' "$base/transfer-2.out"

$client add-mine-job "$base/work" --target 4 > "$base/add-4.out"
$client run-jobs "$base/work" > "$base/mine-4.out" 2>&1
grep -q '^JOB_COMPLETE target=4 frontier=4$' "$base/mine-4.out"

$client fee-pool "$base/work/data/chain.dat" > "$base/pool-before.out"
grep -q '^FEE_POOL_HOLDING epoch=0 prime=3 micro_units=2$' "$base/pool-before.out"

$send distribute-fee-pool 127.0.0.1 19188 0 3 2 1 "$a" "$b" "$c" > "$base/distribute.out"
grep -q '^TX_ACCEPTED ' "$base/distribute.out"

$client add-mine-job "$base/work" --target 5 > "$base/add-5.out"
$client run-jobs "$base/work" > "$base/mine-5.out" 2>&1
grep -q '^JOB_COMPLETE target=5 frontier=5$' "$base/mine-5.out"

$client fee-pool "$base/work/data/chain.dat" > "$base/pool-after.out"
grep -q '^VALIDATOR_FEE_POOL .* holdings=0 total_micro_units=0$' "$base/pool-after.out"

for wallet in a b c; do
    $client balance "$base/work/data/chain.dat" "$base/$wallet.wallet" > "$base/$wallet-balance.out"
done
if [ "$v1" = "$a" ]; then grep -q '^3 1$' "$base/a-balance.out"; fi
if [ "$v1" = "$b" ]; then grep -q '^3 1$' "$base/b-balance.out"; fi
if [ "$v1" = "$c" ]; then grep -q '^3 1$' "$base/c-balance.out"; fi
if [ "$v2" = "$a" ]; then grep -q '^3 1$' "$base/a-balance.out"; fi
if [ "$v2" = "$b" ]; then grep -q '^3 1$' "$base/b-balance.out"; fi
if [ "$v2" = "$c" ]; then grep -q '^3 1$' "$base/c-balance.out"; fi
if [ "$v3" = "$a" ]; then ! grep -q '^3 1$' "$base/a-balance.out"; fi
if [ "$v3" = "$b" ]; then ! grep -q '^3 1$' "$base/b-balance.out"; fi
if [ "$v3" = "$c" ]; then ! grep -q '^3 1$' "$base/c-balance.out"; fi

cat "$base/pool-before.out"
cat "$base/pool-after.out"
cat "$base/a-balance.out"
cat "$base/b-balance.out"
cat "$base/c-balance.out"
