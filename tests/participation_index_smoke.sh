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

export PRIMECHAIN_WALLET_PASSPHRASE="participation-index-smoke-test"

# Real, signature-capable wallets for the populated-genesis fixture used
# near the end of this script -- primechain-sequential's --validator-set
# requires every address in it to pass isProtocolSignatureAddress(), so
# placeholder strings like "pcdev1_validator_x" don't qualify here (unlike
# the dev-bootstrap addresses that show up automatically as finalization
# voters elsewhere in this fixture).
"$wallet" new-miner "$base/reg-val-a.wallet" > /dev/null
"$wallet" new-miner "$base/reg-val-b.wallet" > /dev/null
"$wallet" new-miner "$base/reg-val-c.wallet" > /dev/null
reg_val_a=$("$wallet" address "$base/reg-val-a.wallet")
reg_val_b=$("$wallet" address "$base/reg-val-b.wallet")
reg_val_c=$("$wallet" address "$base/reg-val-c.wallet")

"$client" init-workdir "$base/work" 127.0.0.1 19346 > "$base/init.out"
prime_addr=$("$wallet" address "$base/work/wallets/prime.wallet")

"$sequential" 8 "$base/seed.log" "$base/seed.dat" --prime-miner "$prime_addr" > /dev/null

"$server" 19346 "$base/seed.dat" --enable-advance > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" sync-peer "$base/work" > "$base/sync.out" 2>&1
grep -qE 'Synced.*start=2 end=8$' "$base/sync.out"

# Fresh build. validator-reputation-fast also reads the reward index (for
# MINING_HISTORY), so keep both current throughout this test.
"$client" update-reward-index "$base/work" > /dev/null
update_out=$("$client" update-participation-index "$base/work")
echo "$update_out"
echo "$update_out" | grep -q '^PARTICIPATION_INDEX_UPDATED .* from=0 to=8 .* rebuilt=0$'

status_out=$("$client" participation-index-status "$base/work")
echo "$status_out"
echo "$status_out" | grep -q '^PARTICIPATION_INDEX_STATUS .* checkpoint_integer=8 .* prime_record_count=4 '

# Every -fast command must match its full-replay original exactly (aside
# from the header echoing workdir vs. the record-store path).
for pair in \
    "validator-endpoints:validator-endpoints-fast" \
    "economic-policy:economic-policy-fast" \
    "fee-distribution-status:fee-distribution-status-fast" \
    "validator-reward-distribution-status:validator-reward-distribution-status-fast"
do
    orig=${pair%%:*}
    fast=${pair##*:}
    direct_out=$("$client" $orig "$base/work/data/chain.dat")
    fast_out=$("$client" $fast "$base/work")
    direct_body=$(echo "$direct_out" | tail -n +2)
    fast_body=$(echo "$fast_out" | tail -n +2)
    if [ "$direct_body" != "$fast_body" ]; then
        echo "$orig / $fast mismatch:" >&2
        echo "--- direct ---" >&2
        echo "$direct_out" >&2
        echo "--- fast ---" >&2
        echo "$fast_out" >&2
        exit 1
    fi
done

direct_rep=$("$client" validator-reputation "$base/work/data/chain.dat" "$prime_addr")
fast_rep=$("$client" validator-reputation-fast "$base/work" "$prime_addr")
[ "$direct_rep" = "$fast_rep" ]
echo "$fast_rep" | grep -q '^MINING_HISTORY prime_records=3 composite_records=0 work_score=[0-9]* discovery_micro_units=2000000 fee_micro_units=0$'
# The dev-bootstrap validator set votes on every record's finalization in
# this fixture; confirm the vote-counting path actually saw real votes
# rather than trivially matching on an all-zero report.
bootstrap_rep_direct=$("$client" validator-reputation "$base/work/data/chain.dat" pcdev1_validator_a)
bootstrap_rep_fast=$("$client" validator-reputation-fast "$base/work" pcdev1_validator_a)
[ "$bootstrap_rep_direct" = "$bootstrap_rep_fast" ]
echo "$bootstrap_rep_fast" | grep -qE '^VALIDATOR_PARTICIPATION finalization_votes=[1-9][0-9]* '

# validator-registry-fast: reconstructs the same genesis/epoch-transition
# state loadValidatorRegistry() would, by replaying registry-genesis
# events out of this index instead of calling that core-library function.
# This fixture's genesis has no formal validator_set (only --prime-miner
# was given to primechain-sequential), so this exercises the empty-
# registry path; a real populated genesis is covered separately below.
direct_registry=$("$client" validator-registry "$base/work/data/chain.dat")
fast_registry=$("$client" validator-registry-fast "$base/work")
direct_registry_body=$(echo "$direct_registry" | tail -n +2)
fast_registry_body=$(echo "$fast_registry" | tail -n +2)
[ "$direct_registry_body" = "$fast_registry_body" ]
echo "$fast_registry" | grep -q '^VALIDATOR_REGISTRY .* has_genesis=0 current_epoch=0 active_validators=0 events=0$'

# Staleness: advance the chain without updating the index, verify every
# fast command refuses rather than serving outdated data.
"$client" query 127.0.0.1 19346 ADVANCE_TO 9 "$prime_addr" pcdev1_participation_composite 4 > /dev/null
"$client" sync-peer "$base/work" > /dev/null
for cmd in validator-endpoints-fast economic-policy-fast fee-distribution-status-fast validator-reward-distribution-status-fast validator-registry-fast
do
    if "$client" $cmd "$base/work" > "$base/stale-$cmd.out" 2>&1; then
        echo "expected $cmd to refuse a stale index" >&2
        cat "$base/stale-$cmd.out" >&2
        exit 1
    fi
    grep -q 'participation index is stale' "$base/stale-$cmd.out"
done
if "$client" validator-reputation-fast "$base/work" "$prime_addr" > "$base/stale-rep.out" 2>&1; then
    echo "expected validator-reputation-fast to refuse a stale index" >&2
    cat "$base/stale-rep.out" >&2
    exit 1
fi

# Incremental catch-up: prime_record_count must keep accumulating (not
# reset), and every -fast command must still match its original after the
# new range is folded in.
"$client" update-reward-index "$base/work" > /dev/null
update_out2=$("$client" update-participation-index "$base/work")
echo "$update_out2"
echo "$update_out2" | grep -qE '^PARTICIPATION_INDEX_UPDATED .* from=8 to=[0-9]+ .* rebuilt=0$'

for pair in \
    "validator-endpoints:validator-endpoints-fast" \
    "economic-policy:economic-policy-fast" \
    "fee-distribution-status:fee-distribution-status-fast" \
    "validator-reward-distribution-status:validator-reward-distribution-status-fast" \
    "validator-registry:validator-registry-fast"
do
    orig=${pair%%:*}
    fast=${pair##*:}
    direct_body=$("$client" $orig "$base/work/data/chain.dat" | tail -n +2)
    fast_body=$("$client" $fast "$base/work" | tail -n +2)
    [ "$direct_body" = "$fast_body" ]
done

# Divergence: corrupt the checkpoint hash and confirm the next update
# rebuilds from scratch (prime_record_count included) and converges back
# to the same correct totals.
sed -i.bak 's/checkpoint_hash=.*/checkpoint_hash=0000000000000000000000000000000000000000000000000000000000000000/' \
    "$base/work/indexes/participation-index.meta"
rebuild_out=$("$client" update-participation-index "$base/work")
echo "$rebuild_out"
echo "$rebuild_out" | grep -q '^PARTICIPATION_INDEX_UPDATED .* from=0 .* rebuilt=1$'

rebuilt_bootstrap=$("$client" validator-reputation-fast "$base/work" pcdev1_validator_a)
[ "$rebuilt_bootstrap" = "$("$client" validator-reputation "$base/work/data/chain.dat" pcdev1_validator_a)" ]

rebuilt_registry=$("$client" validator-registry-fast "$base/work")
[ "$rebuilt_registry" = "$("$client" validator-registry "$base/work/data/chain.dat" | sed "s#$base/work/data/chain.dat#$base/work#")" ]

# validator-registry-fast with an actual populated genesis: the fixture
# above never exercises the has_genesis=1 / active_validators>0 path,
# since it only passed --prime-miner to primechain-sequential. Build a
# second, separate chain with a real 3-validator genesis config to cover
# appendRegistryGenesisEvent's non-trivial branch.
"$sequential" 3 "$base/registry-seed.log" "$base/registry-seed.dat" \
    --prime-miner pcdev1_prime_miner \
    --validator-set "$reg_val_a" "$reg_val_b" "$reg_val_c" \
    --validator-identities "$base/reg-val-a.wallet" "$base/reg-val-b.wallet" \
    > /dev/null

"$server" 19347 "$base/registry-seed.dat" --enable-advance > "$base/registry-server.log" 2>&1 &
echo $! > "$base/registry-server.pid"
sleep 0.3

"$client" init-workdir "$base/registry-work" 127.0.0.1 19347 > "$base/registry-init.out"
"$client" sync-peer "$base/registry-work" > "$base/registry-sync.out"
"$client" update-participation-index "$base/registry-work" > /dev/null

direct_genesis=$("$client" validator-registry "$base/registry-work/data/chain.dat")
fast_genesis=$("$client" validator-registry-fast "$base/registry-work")
direct_genesis_body=$(echo "$direct_genesis" | tail -n +2)
fast_genesis_body=$(echo "$fast_genesis" | tail -n +2)
[ "$direct_genesis_body" = "$fast_genesis_body" ]
echo "$fast_genesis" | grep -q '^VALIDATOR_REGISTRY .* has_genesis=1 current_epoch=0 active_validators=3 events=1$'
echo "$fast_genesis" | grep -q '^VALIDATOR_REGISTRY_EVENT GENESIS height=0 integer=2 epoch=0 activation_integer=2 validators=3 '
