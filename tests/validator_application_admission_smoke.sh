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

$server 19191 "$base/node.dat"     --validator-set "$validator"     --validator-identity "$base/validator.wallet"     --finalization-timeout-ms 500     > "$base/node.log" 2>&1 &
echo $! > "$base/node.pid"
sleep 0.3

$client init-workdir "$base/work" 127.0.0.1 19191 > "$base/init.out"
miner=$($client address "$base/work/wallets/prime.wallet")
candidate=$($client new-miner "$base/candidate.wallet")

$client add-mine-job "$base/work" --target 37 > "$base/add-37.out"
$client run-jobs "$base/work" > "$base/mine-37.out" 2>&1
grep -q '^JOB_COMPLETE target=37 frontier=37$' "$base/mine-37.out"

nonce=1
for prime in 3 5 7 11 13 17 19 23 29; do
    amount=449999
    if [ "$prime" = "3" ]; then amount=899999; fi
    $send reserve-lock 127.0.0.1 19191 "$base/work/wallets/prime.wallet" "$candidate" "$prime" "$amount" 1 "$nonce" > "$base/lock-$prime.out"
    grep -q '^TX_ACCEPTED ' "$base/lock-$prime.out"
    nonce=$((nonce + 1))
done

$client add-mine-job "$base/work" --target 38 > "$base/add-38.out"
$client run-jobs "$base/work" > "$base/mine-38.out" 2>&1
grep -q '^JOB_COMPLETE target=38 frontier=38$' "$base/mine-38.out"
$client validator-reserve "$base/work/data/chain.dat" "$candidate" > "$base/reserve.out"
grep -q '^VALIDATOR_RESERVE .* total_micro_units=4499991$' "$base/reserve.out"

$send reserve-lock 127.0.0.1 19191 "$base/work/wallets/prime.wallet" "$candidate" 31 449999 1 "$nonce" > "$base/lock-31.out"
grep -q '^TX_ACCEPTED ' "$base/lock-31.out"
nonce=$((nonce + 1))
$send reserve-lock 127.0.0.1 19191 "$base/work/wallets/prime.wallet" "$candidate" 37 50010 1 "$nonce" > "$base/lock-37.out"
grep -q '^TX_ACCEPTED ' "$base/lock-37.out"

$client add-mine-job "$base/work" --target 39 > "$base/add-39.out"
$client run-jobs "$base/work" > "$base/mine-39.out" 2>&1
grep -q '^JOB_COMPLETE target=39 frontier=39$' "$base/mine-39.out"
$client validator-reserve "$base/work/data/chain.dat" "$candidate" > "$base/reserve-final.out"
grep -q '^VALIDATOR_RESERVE .* total_micro_units=5000000$' "$base/reserve-final.out"
$client validator-eligibility "$base/work/data/chain.dat" "$candidate" --reserve auto --observed 100 --total 100 > "$base/eligibility-before-binding.out"
grep -q '^VALIDATOR_ELIGIBILITY .* eligible=0$' "$base/eligibility-before-binding.out"

status=$($client status 127.0.0.1 19191)
previous=$(printf '%s
' "$status" | awk '{print $8}')
record_integer=$(printf '%s
' "$status" | awk '{print $7 + 1}')

application=$($commitment sign-application "$base/candidate.wallet" "$previous" "$record_integer" 127.0.0.1 19192 1 100 100)
$client query 127.0.0.1 19191 $application > "$base/application.out"
grep -q '^VALIDATOR_APPLICATION_ACCEPTED ' "$base/application.out"
$client query 127.0.0.1 19191 GET_VALIDATOR_APPLICATIONS > "$base/applications.out"
grep -q "^VALIDATOR_APPLICATION $candidate " "$base/applications.out"

work_binding=$($commitment sign-work-binding "$base/work/wallets/prime.wallet" "$previous" "$record_integer" "$candidate" 1)
$client query 127.0.0.1 19191 $work_binding > "$base/work-binding.out"
grep -q '^VALIDATOR_WORK_BINDING_ACCEPTED ' "$base/work-binding.out"
$client query 127.0.0.1 19191 GET_VALIDATOR_WORK_BINDINGS > "$base/work-bindings.out"
grep -q "^VALIDATOR_WORK_BINDING $candidate $miner " "$base/work-bindings.out"

next_set=$(printf '%s
%s
' "$validator" "$candidate" | sort | tr '
' ' ')
epoch_vote=$($commitment sign-epoch "$base/validator.wallet" "$previous" "$record_integer" 1 $next_set)
$client query 127.0.0.1 19191 $epoch_vote > "$base/epoch.out"
grep -q '^EPOCH_VOTE_ACCEPTED 1 votes=1$' "$base/epoch.out"

$client add-mine-job "$base/work" --target "$record_integer" > "$base/add-admission.out"
$client run-jobs "$base/work" > "$base/mine-admission.out" 2>&1
grep -q "^JOB_COMPLETE target=$record_integer frontier=$record_integer$" "$base/mine-admission.out"
$client query 127.0.0.1 19191 GET_VALIDATORS > "$base/validators.out"
grep -q "^VALIDATORS 2 " "$base/validators.out"

$client sync 127.0.0.1 19191 2 "$record_integer" "$base/node2.dat" > "$base/sync-node2.out"
$server 19192 "$base/node2.dat" \
    --peer 127.0.0.1 19191 \
    --validator-set "$validator" \
    --validator-identity "$base/candidate.wallet" \
    --finalization-timeout-ms 500 \
    > "$base/node2.log" 2>&1 &
echo $! > "$base/node2.pid"
sleep 0.5
$client query 127.0.0.1 19191 ADD_PEER 127.0.0.1 19192 > "$base/add-peer-node1.out"
$client query 127.0.0.1 19192 ADD_PEER 127.0.0.1 19191 > "$base/add-peer-node2.out"

first_post_admission_prime=$((record_integer + 1))
$client add-mine-job "$base/work" --target "$first_post_admission_prime" > "$base/add-post-admission-prime.out"
$client run-jobs "$base/work" > "$base/mine-post-admission-prime.out" 2>&1
grep -q "^JOB_COMPLETE target=$first_post_admission_prime frontier=$first_post_admission_prime$" "$base/mine-post-admission-prime.out"

first_post_admission_composite=$((record_integer + 2))
commit_42=$($commitment sign-commit "$base/work/wallets/composite.wallet" "$first_post_admission_composite" 2 21 4242)
$client query 127.0.0.1 19191 $commit_42 > "$base/commit-42.out"
grep -q "^COMMIT_ACCEPTED $first_post_admission_composite " "$base/commit-42.out"
$client query 127.0.0.1 19191 CLOSE_COMMIT_PHASE "$first_post_admission_composite" > "$base/close-42-a.out"
grep -q "^PHASE_VOTE_ACCEPTED $first_post_admission_composite " "$base/close-42-a.out"
$client query 127.0.0.1 19191 $commit_42 > "$base/commit-42-duplicate-closing.out"
grep -q "^COMMIT_DUPLICATE $first_post_admission_composite " "$base/commit-42-duplicate-closing.out"

# Mine through the first post-admission composite. With two active validators this
# exercises quorum commit-phase close, commitment propagation, and retry idempotency.
post_admission_target=$first_post_admission_composite
$client add-mine-job "$base/work" --target "$post_admission_target" > "$base/add-post-admission.out"
$client run-jobs "$base/work" > "$base/mine-post-admission.out" 2>&1
grep -q "^JOB_COMPLETE target=$post_admission_target frontier=$post_admission_target$" "$base/mine-post-admission.out"

$client sync-peer "$base/work" > "$base/sync-final.out"
$client inspect "$base/work/data/chain.dat" > "$base/inspect-final-work.out"
$client inspect "$base/node.dat" > "$base/inspect-final-node.out"
$client inspect "$base/node2.dat" > "$base/inspect-final-node2.out"
grep -q "frontier_integer: $post_admission_target" "$base/inspect-final-work.out"
grep -q "frontier_integer: $post_admission_target" "$base/inspect-final-node.out"
grep -q "frontier_integer: $post_admission_target" "$base/inspect-final-node2.out"
$client validator-eligibility "$base/work/data/chain.dat" "$candidate" --reserve auto --observed 100 --total 100 > "$base/eligibility.out"
grep -q '^VALIDATOR_ELIGIBILITY .* eligible=1$' "$base/eligibility.out"
grep -q "^WORK_SPONSORS count=1 $miner$" "$base/eligibility.out"

echo "miner=$miner"
echo "candidate=$candidate"
cat "$base/reserve-final.out"
cat "$base/eligibility.out"
cat "$base/validators.out"
