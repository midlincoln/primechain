#!/usr/bin/env bash
set -euo pipefail

client=$1
server=$2
wallet=$3
miner=$4
sync_query=$5
tmp=$6

cleanup() {
  if [ -f "$tmp-b.pid" ]; then
    kill "$(cat "$tmp-b.pid")" 2>/dev/null || true
  fi
}
trap cleanup EXIT

rm -f "$tmp"*

for attempt in $(seq 1 20); do
  rm -f "$tmp"*
  a=$("$wallet" new-miner "$tmp-a.wallet")
  b=$("$wallet" new-miner "$tmp-b.wallet")
  c=$("$wallet" new-miner "$tmp-c.wallet")

  ("$server" 19079 "$tmp-b.dat" \
    --validator-set "$a" "$b" "$c" \
    --validator-identity "$tmp-b.wallet" \
    > "$tmp-b.log" 2>&1 & echo $! > "$tmp-b.pid")
  sleep 0.2

  proposer_line=$("$sync_query" 127.0.0.1 19079 GET_FINALIZATION_PROPOSER)
  proposer=$(printf '%s\n' "$proposer_line" | awk '{print $4}')
  if [ "$proposer" != "$b" ]; then
    break
  fi

  cleanup
  rm -f "$tmp-b.pid"
done

if [ "$proposer" = "$b" ]; then
  echo "could not generate a non-proposer validator case" >&2
  exit 1
fi

if "$miner" 127.0.0.1 19079 3 \
    --prime-identity "$tmp-b.wallet" \
    --composite-identity "$tmp-b.wallet" \
    > "$tmp-b-miner.log" 2>&1; then
  echo "non-proposer miner unexpectedly finalized a record" >&2
  exit 1
fi

grep -q 'local validator is not assigned proposer for this finalization round' "$tmp-b-miner.log"
if [ -s "$tmp-b.dat.finalization" ]; then
  echo "non-proposer wrote a finalization sidecar vote" >&2
  exit 1
fi
