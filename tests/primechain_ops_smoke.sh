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
  echo "usage: primechain-sync-server [--bootstrap-peer host port] [--genesis-validator-set addr] [--use-chain-endpoints] [--allow-remote-admin]"
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
  version)
    echo "VERSION name=primechain version=0.1.0 git_commit=testcommit build_time=2026-01-01T00:00:00Z protocol=1 network=launch-testnet-1"
    ;;
  status)
    echo "STATUS 122 30 92 1 121 123 abc123"
    ;;
  sync-peer)
    echo "SYNC_UP_TO_DATE 123"
    ;;
  sync)
    echo "recovered-chain" > "$6"
    echo "sync download complete"
    echo "output_store: $6"
    echo "records: 122"
    echo "start: $4"
    echo "end: $5"
    echo "frontier_integer: $5"
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
  wallet-history)
    echo "WALLET_HISTORY $2 wallet=$3 address=pcpq1_sender events=2"
    echo "TX_EVENT integer=4 height=2 kind=COMPOSITE confirmations=2 direction=sent tx_hash=tx-smoke-123 version=2 nonce=1 prime=3 amount_micro_units=100 amount_denominator=1 sender=pcpq1_sender receiver=pcpq1_receiver"
    echo "TX_EVENT integer=4 height=2 kind=COMPOSITE confirmations=2 direction=fee-paid tx_hash=tx-smoke-123 version=2 nonce=1 prime=3 amount_micro_units=1 amount_denominator=1 sender=pcpq1_sender receiver=validator-fee-pool"
    ;;
  wallet-pending)
    echo "WALLET_PENDING $2:$3 wallet=$4 address=pcpq1_sender mempool=1 transactions=1 events=2"
    echo "PENDING_TX direction=sent tx_hash=tx-pending-123 version=2 nonce=2 prime=3 amount_micro_units=50 amount_denominator=1 sender=pcpq1_sender receiver=pcpq1_receiver"
    echo "PENDING_TX direction=fee-paid tx_hash=tx-pending-123 version=2 nonce=2 prime=3 amount_micro_units=1 amount_denominator=1 sender=pcpq1_sender receiver=validator-fee-pool"
    ;;
  record)
    echo "RECORD integer=$3 height=2 kind=COMPOSITE hash=abc123 frontier=5 confirmations=2 provider=pcpq1_sender txs=1 finalization_votes=2 commit_phase_votes=2 round_changes=0"
    echo "COMPOSITE_PROOF integer=$3 divisor=2 cofactor=2"
    echo "COMMIT_PHASE integer=$3 commitments=1 votes=2 validators=2"
    echo "RECORD_TX tx_hash=tx-smoke-123 version=2 nonce=1 sender=pcpq1_sender inputs=1 outputs=1 fee_prime=3 fee_micro_units=1 fee_denominator=1"
    ;;
  latest-records)
    echo "LATEST_RECORDS $2 frontier=5 records=4 showing=${4:-20}"
    echo "RECORD integer=4 height=2 kind=COMPOSITE hash=abc123 frontier=5 confirmations=2 provider=pcpq1_sender txs=1 finalization_votes=2 commit_phase_votes=2 round_changes=0"
    echo "RECORD integer=5 height=3 kind=PRIME hash=def456 frontier=5 confirmations=1 provider=pcpq1_sender txs=0 finalization_votes=2 commit_phase_votes=0 round_changes=0"
    ;;
  tx)
    echo "TX_FOUND $3 store=$2 integer=4 height=2 kind=COMPOSITE frontier=5 confirmations=2 version=2 nonce=1 sender=pcpq1_sender"
    echo "TX_INPUTS count=1"
    echo "TX_INPUT prime=3 amount_micro_units=101 amount_denominator=1"
    echo "TX_OUTPUTS count=1"
    echo "TX_OUTPUT prime=3 amount_micro_units=100 amount_denominator=1 receiver=pcpq1_receiver"
    echo "TX_FEE prime=3 amount_micro_units=1 amount_denominator=1"
    ;;
  address-report)
    echo "ADDRESS_REPORT $2 address=$3 frontier=5 holdings=1 total_micro_units=999899 transactions=1 events=2 sent_micro_units=100 received_micro_units=0 fee_micro_units=1"
    echo "ADDRESS_HOLDING address=$3 prime=3 micro_units=999899"
    echo "ADDRESS_TX integer=4 height=2 kind=COMPOSITE confirmations=2 direction=sent tx_hash=tx-smoke-123 version=2 nonce=1 prime=3 amount_micro_units=100 amount_denominator=1 sender=pcpq1_sender receiver=pcpq1_receiver"
    echo "ADDRESS_TX integer=4 height=2 kind=COMPOSITE confirmations=2 direction=fee-paid tx_hash=tx-smoke-123 version=2 nonce=1 prime=3 amount_micro_units=1 amount_denominator=1 sender=pcpq1_sender receiver=validator-fee-pool"
    ;;
  inspect)
    echo "record store inspection"
    echo "store_path: $2"
    echo "records: 122"
    echo "prime_records: 30"
    echo "composite_records: 92"
    echo "has_genesis: yes"
    echo "height: 121"
    echo "frontier_integer: 123"
    echo "latest_record_hash: abc123"
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
    echo "VALIDATOR_STATE epoch=2 active_validators=2 registry_events=2 endpoint_events=2 active_endpoints=2 transfer_fee_micro_units=1 validator_min_reserve_micro_units=5000000"
    echo "VALIDATOR_EVIDENCE_SUMMARY active=2 historical=0 bootstrap_dev=2"
    echo "ACTIVE_VALIDATORS pcpq1_a pcpq1_b"
    echo "VALIDATOR_ENDPOINT pcpq1_a host=192.0.2.10 port=8339 effective_integer=101 sequence=1"
    echo "VALIDATOR_ENDPOINT pcpq1_b host=192.0.2.11 port=8339 effective_integer=101 sequence=1"
    echo "VALIDATOR_RESERVE_SUMMARY pcpq1_a admission=genesis holdings=0 total_micro_units=0"
    echo "VALIDATOR_RESERVE_SUMMARY pcpq1_b admission=reserve holdings=10 total_micro_units=5000000"
    echo "ECONOMIC_POLICY active_transfer_fee_micro_units=1 active_validator_min_reserve_micro_units=5000000 events=0"
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
      GET_VERSION)
        echo "VERSION name=primechain version=0.1.0 git_commit=testcommit build_time=2026-01-01T00:00:00Z protocol=1 network=launch-testnet-1"
        ;;
      GET_MEMPOOL)
        echo "MEMPOOL 0"
        echo "END_MEMPOOL"
        ;;
      GET_MEMPOOL_SUMMARY)
        echo "MEMPOOL_SUMMARY transactions=0 max_transactions=1000 max_per_sender=25 max_age_seconds=3600 unique_senders=0 total_input_micro_units=0 total_output_micro_units=0 total_fee_micro_units=0 oldest_age_seconds=0 newest_age_seconds=0 active_peers=1"
        echo "END_MEMPOOL_SUMMARY"
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
      GET_PEER_HEALTH)
        echo "PEER_HEALTH 1 local_frontier=123 local_hash=abc123"
        if [ "$host" = "192.0.2.10" ]; then
          echo "PEER_HEALTH_ENTRY host=192.0.2.11 port=8339 reachable=1 failures=0 quarantined=0 last_success=1 last_failure=0 last_error=none has_genesis=1 frontier=123 height=121 hash=abc123 hash_match=1 frontier_delta=0 peer_list_ok=1 peer_count=1"
        else
          echo "PEER_HEALTH_ENTRY host=192.0.2.10 port=8339 reachable=1 failures=0 quarantined=0 last_success=1 last_failure=0 last_error=none has_genesis=1 frontier=123 height=121 hash=abc123 hash_match=1 frontier_delta=0 peer_list_ok=1 peer_count=1"
        fi
        echo "END_PEER_HEALTH"
        ;;
      GET_PEER_STATE)
        echo "PEER_STATE path=/tmp/chain.dat.peers peers=1 quarantine_threshold=3"
        if [ "$host" = "192.0.2.10" ]; then
          echo "PEER_STATE_ENTRY host=192.0.2.11 port=8339 failures=0 quarantined=0 last_success=1 last_failure=0 last_error=none"
        else
          echo "PEER_STATE_ENTRY host=192.0.2.10 port=8339 failures=0 quarantined=0 last_success=1 last_failure=0 last_error=none"
        fi
        echo "END_PEER_STATE"
        ;;
      RESET_PEER_STATE)
        echo "PEER_STATE_RESET 1"
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

"$ops" version-network \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 > "$tmp/version-network.txt"

grep -q '^LOCAL VERSION name=primechain version=0.1.0 git_commit=testcommit ' "$tmp/version-network.txt"
grep -q '^NODE_VERSION 192.0.2.10:8339 VERSION name=primechain version=0.1.0 git_commit=testcommit ' "$tmp/version-network.txt"
grep -q '^VERSION_NETWORK_OK validators=2$' "$tmp/version-network.txt"

"$ops" peer-health \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 > "$tmp/peer-health.txt"

grep -q '^NODE_PEER_HEALTH 192.0.2.10:8339$' "$tmp/peer-health.txt"
grep -q '^PEER_HEALTH_ENTRY host=192.0.2.11 port=8339 reachable=1 .* hash_match=1 ' "$tmp/peer-health.txt"
grep -q '^PEER_HEALTH_OK validators=2$' "$tmp/peer-health.txt"


"$ops" peer-state \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 > "$tmp/peer-state.txt"

grep -q '^NODE_PEER_STATE 192.0.2.10:8339$' "$tmp/peer-state.txt"
grep -q '^PEER_STATE_ENTRY host=192.0.2.11 port=8339 failures=0 quarantined=0 ' "$tmp/peer-state.txt"
grep -q '^PEER_STATE_OK validators=2$' "$tmp/peer-state.txt"

"$ops" peer-state \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --reset-peer 192.0.2.11:8339 > "$tmp/peer-state-reset.txt"

grep -q '^PEER_STATE_RESET 1$' "$tmp/peer-state-reset.txt"
grep -q '^PEER_STATE_OK validators=1$' "$tmp/peer-state-reset.txt"

"$ops" mempool-health \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 > "$tmp/mempool-health.txt"

grep -q '^NODE_MEMPOOL 192.0.2.10:8339$' "$tmp/mempool-health.txt"
grep -q '^MEMPOOL_SUMMARY transactions=0 ' "$tmp/mempool-health.txt"
grep -q '^MEMPOOL_HEALTH_OK validators=2$' "$tmp/mempool-health.txt"

"$ops" mempool-network \
  --client "$tmp/bin/primechain-client" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 > "$tmp/mempool-network.txt"

grep -q '^NODE_MEMPOOL_SET 192.0.2.10:8339$' "$tmp/mempool-network.txt"
grep -q '^MEMPOOL_SET count=0 hashes=none$' "$tmp/mempool-network.txt"
grep -q '^MEMPOOL_NETWORK_OK validators=2$' "$tmp/mempool-network.txt"

mkdir -p "$tmp/workdir/data"
touch "$tmp/workdir/data/chain.dat"
"$ops" chain-doctor \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" > "$tmp/chain-doctor.txt"

grep -q '^CHAIN_DOCTOR .*chain.dat$' "$tmp/chain-doctor.txt"
grep -q '^STORE_SUMMARY records=122 prime_records=30 composite_records=92 has_genesis=yes height=121 frontier=123 latest_hash=abc123$' "$tmp/chain-doctor.txt"
grep -q '^OK sequential-validation$' "$tmp/chain-doctor.txt"
grep -q '^CHAIN_DOCTOR_OK$' "$tmp/chain-doctor.txt"

echo "old-chain" > "$tmp/workdir/data/chain.dat"
"$ops" chain-recover \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 > "$tmp/chain-recover.txt"

grep -q '^RECOVERY_SOURCE 192.0.2.10:8339 frontier=123 hash=abc123 agreement=2$' "$tmp/chain-recover.txt"
grep -q '^RESULT download OK candidate=' "$tmp/chain-recover.txt"
grep -q '^RECOVERY_INSTALLED .*chain.dat backup_dir=' "$tmp/chain-recover.txt"
grep -q '^CHAIN_RECOVER_OK$' "$tmp/chain-recover.txt"
grep -q '^recovered-chain$' "$tmp/workdir/data/chain.dat"
ls "$tmp/workdir"/recovery-backup-*/chain.dat >/dev/null

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

"$ops" release-check \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 \
  --interval-records 1000 > "$tmp/release-check.txt"

grep -q '^PRIMECHAIN_RELEASE_CHECK generated_at=' "$tmp/release-check.txt"
grep -q '^SECTION version-network$' "$tmp/release-check.txt"
grep -q '^RESULT version-network OK$' "$tmp/release-check.txt"
grep -q '^RESULT doctor-network OK$' "$tmp/release-check.txt"
grep -q '^RESULT mempool-network OK$' "$tmp/release-check.txt"
grep -q '^RESULT chain-doctor OK$' "$tmp/release-check.txt"
grep -q '^RESULT fee-distribution-status OK$' "$tmp/release-check.txt"
grep -q '^RESULT launch-summary OK$' "$tmp/release-check.txt"
grep -q '^RELEASE_CHECK_OK validators=2$' "$tmp/release-check.txt"


"$ops" launch-status-json \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 \
  --interval-records 1000 \
  --output "$tmp/status.json" | grep -q '^LAUNCH_STATUS_JSON '

python3 -m json.tool "$tmp/status.json" >/dev/null
grep -q '"frontier": 123' "$tmp/status.json"
grep -q '"next_distribution_integer": 1023' "$tmp/status.json"
grep -q '"address": "pcpq1_b"' "$tmp/status.json"


touch "$tmp/sender.wallet"
rm -f "$tmp/tx-included"
"$ops" wallet-summary \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --validator 192.0.2.10:8339 > "$tmp/wallet-summary.txt"

grep -q '^PRIMECHAIN_WALLET_SUMMARY generated_at=' "$tmp/wallet-summary.txt"
grep -q '^ADDRESS pcpq1_sender$' "$tmp/wallet-summary.txt"
grep -q '^CHAIN height=121 frontier=123 hash=abc123 records=122 prime_records=30 composite_records=92$' "$tmp/wallet-summary.txt"
grep -q '^BALANCE holdings=1 total_micro_units=1000000$' "$tmp/wallet-summary.txt"
grep -q '^HOLDING prime=3 micro_units=1000000$' "$tmp/wallet-summary.txt"
grep -q '^NONCE validator=192.0.2.10:8339 confirmed=0 next=1$' "$tmp/wallet-summary.txt"


"$ops" wallet-history \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --last 1 > "$tmp/wallet-history.txt"

grep -q '^WALLET_HISTORY .* address=pcpq1_sender events=2$' "$tmp/wallet-history.txt"
grep -q '^TX_EVENT .* direction=sent .* prime=3 amount_micro_units=100 ' "$tmp/wallet-history.txt"
grep -q '^TX_EVENT .* direction=fee-paid .* receiver=validator-fee-pool$' "$tmp/wallet-history.txt"


"$ops" wallet-pending \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --validator 192.0.2.10:8339 > "$tmp/wallet-pending.txt"

grep -q '^WALLET_PENDING 192.0.2.10:8339 .* mempool=1 transactions=1 events=2$' "$tmp/wallet-pending.txt"
grep -q '^PENDING_TX direction=sent tx_hash=tx-pending-123 ' "$tmp/wallet-pending.txt"


"$ops" wallet-dashboard \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --validator 192.0.2.10:8339 \
  --last 1 > "$tmp/wallet-dashboard.txt"

grep -q '^PRIMECHAIN_WALLET_DASHBOARD generated_at=' "$tmp/wallet-dashboard.txt"
grep -q '^PRIMECHAIN_WALLET_SUMMARY generated_at=' "$tmp/wallet-dashboard.txt"
grep -q '^WALLET_PENDING 192.0.2.10:8339 ' "$tmp/wallet-dashboard.txt"
grep -q '^WALLET_HISTORY .* address=pcpq1_sender events=2$' "$tmp/wallet-dashboard.txt"


"$ops" explorer-dashboard \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --validator 192.0.2.10:8339 \
  --validator 192.0.2.11:8339 \
  --last-records 2 \
  --last-wallet-events 1 \
  --interval-records 1000 > "$tmp/explorer-dashboard.txt"

grep -q '^PRIMECHAIN_EXPLORER_DASHBOARD generated_at=' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION network-health$' "$tmp/explorer-dashboard.txt"
grep -q '^NETWORK_OK validators=2 frontier=123 hash=abc123$' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION chain-report$' "$tmp/explorer-dashboard.txt"
grep -q '^CHAIN has_genesis=1 height=121 frontier=123 latest_hash=abc123 records=122 prime_records=30 composite_records=92 transactions=21$' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION fee-distribution$' "$tmp/explorer-dashboard.txt"
grep -q '^FEE_DISTRIBUTION_STATUS .* interval_records=1000 ' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION latest-records$' "$tmp/explorer-dashboard.txt"
grep -q '^LATEST_RECORDS .* showing=2$' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION wallet-summary$' "$tmp/explorer-dashboard.txt"
grep -q '^PRIMECHAIN_WALLET_SUMMARY generated_at=' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION wallet-pending$' "$tmp/explorer-dashboard.txt"
grep -q '^WALLET_PENDING 192.0.2.10:8339 ' "$tmp/explorer-dashboard.txt"
grep -q '^SECTION wallet-history$' "$tmp/explorer-dashboard.txt"
grep -q '^WALLET_HISTORY .* address=pcpq1_sender events=2$' "$tmp/explorer-dashboard.txt"


"$ops" record \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --integer 4 > "$tmp/record.txt"

grep -q '^RECORD integer=4 ' "$tmp/record.txt"
grep -q '^COMPOSITE_PROOF integer=4 divisor=2 cofactor=2$' "$tmp/record.txt"


"$ops" latest-records \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --last 2 > "$tmp/latest-records.txt"

grep -q '^LATEST_RECORDS .* showing=2$' "$tmp/latest-records.txt"
grep -q '^RECORD integer=5 .* kind=PRIME ' "$tmp/latest-records.txt"


"$ops" tx \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --hash tx-smoke-123 > "$tmp/tx-lookup.txt"

grep -q '^TX_FOUND tx-smoke-123 .* confirmations=2 .* sender=pcpq1_sender$' "$tmp/tx-lookup.txt"
grep -q '^TX_OUTPUT prime=3 amount_micro_units=100 amount_denominator=1 receiver=pcpq1_receiver$' "$tmp/tx-lookup.txt"


"$ops" address-report \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --address pcpq1_sender \
  --last 1 > "$tmp/address-report.txt"

grep -q '^ADDRESS_REPORT .* address=pcpq1_sender .* transactions=1 events=2 sent_micro_units=100 received_micro_units=0 fee_micro_units=1$' "$tmp/address-report.txt"
grep -q '^ADDRESS_TX .* confirmations=2 direction=sent tx_hash=tx-smoke-123 ' "$tmp/address-report.txt"


"$ops" wallet-receive \
  --client "$tmp/bin/primechain-client" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --prime 3 \
  --amount 100 > "$tmp/wallet-receive.txt"

grep -q '^PRIMECHAIN_WALLET_RECEIVE generated_at=' "$tmp/wallet-receive.txt"
grep -q '^ADDRESS pcpq1_sender$' "$tmp/wallet-receive.txt"
grep -q '^REQUEST prime=3 amount_micro_units=100 address=pcpq1_sender$' "$tmp/wallet-receive.txt"
grep -q '^EXAMPLE_COMMAND wallet-send .* --to pcpq1_sender --prime 3 --amount 100 --validator <host:port>$' "$tmp/wallet-receive.txt"


"$ops" wallet-send \
  --client "$tmp/bin/primechain-client" \
  --send "$tmp/bin/primechain-send" \
  --workdir "$tmp/workdir" \
  --wallet "$tmp/sender.wallet" \
  --to pcpq1_receiver \
  --prime 3 \
  --amount 100 \
  --fee 1 \
  --use-env-passphrase \
  --validator 192.0.2.10:8339 > "$tmp/wallet-send.txt"

grep -q '^PRIMECHAIN_WALLET_SEND generated_at=' "$tmp/wallet-send.txt"
grep -q '^FROM address=pcpq1_sender wallet=' "$tmp/wallet-send.txt"
grep -q '^TO address=pcpq1_receiver$' "$tmp/wallet-send.txt"
grep -q '^TRANSFER tx_hash=tx-smoke-123 prime=3 amount_micro_units=100 fee_micro_units=1 nonce=1$' "$tmp/wallet-send.txt"
grep -q '^BALANCE_CHECK prime=3 available=1000000 required=101 pass=1$' "$tmp/wallet-send.txt"
grep -q '^PASSPHRASE_SOURCE environment$' "$tmp/wallet-send.txt"
grep -q '^SUBMIT_RESULT accepted=1$' "$tmp/wallet-send.txt"
grep -q '^NEXT_MINE_TARGET 124$' "$tmp/wallet-send.txt"
grep -q '^NEXT_COMMAND add-mine-job .* add-mine-job .* --target 124$' "$tmp/wallet-send.txt"


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
