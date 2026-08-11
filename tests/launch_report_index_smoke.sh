#!/bin/sh
set -eu

client=$1
wallet=$2
sequential=$3
server=$4
base=$5

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

export PRIMECHAIN_WALLET_PASSPHRASE="launch-report-index-smoke-test"

# Real, signature-capable validator identities -- primechain-sequential's
# --validator-set requires every address to pass
# isProtocolSignatureAddress(), so this exercises the populated-genesis
# path (has_genesis=1, non-empty ACTIVE_VALIDATORS/VALIDATOR_RESERVE_SUMMARY)
# rather than the trivial empty-registry case.
"$wallet" new-miner "$base/val-a.wallet" > /dev/null
"$wallet" new-miner "$base/val-b.wallet" > /dev/null
"$wallet" new-miner "$base/val-c.wallet" > /dev/null
val_a=$("$wallet" address "$base/val-a.wallet")
val_b=$("$wallet" address "$base/val-b.wallet")
val_c=$("$wallet" address "$base/val-c.wallet")

"$sequential" 3 "$base/seed.log" "$base/seed.dat" \
    --prime-miner pcdev1_prime_miner \
    --validator-set "$val_a" "$val_b" "$val_c" \
    --validator-identities "$base/val-a.wallet" "$base/val-b.wallet" \
    > /dev/null

"$server" 19371 "$base/seed.dat" --enable-advance > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" init-workdir "$base/work" 127.0.0.1 19371 > "$base/init.out"
"$client" sync-peer "$base/work" > "$base/sync.out"

# launch-report-workdir needs both the reward index (for per-miner
# discovery/prime/composite counts and the pending-composite snapshot) and
# the participation index (for registry/endpoint/policy/vote data and the
# transaction-count running total) -- verify it refuses cleanly before
# either exists.
if "$client" launch-report-workdir "$base/work" > "$base/missing.out" 2>&1; then
    echo "expected launch-report-workdir to refuse without a reward index" >&2
    cat "$base/missing.out" >&2
    exit 1
fi
grep -q 'reward index is missing' "$base/missing.out"

"$client" update-reward-index "$base/work" > /dev/null

if "$client" launch-report-workdir "$base/work" > "$base/missing2.out" 2>&1; then
    echo "expected launch-report-workdir to refuse without a participation index" >&2
    cat "$base/missing2.out" >&2
    exit 1
fi
grep -q 'participation index is missing' "$base/missing2.out"

"$client" update-participation-index "$base/work" > /dev/null

direct_out=$("$client" launch-report "$base/work/data/chain.dat")
fast_out=$("$client" launch-report-workdir "$base/work")
direct_body=$(echo "$direct_out" | tail -n +2)
fast_body=$(echo "$fast_out" | tail -n +2)
if [ "$direct_body" != "$fast_body" ]; then
    echo "launch-report / launch-report-workdir mismatch:" >&2
    echo "--- direct ---" >&2
    echo "$direct_out" >&2
    echo "--- fast ---" >&2
    echo "$fast_out" >&2
    exit 1
fi

echo "$fast_out"
echo "$fast_out" | grep -q '^CHAIN has_genesis=1 height=1 frontier=3 .* records=2 prime_records=2 composite_records=0 transactions=0$'
echo "$fast_out" | grep -q '^VALIDATOR_STATE epoch=0 active_validators=3 registry_events=1 '
echo "$fast_out" | grep -q '^ACTIVE_VALIDATORS '
echo "$fast_out" | grep -c '^VALIDATOR_RESERVE_SUMMARY ' | grep -q '^3$'

# Staleness: corrupt the participation index checkpoint hash (finalizing
# further via ADVANCE_TO isn't reachable here -- this fixture's genesis
# configures a real 3-validator quorum, and the single dev sync-server
# isn't one of them, so it can't finalize on its own). Confirms
# launch-report-workdir refuses rather than serving stale data, and that
# the next update rebuilds and converges back to the same correct report.
sed -i.bak 's/checkpoint_hash=.*/checkpoint_hash=0000000000000000000000000000000000000000000000000000000000000000/' \
    "$base/work/indexes/participation-index.meta"
if "$client" launch-report-workdir "$base/work" > "$base/stale.out" 2>&1; then
    echo "expected launch-report-workdir to refuse a stale participation index" >&2
    cat "$base/stale.out" >&2
    exit 1
fi
grep -q 'participation index is stale' "$base/stale.out"

"$client" update-participation-index "$base/work" > /dev/null
direct_body2=$("$client" launch-report "$base/work/data/chain.dat" | tail -n +2)
fast_body2=$("$client" launch-report-workdir "$base/work" | tail -n +2)
[ "$direct_body2" = "$fast_body2" ]
