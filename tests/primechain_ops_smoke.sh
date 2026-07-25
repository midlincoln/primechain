#!/usr/bin/env bash
set -euo pipefail

ops="${1:?primechain-ops path required}"
build_dir="${2:?build dir required}"
tmp="${build_dir}/primechain-ops-smoke"

rm -rf "$tmp"
mkdir -p "$tmp/bin" "$tmp/data" "$tmp/work"

cat > "$tmp/bin/primechain-sync-server" <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--help" ]; then
  echo "usage: primechain-sync-server [--bootstrap-peer host port] [--genesis-validator-set addr] [--use-chain-endpoints]"
  exit 0
fi
exit 0
EOF
chmod +x "$tmp/bin/primechain-sync-server"

touch "$tmp/validator.wallet" "$tmp/wallet.env"

unit="$("$ops" print-validator-service \
  --binary "$tmp/bin/primechain-sync-server" \
  --data "$tmp/data/chain.dat" \
  --working-dir "$tmp/work" \
  --env-file "$tmp/wallet.env" \
  --identity "$tmp/validator.wallet" \
  --genesis-validator pcpq1_genesis \
  --bootstrap 192.0.2.10:8339 \
  --port 8339)"

echo "$unit" | grep -q '^ExecStart='
echo "$unit" | grep -q -- '--bind 0.0.0.0 --bootstrap-peer 192.0.2.10 8339'
echo "$unit" | grep -q -- '--genesis-validator-set pcpq1_genesis'
echo "$unit" | grep -q -- "--validator-identity $tmp/validator.wallet"
echo "$unit" | grep -q -- '--use-chain-endpoints'
echo "$unit" | grep -q -- '--finalization-timeout-ms 5000'

if echo "$unit" | grep -q -- '--use-chain- endpoints'; then
  echo "split --use-chain-endpoints flag found" >&2
  exit 1
fi

"$ops" install-validator-service \
  --binary "$tmp/bin/primechain-sync-server" \
  --client "$tmp/bin/missing-client" \
  --data "$tmp/data/chain.dat" \
  --working-dir "$tmp/work" \
  --env-file "$tmp/wallet.env" \
  --identity "$tmp/validator.wallet" \
  --genesis-validator pcpq1_genesis \
  --no-restart \
  --dry-run >/dev/null


cat > "$tmp/bin/primechain-client" <<'EOF'
#!/usr/bin/env bash
cmd="${1:-}"
host="${2:-}"
port="${3:-}"
state_file="$(dirname "$0")/../tx-included"
case "$cmd" in
  status)
    echo "STATUS 122 30 92 1 121 123 abc123"
    ;;
  sync-peer)
    echo "SYNC_UP_TO_DATE 123"
    ;;
  new-miner)
    echo "wallet" > "$2"
    ;;
  address)
    case "$2" in
      *sender.wallet) echo "pcpq1_sender" ;;
      *receiver.wallet) echo "pcpq1_receiver" ;;
      *) echo "pcpq1_wallet" ;;
    esac
    ;;
  balance)
    wallet="$3"
    echo "address: $("$0" address "$wallet")"
    if [ -f "$state_file" ]; then
      case "$wallet" in
        *sender.wallet) echo "holdings: 1"; echo "3 999899" ;;
        *receiver.wallet) echo "holdings: 1"; echo "3 100" ;;
        *) echo "holdings: 0" ;;
      esac
    else
      case "$wallet" in
        *sender.wallet) echo "holdings: 1"; echo "3 1000000" ;;
        *) echo "holdings: 0" ;;
      esac
    fi
    ;;
  fee-pool)
    if [ -f "$state_file" ]; then
      echo "VALIDATOR_FEE_POOL $2 epoch=2 address=pcpool_validator_fees_epoch_2 holdings=1 total_micro_units=1"
      echo "FEE_POOL_HOLDING epoch=2 prime=3 micro_units=1"
    else
      echo "VALIDATOR_FEE_POOL $2 epoch=2 address=pcpool_validator_fees_epoch_2 holdings=0 total_micro_units=0"
    fi
    ;;
  fee-distribution-status)
    echo "FEE_DISTRIBUTION_STATUS $2 interval_records=${3:-1000} current_frontier=123 last_distribution_integer=23 next_distribution_integer=1023 due=0 current_epoch=2 pool_address=pcpool_validator_fees_epoch_2 pool_holdings=0 pool_total_micro_units=0 distributions=1"
    echo "LAST_FEE_DISTRIBUTION integer=23 epoch=2 prime=101 micro_units=3"
    echo "FEE_DISTRIBUTION_EVENT integer=23 epoch=2 prime=101 micro_units=3 recipients=3"
    ;;
  add-mine-job)
    echo "MINE_JOB_ADDED $2 target=$4"
    ;;
  run-jobs)
    touch "$state_file"
    echo "JOB_COMPLETE target=124 frontier=124"
    ;;
  launch-report)
    echo "LAUNCH_REPORT $2"
    echo "CHAIN has_genesis=1 height=121 frontier=123 latest_hash=abc123 records=122 prime_records=30 composite_records=92 transactions=21"
    echo "VALIDATOR_STATE epoch=2 active_validators=2 registry_events=2 endpoint_events=2 active_endpoints=2 transfer_fee_micro_units=1"
    echo "VALIDATOR_EVIDENCE_SUMMARY active=2 historical=0 bootstrap_dev=2"
    echo "ACTIVE_VALIDATORS pcpq1_a pcpq1_b"
    echo "VALIDATOR_ENDPOINT pcpq1_a host=192.0.2.10 port=8339 effective_integer=101 sequence=1"
    echo "VALIDATOR_ENDPOINT pcpq1_b host=192.0.2.11 port=8339 effective_integer=101 sequence=1"
    echo "VALIDATOR_RESERVE_SUMMARY pcpq1_a admission=genesis holdings=0 total_micro_units=0"
    echo "VALIDATOR_RESERVE_SUMMARY pcpq1_b admission=reserve holdings=10 total_micro_units=5000000"
    echo "ECONOMIC_POLICY active_transfer_fee_micro_units=1 events=0"
    echo "VALIDATOR_FEE_POOL epoch=2 address=pcpool_validator_fees_epoch_2 holdings=0 total_micro_units=0"
    echo "BOARD records=122 prime=30 composite=92 transactions=21 discovery_micro_units=30000000 fee_micro_units=21 unique_miners=3 pending_composites_after_range=1"
    echo "VALIDATOR_EVIDENCE pcpq1_a class=active finalization_votes=121 commit_phase_votes=92 round_change_votes=0"
    echo "VALIDATOR_EVIDENCE pcpq1_b class=active finalization_votes=89 commit_phase_votes=70 round_change_votes=0"
    ;;
  query)
    q="${4:-}"
    case "$q" in
      GET_NONCE)
        echo "NONCE ${5:-pcpq1_sender} 0 1"
        ;;
      GET_MEMPOOL)
        echo "MEMPOOL 0"
        ;;
      GET_VALIDATORS)
        echo "VALIDATORS 2 pcpq1_a pcpq1_b"
        ;;
      GET_VALIDATOR_EPOCH)
        echo "VALIDATOR_EPOCH 1 124 abc123"
        ;;
      GET_VALIDATOR_ENDPOINTS)
        echo "VALIDATOR_ENDPOINTS 2"
        echo "VALIDATOR_ENDPOINT pcpq1_a 192.0.2.10 8339 101 1"
        echo "VALIDATOR_ENDPOINT pcpq1_b 192.0.2.11 8339 101 1"
        echo "END_VALIDATOR_ENDPOINTS"
        ;;
      GET_PEERS)
        echo "PEERS 1"
        if [ "$host" = "192.0.2.10" ]; then
          echo "PEER 192.0.2.11 8339"
        else
          echo "PEER 192.0.2.10 8339"
        fi
        echo "END_PEERS"
        ;;
      *)
        echo "unexpected query $q" >&2
        exit 1
        ;;
    esac
    ;;
  *)
    echo "unexpected command $cmd" >&2
    exit 1
    ;;
esac
EOF
chmod +x "$tmp/bin/primechain-client"

cat > "$tmp/bin/primechain-send" <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "submit" ]; then
  echo "TX_ACCEPTED tx-smoke-123"
  exit 0
fi
echo "unexpected send command $*" >&2
exit 1
EOF
chmod +x "$tmp/bin/primechain-send"

"$ops" doctor-network \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 | grep -q '^NETWORK_OK validators=2 frontier=123 hash=abc123$'


mkdir -p "$tmp/workdir/data"
touch "$tmp/workdir/data/chain.dat"
"$ops" evidence \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 \
  --output "$tmp/evidence.txt" | grep -q '^EVIDENCE_REPORT '

grep -q '^PRIMECHAIN_EVIDENCE generated_at=' "$tmp/evidence.txt"
grep -q '^RESULT sync-peer OK$' "$tmp/evidence.txt"
grep -q '^RESULT launch-report OK$' "$tmp/evidence.txt"
grep -q '^NETWORK_OK validators=2 frontier=123 hash=abc123$' "$tmp/evidence.txt"
grep -q '^RESULT doctor-network OK$' "$tmp/evidence.txt"


"$ops" launch-summary \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 \
  --output "$tmp/summary.txt" | grep -q '^LAUNCH_SUMMARY_REPORT '

grep -q '^PRIMECHAIN_LAUNCH_SUMMARY generated_at=' "$tmp/summary.txt"
grep -q '^NETWORK validators=2 frontier=123 hash=abc123$' "$tmp/summary.txt"
grep -q '^VALIDATOR_EVIDENCE_SUMMARY active=2 historical=0 bootstrap_dev=2$' "$tmp/summary.txt"
grep -q '^VALIDATOR pcpq1_b admission=reserve holdings=10 total_micro_units=5000000 host=192.0.2.11 port=8339 effective_integer=101 sequence=1 class=active finalization_votes=89 commit_phase_votes=70 round_change_votes=0$' "$tmp/summary.txt"


"$ops" fee-distribution-status \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --interval-records 1000 > "$tmp/fee-distribution-status.txt"

grep -q '^FEE_DISTRIBUTION_STATUS .* interval_records=1000 current_frontier=123 last_distribution_integer=23 next_distribution_integer=1023 due=0 .* pool_total_micro_units=0 distributions=1$' "$tmp/fee-distribution-status.txt"
grep -q '^LAST_FEE_DISTRIBUTION integer=23 epoch=2 prime=101 micro_units=3$' "$tmp/fee-distribution-status.txt"


touch "$tmp/sender.wallet"
rm -f "$tmp/tx-included"
"$ops" transaction-evidence \
  --client "$tmp/bin/primechain-client" \
  --send "$tmp/bin/primechain-send" \
  --workdir "$tmp/workdir" \
  --sender-wallet "$tmp/sender.wallet" \
  --receiver-wallet "$tmp/receiver.wallet" \
  --prime 3 \
  --amount 100 \
  --fee 1 \
  --target 124 \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 \
  --output "$tmp/tx-evidence.txt" | grep -q '^TRANSACTION_EVIDENCE_REPORT '

grep -q '^TRANSFER tx_hash=tx-smoke-123 prime=3 amount_micro_units=100 fee_micro_units=1 nonce=1 sender=pcpq1_sender receiver=pcpq1_receiver$' "$tmp/tx-evidence.txt"
grep -q '^BALANCE_CHECK role=sender before=1000000 after=999899 expected_after=999899 pass=1$' "$tmp/tx-evidence.txt"
grep -q '^BALANCE_CHECK role=receiver before=0 after=100 expected_after=100 pass=1$' "$tmp/tx-evidence.txt"
grep -q '^FEE_POOL_CHECK before=0 after=1 expected_after=1 pass=1$' "$tmp/tx-evidence.txt"
grep -q '^RESULT transaction-evidence OK$' "$tmp/tx-evidence.txt"
