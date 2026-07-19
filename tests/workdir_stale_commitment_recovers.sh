#!/bin/sh
set -eu

client=$1
server=$2
commitment=$3
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

validator=$($client new-miner "$base/validator.wallet")

"$server" 19143 "$base/node.dat" \
    --validator-set "$validator" \
    --validator-identity "$base/validator.wallet" \
    > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" init-workdir "$base/work" 127.0.0.1 19143 > "$base/init.out"
"$client" sync-peer "$base/work" > "$base/sync.out"
"$client" add-mine-job "$base/work" --target 3 > "$base/add-3.out"
"$client" run-jobs "$base/work" > "$base/run-3.out" 2>&1

grep -q '^JOB_COMPLETE target=3 frontier=3$' "$base/run-3.out"

stale_commit=$("$commitment" sign-commit "$base/work/wallets/composite.wallet" 4 2 2 44)
"$client" query 127.0.0.1 19143 $stale_commit > "$base/stale-commit.out"
grep -q '^COMMIT_ACCEPTED 4 ' "$base/stale-commit.out"
rm -f "$base/work/jobs/pending-composite.state"

"$client" add-mine-job "$base/work" --target 4 > "$base/add-4.out"
"$client" run-jobs "$base/work" > "$base/run-4.out" 2>&1 || {
    cat "$base/run-4.out"
    exit 1
}
cat "$base/run-4.out"
grep -q 'provider already committed a different hash for integer 4' "$base/run-4.out"
grep -q '^JOB_COMPLETE target=4 frontier=4$' "$base/run-4.out"
"$client" status 127.0.0.1 19143 | grep -q '^STATUS 3 2 1 1 2 4 '
