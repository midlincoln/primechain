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
