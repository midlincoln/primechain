#!/bin/sh
set -eu

client=$1
server=$2
base=$3

export PRIMECHAIN_WALLET_PASSPHRASE=${PRIMECHAIN_WALLET_PASSPHRASE:-test-passphrase}

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

$client new-miner "$base/validator.wallet" > "$base/validator-address.out"
$client new-miner "$base/candidate.wallet" > "$base/candidate-address.out"
validator=$($client address "$base/validator.wallet")
candidate=$($client address "$base/candidate.wallet")

$server \
    19195 \
    "$base/validator.dat" \
    --genesis-validator-set "$validator" \
    --validator-identity "$base/validator.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/validator.log" 2>&1 &
echo $! > "$base/validator.pid"
sleep 0.3

$client status 127.0.0.1 19195 > "$base/validator-status.out"
grep -q '^STATUS 1 1 0 1 0 2 ' "$base/validator-status.out"

$server \
    19196 \
    "$base/candidate.dat" \
    --bootstrap-peer 127.0.0.1 19195 \
    --genesis-validator-set "$validator" \
    --validator-identity "$base/candidate.wallet" \
    --use-chain-endpoints \
    --finalization-timeout-ms 500 \
    > "$base/candidate.log" 2>&1 &
echo $! > "$base/candidate.pid"
sleep 0.4

$client status 127.0.0.1 19196 > "$base/candidate-status.out"
grep -q '^STATUS 1 1 0 1 0 2 ' "$base/candidate-status.out"

$client query 127.0.0.1 19196 GET_VALIDATORS > "$base/candidate-validators.out"
grep -q "^VALIDATORS 1 $validator$" "$base/candidate-validators.out"

# The candidate identity is configured but not active. It should still start, sync,
# and expose the replay-derived active validator set without signing duties.
! grep -q "validator genesis anchor failed" "$base/candidate.log"
! grep -q "local validator identity is not in active validator epoch" "$base/candidate.log"
