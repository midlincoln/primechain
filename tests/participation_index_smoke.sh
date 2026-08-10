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

"$client" init-workdir "$base/work" 127.0.0.1 19346 > "$base/init.out"
prime_addr=$("$wallet" address "$base/work/wallets/prime.wallet")

"$sequential" 8 "$base/seed.log" "$base/seed.dat" --prime-miner "$prime_addr" > /dev/null

"$server" 19346 "$base/seed.dat" --enable-advance > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" sync-peer "$base/work" > "$base/sync.out"
grep -q '^SYNCED 2 8$' "$base/sync.out"

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

# Staleness: advance the chain without updating the index, verify every
# fast command refuses rather than serving outdated data.
"$client" query 127.0.0.1 19346 ADVANCE_TO 9 "$prime_addr" pcdev1_participation_composite 4 > /dev/null
"$client" sync-peer "$base/work" > /dev/null
for cmd in validator-endpoints-fast economic-policy-fast fee-distribution-status-fast validator-reward-distribution-status-fast
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
    "validator-reward-distribution-status:validator-reward-distribution-status-fast"
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
