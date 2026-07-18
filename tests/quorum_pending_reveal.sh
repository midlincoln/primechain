#!/bin/sh
set -eu

client=$1
server=$2
commitment=$3
sequential=$4
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

new_miner() {
    "$client" new-miner "$1"
}

a=$(new_miner "$base/a.wallet")
b=$(new_miner "$base/b.wallet")
c=$(new_miner "$base/c.wallet")
miner=$(new_miner "$base/miner.wallet")

"$sequential" 3 "$base/bootstrap.log" "$base/a.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identities "$base/a.wallet" "$base/b.wallet" >/dev/null
cp "$base/a.dat" "$base/b.dat"

"$server" 19141 "$base/a.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"

"$server" 19142 "$base/b.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"
sleep 0.4

"$client" query 127.0.0.1 19141 ADD_PEER 127.0.0.1 19142 >/dev/null
"$client" query 127.0.0.1 19142 ADD_PEER 127.0.0.1 19141 >/dev/null

commit=$("$commitment" sign-commit "$base/miner.wallet" 4 2 2 44)
reveal=$("$commitment" sign-reveal "$base/miner.wallet" 4 2 2 44)

"$client" query 127.0.0.1 19141 $reveal > "$base/pending.out"
cat "$base/pending.out"
grep -q '^REVEAL_PENDING 4 awaiting_commitment$' "$base/pending.out"

"$client" query 127.0.0.1 19142 $commit > "$base/commit.out"
cat "$base/commit.out"
grep -q '^COMMIT_ACCEPTED 4 ' "$base/commit.out"
sleep 0.3

"$client" query 127.0.0.1 19141 $reveal > "$base/final.out"
cat "$base/final.out"
grep -q '^COMPOSITE_ACCEPTED 4 ' "$base/final.out"

"$client" status 127.0.0.1 19142 > "$base/status-b.out"
cat "$base/status-b.out"
grep -q 'STATUS 3 2 1 1 2 4 ' "$base/status-b.out"
