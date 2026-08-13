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

export PRIMECHAIN_WALLET_PASSPHRASE="reward-index-smoke-test"

"$client" init-workdir "$base/work" 127.0.0.1 19345 > "$base/init.out"
prime_addr=$("$wallet" address "$base/work/wallets/prime.wallet")
composite_addr=$("$wallet" address "$base/work/wallets/composite.wallet")

# Seed a chain where the workdir's own prime wallet is the miner -- the
# default composite-miner placeholder is fine here since what we're
# exercising is the reward split/pending-carryover logic, not signing.
"$sequential" 8 "$base/seed.log" "$base/seed.dat" --prime-miner "$prime_addr" > /dev/null

"$server" 19345 "$base/seed.dat" --enable-advance > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" sync-peer "$base/work" > "$base/sync.out" 2>&1
grep -qE 'Synced.*start=2 end=8$' "$base/sync.out"

# Fresh reward index: full build from scratch. The seed ends on a composite
# record with no following prime record yet, so it should land with an
# outstanding pending provider.
update_out=$("$client" update-reward-index "$base/work")
echo "$update_out"
echo "$update_out" | grep -q '^REWARD_INDEX_UPDATED .* from=0 to=8 .* rebuilt=0$'

status_out=$("$client" reward-index-status "$base/work")
echo "$status_out"
echo "$status_out" | grep -q '^REWARD_INDEX_STATUS .* checkpoint_integer=8 .* pending=1 '

# rewards-fast / reward-history-fast must match the full-replay originals
# exactly, including the prime wallet's non-zero accrued reward and the
# still-nonzero PENDING_COMPOSITE_RECORDS count.
direct_rewards=$("$client" rewards "$base/work")
fast_rewards=$("$client" rewards-fast "$base/work")
[ "$direct_rewards" = "$fast_rewards" ]
echo "$fast_rewards" | grep -q '^PRIME_WALLET '"$prime_addr"' records=3 reward_micro_units=2000000$'
echo "$fast_rewards" | grep -q '^PENDING_COMPOSITE_RECORDS 1$'

direct_history=$("$client" reward-history "$base/work")
fast_history=$("$client" reward-history-fast "$base/work")
[ "$direct_history" = "$fast_history" ]

# Staleness: advance the chain without updating the index, verify both
# fast commands refuse rather than serving outdated totals.
"$client" add-mine-job "$base/work" --target 9 > /dev/null
"$client" run-jobs "$base/work" > "$base/run.out" 2>&1 || true
cat "$base/run.out"
"$client" sync-peer "$base/work" > /dev/null

if "$client" rewards-fast "$base/work" > "$base/stale-rewards.out" 2>&1; then
    echo "expected rewards-fast to refuse a stale index" >&2
    cat "$base/stale-rewards.out" >&2
    exit 1
fi
grep -q 'reward index is stale' "$base/stale-rewards.out"

if "$client" reward-history-fast "$base/work" > "$base/stale-history.out" 2>&1; then
    echo "expected reward-history-fast to refuse a stale index" >&2
    cat "$base/stale-history.out" >&2
    exit 1
fi
grep -q 'reward index is stale' "$base/stale-history.out"

# Incremental catch-up across a pending-provider carryover: this must
# resume the split correctly (not lose track of the composite indexed in
# the previous run) rather than only reprocessing the newly-synced range
# in isolation.
update_out2=$("$client" update-reward-index "$base/work")
echo "$update_out2"
echo "$update_out2" | grep -qE '^REWARD_INDEX_UPDATED .* from=8 to=[0-9]+ .* rebuilt=0$'

diff_rewards=$("$client" rewards "$base/work")
diff_fast_rewards=$("$client" rewards-fast "$base/work")
[ "$diff_rewards" = "$diff_fast_rewards" ]

diff_history=$("$client" reward-history "$base/work")
diff_fast_history=$("$client" reward-history-fast "$base/work")
[ "$diff_history" = "$diff_fast_history" ]

# Divergence: corrupt the checkpoint hash and confirm the next update
# detects it and rebuilds from scratch rather than extending bad state,
# converging back to the same correct totals.
sed -i.bak 's/checkpoint_hash=.*/checkpoint_hash=0000000000000000000000000000000000000000000000000000000000000000/' \
    "$base/work/indexes/reward-index.meta"
rebuild_out=$("$client" update-reward-index "$base/work")
echo "$rebuild_out"
echo "$rebuild_out" | grep -q '^REWARD_INDEX_UPDATED .* from=0 .* rebuilt=1$'

rebuilt_rewards=$("$client" rewards-fast "$base/work")
[ "$rebuilt_rewards" = "$diff_fast_rewards" ]
