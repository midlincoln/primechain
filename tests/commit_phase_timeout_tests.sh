#!/bin/sh
set -eu

client=$1
server=$2
commit_tool=$3
base=$4

rm -rf "$base"
mkdir -p "$base"

cleanup() {
    for pidfile in "$base"/*.pid; do
        [ -f "$pidfile" ] || continue
        kill "$(cat "$pidfile")" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

a=$("$client" new-miner "$base/a.wallet")
b=$("$client" new-miner "$base/b.wallet")
c=$("$client" new-miner "$base/c.wallet")

"$server" 19150 "$base/a.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/a.wallet" \
    --finalization-timeout-ms 100 \
    > "$base/a.log" 2>&1 &
echo $! > "$base/a.pid"
sleep 0.3

"$server" 19151 "$base/b.dat" \
    --peer 127.0.0.1 19150 \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$base/b.wallet" \
    --finalization-timeout-ms 100 \
    > "$base/b.log" 2>&1 &
echo $! > "$base/b.pid"
sleep 0.5
"$client" query 127.0.0.1 19150 ADD_PEER 127.0.0.1 19151 >/dev/null
sleep 0.2

commit=$("$commit_tool" sign-commit "$base/a.wallet" 3 1 3 44)
"$client" query 127.0.0.1 19150 $commit | grep -q '^COMMIT_ACCEPTED '
sleep 0.3

"$client" query 127.0.0.1 19150 CLOSE_COMMIT_PHASE 3 | grep -q 'votes=1$'
"$client" query 127.0.0.1 19151 CLOSE_COMMIT_PHASE 3 | grep -q 'votes=2$'
"$client" query 127.0.0.1 19150 GET_COMMIT_PHASE 3 | grep -q '^COMMIT_PHASE 3 CLOSED 2 '

"$client" query 127.0.0.1 19150 TIMEOUT_COMMIT_PHASE 3 | grep -q '^COMMIT_PHASE_TIMED_OUT 3$'
sleep 0.3

"$client" query 127.0.0.1 19150 GET_COMMIT_PHASE 3 | grep -q '^COMMIT_PHASE 3 OPEN 0 '
"$client" query 127.0.0.1 19151 GET_COMMIT_PHASE 3 | grep -q '^COMMIT_PHASE 3 OPEN 0 '
"$client" query 127.0.0.1 19150 GET_COMMITMENTS 3 | grep -q '^COMMITMENTS 3 0$'
"$client" query 127.0.0.1 19151 GET_COMMITMENTS 3 | grep -q '^COMMITMENTS 3 0$'
