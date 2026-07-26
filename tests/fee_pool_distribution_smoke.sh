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

$client query 127.0.0.1 19188 ADD_PEER 127.0.0.1 19188 > "$base/add-self-peer.out"
grep -q '^ERROR self peer not allowed$' "$base/add-self-peer.out"

$client query 127.0.0.1 19188 GET_PEER_HEALTH > "$base/peer-health.out"
grep -q '^PEER_HEALTH 2 ' "$base/peer-health.out"
grep -q '^PEER_HEALTH_ENTRY host=127.0.0.1 port=19187 reachable=1 ' "$base/peer-health.out"
grep -q '^PEER_HEALTH_ENTRY host=127.0.0.1 port=19189 reachable=1 ' "$base/peer-health.out"
grep -q '^END_PEER_HEALTH$' "$base/peer-health.out"

$client query 127.0.0.1 19188 ADD_PEER 127.0.0.1 19999 > "$base/add-dead-peer.out"
grep -q '^PEER_ADDED 127.0.0.1 19999$' "$base/add-dead-peer.out"
for i in 1 2 3; do
    $client query 127.0.0.1 19188 GET_PEER_HEALTH > "$base/peer-health-dead-$i.out"
done
grep -q '^PEER_HEALTH_ENTRY host=127.0.0.1 port=19999 reachable=0 failures=3 quarantined=1 ' "$base/peer-health-dead-3.out"

$client init-workdir "$base/work" 127.0.0.1 19188 > "$base/init.out"
$client add-mine-job "$base/work" --target 3 > "$base/add-3.out"
$client run-jobs "$base/work" > "$base/mine-3.out" 2>&1
grep -q '^JOB_COMPLETE target=3 frontier=3$' "$base/mine-3.out"

receiver=$($client address "$base/work/wallets/composite.wallet")
$send submit 127.0.0.1 19188 "$base/work/wallets/prime.wallet" "$receiver" 3 1000 1 1 > "$base/transfer-1.out"
grep -q '^TX_ACCEPTED ' "$base/transfer-1.out"
$send submit 127.0.0.1 19188 "$base/work/wallets/prime.wallet" "$receiver" 3 1000 1 2 > "$base/transfer-2.out"
grep -q '^TX_ACCEPTED ' "$base/transfer-2.out"

$client wallet-pending 127.0.0.1 19188 "$base/work/wallets/prime.wallet" > "$base/sender-pending.out"
grep -q '^WALLET_PENDING .* mempool=2 transactions=2 events=4$' "$base/sender-pending.out"
grep -E -q '^PENDING_TX direction=sent .* prime=3 amount_micro_units=1000 ' "$base/sender-pending.out"
grep -E -q '^PENDING_TX direction=fee-paid .* prime=3 amount_micro_units=1 .* receiver=validator-fee-pool$' "$base/sender-pending.out"

$client wallet-pending 127.0.0.1 19188 "$base/work/wallets/composite.wallet" > "$base/receiver-pending.out"
grep -q '^WALLET_PENDING .* mempool=2 transactions=2 events=2$' "$base/receiver-pending.out"
grep -E -q '^PENDING_TX direction=received .* prime=3 amount_micro_units=1000 ' "$base/receiver-pending.out"

$client add-mine-job "$base/work" --target 4 > "$base/add-4.out"
$client run-jobs "$base/work" > "$base/mine-4.out" 2>&1
grep -q '^JOB_COMPLETE target=4 frontier=4$' "$base/mine-4.out"

$client fee-pool "$base/work/data/chain.dat" > "$base/pool-before.out"
grep -q '^FEE_POOL_HOLDING epoch=0 prime=3 micro_units=2$' "$base/pool-before.out"

$client record "$base/work/data/chain.dat" 4 > "$base/record-4.out"
grep -E -q '^RECORD integer=4 height=.* kind=COMPOSITE .* confirmations=1 .* txs=2 ' "$base/record-4.out"
grep -q '^COMPOSITE_PROOF integer=4 divisor=2 cofactor=2$' "$base/record-4.out"
grep -E -q '^RECORD_TX .* nonce=1 .* fee_micro_units=1 fee_denominator=1$' "$base/record-4.out"

$client latest-records "$base/work/data/chain.dat" --last 2 > "$base/latest-records.out"
grep -E -q '^LATEST_RECORDS .* frontier=4 records=3 showing=2$' "$base/latest-records.out"
grep -E -q '^RECORD integer=3 .* kind=PRIME ' "$base/latest-records.out"
grep -E -q '^RECORD integer=4 .* kind=COMPOSITE ' "$base/latest-records.out"

$client wallet-history "$base/work/data/chain.dat" "$base/work/wallets/prime.wallet" > "$base/sender-history.out"
grep -q '^WALLET_HISTORY .* events=4$' "$base/sender-history.out"
grep -E -q '^TX_EVENT .* confirmations=1 direction=sent .* prime=3 amount_micro_units=1000 .* receiver=' "$base/sender-history.out"
grep -E -q '^TX_EVENT .* confirmations=1 direction=fee-paid .* prime=3 amount_micro_units=1 .* receiver=validator-fee-pool$' "$base/sender-history.out"

$client wallet-history "$base/work/data/chain.dat" "$base/work/wallets/composite.wallet" > "$base/receiver-history.out"
grep -q '^WALLET_HISTORY .* events=2$' "$base/receiver-history.out"
grep -E -q '^TX_EVENT .* direction=received .* prime=3 amount_micro_units=1000 .* receiver=' "$base/receiver-history.out"

$client wallet-history "$base/work/data/chain.dat" "$base/work/wallets/prime.wallet" --last 1 > "$base/sender-history-last.out"
grep -q '^WALLET_HISTORY .* events=4$' "$base/sender-history-last.out"
[ "$(grep -c '^TX_EVENT ' "$base/sender-history-last.out")" -eq 1 ]

sender_address=$($client address "$base/work/wallets/prime.wallet")
$client address-report "$base/work/data/chain.dat" "$sender_address" > "$base/address-report-sender.out"
grep -q "^ADDRESS_REPORT .* address=$sender_address .* holdings=1 .* transactions=2 events=4 sent_micro_units=2000 received_micro_units=0 fee_micro_units=2$" "$base/address-report-sender.out"
grep -E -q '^ADDRESS_TX .* confirmations=1 direction=sent .* prime=3 amount_micro_units=1000 ' "$base/address-report-sender.out"

$client address-report "$base/work/data/chain.dat" "$receiver" --last 1 > "$base/address-report-receiver-last.out"
grep -q "^ADDRESS_REPORT .* address=$receiver .* transactions=2 events=2 sent_micro_units=0 received_micro_units=2000 fee_micro_units=0$" "$base/address-report-receiver-last.out"
[ "$(grep -c '^ADDRESS_TX ' "$base/address-report-receiver-last.out")" -eq 1 ]

tx_hash=$(awk '/^TX_ACCEPTED / { print $2; exit }' "$base/transfer-1.out")
$client tx "$base/work/data/chain.dat" "$tx_hash" > "$base/tx-lookup.out"
grep -q "^TX_FOUND $tx_hash " "$base/tx-lookup.out"
grep -q ' integer=4 ' "$base/tx-lookup.out"
grep -q ' confirmations=1 ' "$base/tx-lookup.out"
grep -q ' version=2 nonce=1 sender=' "$base/tx-lookup.out"
grep -q '^TX_INPUT prime=3 amount_micro_units=1001 amount_denominator=1$' "$base/tx-lookup.out"
grep -q '^TX_OUTPUT prime=3 amount_micro_units=1000 amount_denominator=1 receiver=' "$base/tx-lookup.out"
grep -q '^TX_FEE prime=3 amount_micro_units=1 amount_denominator=1$' "$base/tx-lookup.out"
if $client tx "$base/work/data/chain.dat" 0000000000000000000000000000000000000000000000000000000000000000 > "$base/tx-missing.out"; then
    echo "missing tx lookup succeeded unexpectedly" >&2
    exit 1
fi
grep -q '^TX_NOT_FOUND 0000000000000000000000000000000000000000000000000000000000000000 ' "$base/tx-missing.out"

$client fee-distribution-status "$base/work/data/chain.dat" 1000 > "$base/distribution-status-before.out"
grep -q '^FEE_DISTRIBUTION_STATUS .* interval_records=1000 current_frontier=4 last_distribution_integer=0 next_distribution_integer=1002 due=0 .* pool_total_micro_units=2 distributions=0$' "$base/distribution-status-before.out"
grep -q '^FEE_POOL_HOLDING epoch=0 prime=3 micro_units=2$' "$base/distribution-status-before.out"

$send distribute-fee-pool 127.0.0.1 19188 0 3 2 1 "$a" "$b" "$c" > "$base/distribute.out"
grep -q '^TX_ACCEPTED ' "$base/distribute.out"

$client add-mine-job "$base/work" --target 5 > "$base/add-5.out"
$client run-jobs "$base/work" > "$base/mine-5.out" 2>&1
grep -q '^JOB_COMPLETE target=5 frontier=5$' "$base/mine-5.out"

$client fee-pool "$base/work/data/chain.dat" > "$base/pool-after.out"
grep -q '^VALIDATOR_FEE_POOL .* holdings=0 total_micro_units=0$' "$base/pool-after.out"

$client fee-distribution-status "$base/work/data/chain.dat" 1000 > "$base/distribution-status-after.out"
grep -q '^FEE_DISTRIBUTION_STATUS .* interval_records=1000 current_frontier=5 last_distribution_integer=5 next_distribution_integer=1005 due=0 .* pool_total_micro_units=0 distributions=1$' "$base/distribution-status-after.out"
grep -q '^LAST_FEE_DISTRIBUTION integer=5 epoch=0 prime=3 micro_units=2$' "$base/distribution-status-after.out"
grep -q '^FEE_DISTRIBUTION_EVENT integer=5 epoch=0 prime=3 micro_units=2 recipients=2$' "$base/distribution-status-after.out"

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
cat "$base/distribution-status-before.out"
cat "$base/pool-after.out"
cat "$base/distribution-status-after.out"
cat "$base/a-balance.out"
cat "$base/b-balance.out"
cat "$base/c-balance.out"
