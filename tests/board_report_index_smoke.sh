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

export PRIMECHAIN_WALLET_PASSPHRASE="board-report-index-smoke-test"

"$client" init-workdir "$base/work" 127.0.0.1 19361 > "$base/init.out"
prime_addr=$("$wallet" address "$base/work/wallets/prime.wallet")

"$sequential" 20 "$base/seed.log" "$base/seed.dat" --prime-miner "$prime_addr" > /dev/null

"$server" 19361 "$base/seed.dat" --enable-advance > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" sync-peer "$base/work" > "$base/sync.out" 2>&1
grep -qE 'Synced.*start=2 end=20$' "$base/sync.out"

# board-report-workdir requires the reward index to cover at least the
# requested range (not necessarily caught up to the current frontier --
# a board report can legitimately ask about any historical window).
if "$client" board-report-workdir "$base/work" --from 2 --to 20 > "$base/missing.out" 2>&1; then
    echo "expected board-report-workdir to refuse without a reward index" >&2
    cat "$base/missing.out" >&2
    exit 1
fi
grep -q 'reward index is missing' "$base/missing.out"

"$client" update-reward-index "$base/work" > /dev/null

if "$client" board-report-workdir "$base/work" --from 2 --to 25 > "$base/behind.out" 2>&1; then
    echo "expected board-report-workdir to refuse a range past the reward index checkpoint" >&2
    cat "$base/behind.out" >&2
    exit 1
fi
grep -q 'reward index is behind the requested range' "$base/behind.out"

# Every range must match the full-replay original exactly (aside from the
# header echoing workdir vs. the record-store path) -- including windows
# that don't start at a composite/prime boundary, which is exactly where a
# naive findRange(from, to) swap (without falling back to the reward index
# for discovery_micro_units and pending_composites_after_range) would get
# the pending-provider carryover wrong.
for range in "2 20" "2 4" "3 6" "7 12" "9 9" "15 20"; do
    from=$(echo "$range" | cut -d' ' -f1)
    to=$(echo "$range" | cut -d' ' -f2)
    direct_out=$("$client" board-report "$base/work/data/chain.dat" --from "$from" --to "$to")
    fast_out=$("$client" board-report-workdir "$base/work" --from "$from" --to "$to")
    direct_body=$(echo "$direct_out" | tail -n +2)
    fast_body=$(echo "$fast_out" | tail -n +2)
    if [ "$direct_body" != "$fast_body" ]; then
        echo "range [$from,$to] mismatch:" >&2
        echo "--- direct ---" >&2
        echo "$direct_out" >&2
        echo "--- fast ---" >&2
        echo "$fast_out" >&2
        exit 1
    fi
done

full_report=$("$client" board-report-workdir "$base/work" --from 2 --to 20)
echo "$full_report"
echo "$full_report" | grep -q '^RECORDS total=19 prime=8 composite=11 transactions=0$'
echo "$full_report" | grep -qE '^MINER '"$prime_addr"' prime_records=7 composite_records=0 discovery_micro_units=[1-9][0-9]* fee_micro_units=0$'

# Incremental catch-up + divergence: reuse the same checks already proven
# by reward_index_workdir_smoke for update-reward-index itself, just
# confirming board-report-workdir keeps matching afterward.
"$client" query 127.0.0.1 19361 ADVANCE_TO 25 "$prime_addr" pcdev1_board_report_composite 4 > /dev/null
"$client" sync-peer "$base/work" > /dev/null
"$client" update-reward-index "$base/work" > /dev/null
direct_body2=$("$client" board-report "$base/work/data/chain.dat" --from 2 --to 25 | tail -n +2)
fast_body2=$("$client" board-report-workdir "$base/work" --from 2 --to 25 | tail -n +2)
[ "$direct_body2" = "$fast_body2" ]
