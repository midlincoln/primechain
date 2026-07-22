#!/bin/sh
set -eu

client=$1
server=$2
send=$3
commitment=$4
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

validator=$($client new-miner "$base/validator.wallet")

$server 19191 "$base/node.dat" \
    --validator-set "$validator" \
    --validator-identity "$base/validator.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/node.log" 2>&1 &
echo $! > "$base/node.pid"
sleep 0.3

$client init-workdir "$base/work" 127.0.0.1 19191 > "$base/init.out"
candidate=$($client address "$base/work/wallets/prime.wallet")

$client add-mine-job "$base/work" --target 31 > "$base/add-31.out"
$client run-jobs "$base/work" > "$base/mine-31.out" 2>&1
grep -q '^JOB_COMPLETE target=31 frontier=31$' "$base/mine-31.out"

nonce=1
for prime in 3 5 7 11 13 17 19 23 29; do
    amount=499999
    if [ "$prime" = "3" ]; then amount=999999; fi
    $send reserve-lock 127.0.0.1 19191 "$base/work/wallets/prime.wallet" "$candidate" "$prime" "$amount" 1 "$nonce" > "$base/lock-$prime.out"
    grep -q '^TX_ACCEPTED ' "$base/lock-$prime.out"
    nonce=$((nonce + 1))
done

$client add-mine-job "$base/work" --target 32 > "$base/add-32.out"
$client run-jobs "$base/work" > "$base/mine-32.out" 2>&1
grep -q '^JOB_COMPLETE target=32 frontier=32$' "$base/mine-32.out"
$client validator-reserve "$base/work/data/chain.dat" "$candidate" > "$base/reserve.out"
grep -q '^VALIDATOR_RESERVE .* total_micro_units=4999991$' "$base/reserve.out"

# Add one more tiny reserve lock to cross the 5,000,000 minimum.
$send reserve-lock 127.0.0.1 19191 "$base/work/wallets/prime.wallet" "$candidate" 31 9 1 "$nonce" > "$base/lock-31.out"
grep -q '^TX_ACCEPTED ' "$base/lock-31.out"

$client add-mine-job "$base/work" --target 33 > "$base/add-33.out"
$client run-jobs "$base/work" > "$base/mine-33.out" 2>&1
grep -q '^JOB_COMPLETE target=33 frontier=33$' "$base/mine-33.out"
$client validator-reserve "$base/work/data/chain.dat" "$candidate" > "$base/reserve-final.out"
grep -q '^VALIDATOR_RESERVE .* total_micro_units=5000000$' "$base/reserve-final.out"
$client validator-eligibility "$base/work/data/chain.dat" "$candidate" --reserve auto --observed 100 --total 100 > "$base/eligibility.out"
grep -q '^VALIDATOR_ELIGIBILITY .* eligible=1$' "$base/eligibility.out"

status=$($client status 127.0.0.1 19191)
previous=$(printf '%s\n' "$status" | awk '{print $8}')
record_integer=$(printf '%s\n' "$status" | awk '{print $7 + 1}')
application=$($commitment sign-application "$base/work/wallets/prime.wallet" "$previous" "$record_integer" 127.0.0.1 19192 1 100 100)
$client query 127.0.0.1 19191 $application > "$base/application.out"
grep -q '^VALIDATOR_APPLICATION_ACCEPTED ' "$base/application.out"
$client query 127.0.0.1 19191 GET_VALIDATOR_APPLICATIONS > "$base/applications.out"
grep -q "^VALIDATOR_APPLICATION $candidate " "$base/applications.out"

next_set=$(printf '%s\n%s\n' "$validator" "$candidate" | sort | tr '\n' ' ')
epoch_vote=$($commitment sign-epoch "$base/validator.wallet" "$previous" "$record_integer" 1 $next_set)
$client query 127.0.0.1 19191 $epoch_vote > "$base/epoch.out"
grep -q '^EPOCH_VOTE_ACCEPTED 1 votes=1$' "$base/epoch.out"

$client add-mine-job "$base/work" --target "$record_integer" > "$base/add-admission.out"
$client run-jobs "$base/work" > "$base/mine-admission.out" 2>&1
grep -q "^JOB_COMPLETE target=$record_integer frontier=$record_integer$" "$base/mine-admission.out"
$client query 127.0.0.1 19191 GET_VALIDATORS > "$base/validators.out"
grep -q "^VALIDATORS 2 " "$base/validators.out"

echo "candidate=$candidate"
cat "$base/reserve-final.out"
cat "$base/eligibility.out"
cat "$base/validators.out"
