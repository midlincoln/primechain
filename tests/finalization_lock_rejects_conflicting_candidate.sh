#!/bin/sh
set -eu

CLIENT=$1
SYNC_SERVER=$2
WALLET=$3
PREFIX=$4
BASE_PORT=${5:-19180}
A_PORT=$BASE_PORT
B_PORT=$((BASE_PORT + 1))

cleanup() {
  for pidfile in "$PREFIX"-a.pid "$PREFIX"-b.pid; do
    if [ -f "$pidfile" ]; then
      kill "$(cat "$pidfile")" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT INT TERM

attempt=1
while [ "$attempt" -le 20 ]; do
  cleanup
  rm -f "$PREFIX"*

  a=$($WALLET new-miner "$PREFIX"-a.wallet)
  b=$($WALLET new-miner "$PREFIX"-b.wallet)
  c=$($WALLET new-miner "$PREFIX"-c.wallet)

  # Start validator B alone. If B is assigned for integer 3 it can sign its
  # candidate, but cannot finalize without quorum. That retained signature is
  # the safety lock this test needs.
  "$SYNC_SERVER" "$B_PORT" "$PREFIX"-b.dat \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$PREFIX"-b.wallet \
    > "$PREFIX"-b.log 2>&1 &
  echo $! > "$PREFIX"-b.pid
  sleep 0.3

  if "$CLIENT" mine 127.0.0.1 "$B_PORT" 3 \
      --prime-identity "$PREFIX"-b.wallet \
      --composite-identity "$PREFIX"-b.wallet \
      > "$PREFIX"-b-miner.log 2>&1; then
    echo "expected isolated validator mining to fail before quorum finalization" >&2
    exit 1
  fi

  if [ -s "$PREFIX"-b.dat.finalization ]; then
    break
  fi

  attempt=$((attempt + 1))
done

if [ ! -s "$PREFIX"-b.dat.finalization ]; then
  echo "could not generate an isolated signed-candidate lock" >&2
  exit 1
fi

# Start validator A from B. A first attempts a different provider candidate. B
# must reject that conflicting candidate, then round-change must converge both
# nodes onto one canonical tip.
"$SYNC_SERVER" "$A_PORT" "$PREFIX"-a.dat \
  --peer 127.0.0.1 "$B_PORT" \
  --finalization-timeout-ms 50 \
  --validator-set "$a" "$b" "$c" \
  --validator-identity "$PREFIX"-a.wallet \
  > "$PREFIX"-a.log 2>&1 &
echo $! > "$PREFIX"-a.pid
sleep 0.5

"$CLIENT" mine 127.0.0.1 "$A_PORT" 3 \
  --prime-identity "$PREFIX"-a.wallet \
  --composite-identity "$PREFIX"-a.wallet \
  > "$PREFIX"-a-miner.log 2>&1

grep -q 'validator already signed a different candidate for this previous hash and integer' "$PREFIX"-a.log
grep -q 'frontier miner complete frontier=3' "$PREFIX"-a-miner.log

a_hash=$($CLIENT inspect "$PREFIX"-a.dat | awk '/latest_record_hash:/ {print $2}')
b_hash=$($CLIENT inspect "$PREFIX"-b.dat | awk '/latest_record_hash:/ {print $2}')
test "$a_hash" = "$b_hash"

echo "finalization lock rejects conflicting candidate and recovers one canonical tip"
