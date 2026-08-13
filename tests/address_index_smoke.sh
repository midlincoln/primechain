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
"$client" sync-peer "$base/work" > "$base/sync.out" 2>&1
grep -qE 'Synced.*start=2 end=20$' "$base/sync.out"

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

# record / latest-records: rewritten to seek via RecordStore's existing
# offset index (findByInteger / findRange) instead of decoding the whole
# chain just to find one record or the tail end of it. No new index
# involved here -- verify byte-identical output against loadAll-derived
# expectations captured by hand from the known fixture.
record_out=$("$client" record "$base/work/data/chain.dat" 4)
echo "$record_out"
echo "$record_out" | grep -q '^RECORD integer=4 height=2 kind=COMPOSITE .* frontier=25 confirmations=22 provider=pcdev1_address_index_composite txs=1 '
echo "$record_out" | grep -q '^RECORD_TX .* sender='"$miner"' inputs=1 outputs=1 fee_prime=3 fee_micro_units=1 fee_denominator=1$'

latest_out=$("$client" latest-records "$base/work/data/chain.dat" --last 3)
echo "$latest_out"
echo "$latest_out" | grep -q '^LATEST_RECORDS .* frontier=25 records=24 showing=3$'
[ "$(echo "$latest_out" | grep -c '^RECORD integer=')" -eq 3 ]
echo "$latest_out" | grep -q '^RECORD integer=25 height=23 kind=COMPOSITE '

# address-report-workdir: the event/totals half comes from the index; the
# holdings/balance half still goes through SequentialNode::load() (ledger
# state isn't something a plain event log can serve) -- must still match
# the original full-replay address-report exactly.
direct_ar=$("$client" address-report "$base/work/data/chain.dat" "$alice")
fast_ar=$("$client" address-report-workdir "$base/work" "$alice")
direct_ar_body=$(echo "$direct_ar" | tail -n +2)
fast_ar_body=$(echo "$fast_ar" | tail -n +2)
[ "$direct_ar_body" = "$fast_ar_body" ]
echo "$fast_ar" | grep -q '^ADDRESS_REPORT .* address='"$alice"' frontier=25 holdings=1 total_micro_units=250000 transactions=1 events=1 sent_micro_units=0 received_micro_units=250000 fee_micro_units=0$'

# tx-workdir: finds the record via the address index's tx_hash field
# (every transaction has at least its sender indexed) instead of a linear
# scan, then reloads and re-decodes just that one record for display.
tx_hash=$(echo "$fast_ar" | grep -oE 'tx_hash=[0-9a-f]+' | head -1 | cut -d= -f2)
direct_tx=$("$client" tx "$base/work/data/chain.dat" "$tx_hash")
fast_tx=$("$client" tx-workdir "$base/work" "$tx_hash")
direct_tx_body=$(echo "$direct_tx" | tail -n +2)
fast_tx_body=$(echo "$fast_tx" | tail -n +2)
[ "$direct_tx_body" = "$fast_tx_body" ]
echo "$fast_tx" | grep -q '^TX_FOUND '"$tx_hash"' .* integer=4 .*'

# tx-workdir on an unknown hash must report not-found rather than crash or
# hang, and must exit non-zero.
if "$client" tx-workdir "$base/work" "0000000000000000000000000000000000000000000000000000000000000000" \
    > "$base/tx-missing.out" 2>&1; then
    echo "expected tx-workdir to fail for an unknown hash" >&2
    cat "$base/tx-missing.out" >&2
    exit 1
fi
grep -q '^TX_NOT_FOUND ' "$base/tx-missing.out"
