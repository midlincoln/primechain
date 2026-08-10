#!/bin/sh
set -eu

client=$1
wallet=$2
sequential=$3
send=$4
server=$5
base=$6

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

export PRIMECHAIN_WALLET_PASSPHRASE="address-index-smoke-test"

"$wallet" new-miner "$base/miner.wallet" > /dev/null
"$wallet" new-miner "$base/alice.wallet" > /dev/null
miner=$("$wallet" address "$base/miner.wallet")
alice=$("$wallet" address "$base/alice.wallet")

"$sequential" 3 "$base/seed.log" "$base/seed.dat" --prime-miner "$miner" > /dev/null

"$server" 19320 "$base/seed.dat" --enable-advance \
    > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$send" submit 127.0.0.1 19320 "$base/miner.wallet" "$alice" 3 250000 1 | grep -q '^TX_ACCEPTED '

# Advance past the mempool tx so it lands in a real, finalized record.
advance_out=$("$client" query 127.0.0.1 19320 ADVANCE_TO 20 "$miner" pcdev1_address_index_composite 4)
echo "$advance_out"
echo "$advance_out" | grep -q '^ADVANCED .* included_txs=1 frontier=20$'

"$client" init-workdir "$base/work" 127.0.0.1 19320 > "$base/init.out"
"$client" sync-peer "$base/work" > "$base/sync.out"
grep -q '^SYNCED 2 20$' "$base/sync.out"

# Fresh index: full build from scratch (no checkpoint yet).
update_out=$("$client" update-address-index "$base/work")
echo "$update_out"
echo "$update_out" | grep -q '^ADDRESS_INDEX_UPDATED .* from=0 to=20 new_events=3 rebuilt=0$'

status_out=$("$client" address-index-status "$base/work")
echo "$status_out"
echo "$status_out" | grep -q '^ADDRESS_INDEX_STATUS .* checkpoint_integer=20 .* events=3 '

# The fast, index-backed path must match the full-replay path exactly
# (aside from the header echoing workdir vs. the record-store path).
direct_out=$("$client" wallet-history "$base/work/data/chain.dat" "$base/alice.wallet")
fast_out=$("$client" wallet-history-workdir "$base/work" "$base/alice.wallet")
direct_events=$(echo "$direct_out" | tail -n +2)
fast_events=$(echo "$fast_out" | tail -n +2)
[ "$direct_events" = "$fast_events" ]
echo "$fast_out" | grep -q '^WALLET_HISTORY .* events=1$'
echo "$fast_out" | grep -q '^TX_EVENT integer=4 height=2 kind=COMPOSITE .* direction=received .*'

# Advance the chain further without updating the index: the fast path must
# refuse rather than silently serve stale data.
"$client" query 127.0.0.1 19320 ADVANCE_TO 25 "$miner" pcdev1_address_index_composite_2 4 > /dev/null
"$client" sync-peer "$base/work" > /dev/null
if "$client" wallet-history-workdir "$base/work" "$base/alice.wallet" > "$base/stale.out" 2>&1; then
    echo "expected wallet-history-workdir to refuse a stale index" >&2
    cat "$base/stale.out" >&2
    exit 1
fi
grep -q 'address index is stale' "$base/stale.out"

# Incremental catch-up: only the newly-synced range should be (re)processed.
update_out2=$("$client" update-address-index "$base/work")
echo "$update_out2"
echo "$update_out2" | grep -q '^ADDRESS_INDEX_UPDATED .* from=20 to=25 new_events=0 rebuilt=0$'

# Corrupt the checkpoint hash to simulate divergence from the canonical
# store; the next update must detect it and rebuild from scratch rather
# than trusting (and incrementally extending) bad state.
sed -i.bak 's/checkpoint_hash=.*/checkpoint_hash=0000000000000000000000000000000000000000000000000000000000000000/' \
    "$base/work/indexes/address-index.meta"
rebuild_out=$("$client" update-address-index "$base/work")
echo "$rebuild_out"
echo "$rebuild_out" | grep -q '^ADDRESS_INDEX_UPDATED .* from=0 to=25 new_events=3 rebuilt=1$'

"$client" wallet-history-workdir "$base/work" "$base/alice.wallet" | grep -q '^WALLET_HISTORY .* events=1$'
