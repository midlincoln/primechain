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
cp "$base/a.dat" "$base/c.dat"

"$server" 19141 "$base/a.dat" \
    --peer 127.0.0.1 19142 \
    --peer 127.0.0.1 19143 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"

"$server" 19142 "$base/b.dat" \
    --peer 127.0.0.1 19141 \
    --peer 127.0.0.1 19143 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"

"$server" 19143 "$base/c.dat" \
    --peer 127.0.0.1 19141 \
    --peer 127.0.0.1 19142 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/c.wallet" \
    > "$base/c.log" 2>&1 &
echo $! > "$base/c.pid"
sleep 0.8

reveal=$("$commitment" sign-reveal "$base/miner.wallet" 4 2 2 44)

"$client" query 127.0.0.1 19141 $reveal > "$base/final.out"
cat "$base/final.out"
if ! grep -Eq '^(COMPOSITE_ACCEPTED 4 |RECORD_DUPLICATE |RECORD_CONFLICT_WORSE )' "$base/final.out"; then
    echo "direct reveal was not accepted, duplicated, or superseded by an accepted record" >&2
    exit 1
fi
sleep 0.3

"$client" status 127.0.0.1 19141 > "$base/status-a.out"
cat "$base/status-a.out"
grep -q '^STATUS 3 2 1 1 2 4 ' "$base/status-a.out"
