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
case "$cmd" in
  status)
    echo "STATUS 122 30 92 1 121 123 abc123"
    ;;
  sync-peer)
    echo "SYNC_UP_TO_DATE 123"
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
