#!/bin/sh
set -eu

client=$1
server=$2
base=$3

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

"$server" 19120 "$base/chain.dat" > "$base/server.log" 2>&1 &
echo $! > "$base/server.pid"
sleep 0.3

"$client" init-workdir "$base/work-a" 127.0.0.1 19120 > "$base/init-a.out"
"$client" init-workdir "$base/work-b" 127.0.0.1 19120 > "$base/init-b.out"
"$client" add-mine-job "$base/work-a" --target 30 > "$base/add-a.out"
"$client" add-mine-job "$base/work-b" --target 30 > "$base/add-b.out"

"$client" run-jobs "$base/work-a" > "$base/run-a.out" 2>&1 &
echo $! > "$base/run-a.pid"
"$client" run-jobs "$base/work-b" > "$base/run-b.out" 2>&1 &
echo $! > "$base/run-b.pid"

wait "$(cat "$base/run-a.pid")"
wait "$(cat "$base/run-b.pid")"

cat "$base/run-a.out"
cat "$base/run-b.out"
grep -q '^JOB_COMPLETE target=30 frontier=30$' "$base/run-a.out"
grep -q '^JOB_COMPLETE target=30 frontier=30$' "$base/run-b.out"
"$client" status 127.0.0.1 19120 | grep -q '^STATUS 29 10 19 1 28 30 '
