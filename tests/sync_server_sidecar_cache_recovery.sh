#!/usr/bin/env bash
set -euo pipefail

client=${1:?client executable required}
server=${2:?sync server executable required}
base=${3:?test base directory required}

rm -rf "$base"
mkdir -p "$base"
chain="$base/chain.dat"
log="$base/server.log"
pid_file="$base/server.pid"
port=$((24000 + ($$ % 10000)))

cleanup() {
  if [ -f "$pid_file" ]; then
    kill "$(cat "$pid_file")" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for suffix in commitments phases epochs finalization rounds; do
  printf 'interrupted-sidecar' > "$chain.$suffix"
  printf 'interrupted-temp-sidecar' > "$chain.$suffix.tmp"
done

"$server" "$port" "$chain" > "$log" 2>&1 &
echo $! > "$pid_file"

ready=0
for _ in $(seq 1 40); do
  if "$client" status 127.0.0.1 "$port" > "$base/status.txt" 2>&1; then
    ready=1
    break
  fi
  if ! kill -0 "$(cat "$pid_file")" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if [ "$ready" -ne 1 ]; then
  cat "$log" >&2 || true
  cat "$base/status.txt" >&2 || true
  exit 1
fi

grep -q 'commitment store load warning: .*discarding volatile consensus cache' "$log"
grep -q 'phase store load warning: .*discarding volatile consensus cache' "$log"
grep -q 'validator epoch store load warning: .*discarding volatile consensus cache' "$log"
grep -q 'finalization store load warning: .*discarding volatile consensus cache' "$log"

for suffix in commitments phases epochs finalization rounds; do
  test ! -e "$chain.$suffix"
  test ! -e "$chain.$suffix.tmp"
done

"$client" status 127.0.0.1 "$port" | grep -q '^STATUS '

echo 'sync server sidecar cache recovery test passed'
