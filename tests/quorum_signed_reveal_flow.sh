#!/bin/sh
set -eu

CLIENT=$1
SYNC_QUERY=$2
SYNC_SERVER=$3
WALLET=$4
COMMITMENT=$5
SEQUENTIAL=$6
PREFIX=$7
BASE_PORT=${8:-19357}
C_PORT=$BASE_PORT
B_PORT=$((BASE_PORT + 1))
A_PORT=$((BASE_PORT + 2))

cleanup() {
  for pidfile in "$PREFIX".pid "$PREFIX"-b.pid "$PREFIX"-c.pid; do
    if [ -f "$pidfile" ]; then
      kill "$(cat "$pidfile")" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT INT TERM

rm -f "$PREFIX"*
a=$($WALLET new-miner "$PREFIX"-a.wallet)
b=$($WALLET new-miner "$PREFIX"-b.wallet)
c=$($WALLET new-miner "$PREFIX"-c.wallet)

$SEQUENTIAL 3 "$PREFIX".log "$PREFIX".dat \
  --validator-set "$a" "$b" "$c" \
  --validator-identities "$PREFIX"-a.wallet "$PREFIX"-b.wallet >/dev/null

$SYNC_SERVER "$A_PORT" "$PREFIX".dat \
  --validator-set "$a" "$b" "$c" \
  --validator-identity "$PREFIX"-a.wallet \
  > "$PREFIX"-server.log 2>&1 &
echo $! > "$PREFIX".pid
sleep 0.4

$SYNC_SERVER "$B_PORT" "$PREFIX"-b.dat \
  --peer 127.0.0.1 "$A_PORT" \
  --validator-set "$a" "$b" "$c" \
  --validator-identity "$PREFIX"-b.wallet \
  > "$PREFIX"-server-b.log 2>&1 &
echo $! > "$PREFIX"-b.pid
sleep 0.5

$SYNC_SERVER "$C_PORT" "$PREFIX"-c.dat \
  --peer 127.0.0.1 "$A_PORT" \
  --peer 127.0.0.1 "$B_PORT" \
  --validator-set "$a" "$b" "$c" \
  --validator-identity "$PREFIX"-c.wallet \
  > "$PREFIX"-server-c.log 2>&1 &
echo $! > "$PREFIX"-c.pid
sleep 0.5

$SYNC_QUERY 127.0.0.1 "$A_PORT" ADD_PEER 127.0.0.1 "$B_PORT" >/dev/null
$SYNC_QUERY 127.0.0.1 "$A_PORT" ADD_PEER 127.0.0.1 "$C_PORT" >/dev/null

commit=$($COMMITMENT sign-commit "$PREFIX"-a.wallet 4 2 2 44)
reveal=$($COMMITMENT sign-reveal "$PREFIX"-a.wallet 4 2 2 44)

$SYNC_QUERY 127.0.0.1 "$A_PORT" $commit | grep -q '^COMMIT_ACCEPTED '
sleep 0.2
$SYNC_QUERY 127.0.0.1 "$A_PORT" $reveal | grep -q '^COMPOSITE_ACCEPTED 4 '
sleep 0.3
$SYNC_QUERY 127.0.0.1 "$A_PORT" GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4'
$SYNC_QUERY 127.0.0.1 "$B_PORT" GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4'

echo "quorum signed reveal flow passed"
