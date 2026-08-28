# Mainnet-Candidate Validator Onboarding

This runbook is for validator owners joining or operating a mainnet-candidate Primechain network. It assumes the candidate chain has a frozen release commit, network name, validator set, bootstrap endpoints, and economic parameters.

For the detailed launch-testnet rehearsal history, see [`launch-validator-runbook.md`](launch-validator-runbook.md). For validator admission and economic rules, see [`validator-economics.md`](validator-economics.md).

## Operator Inputs

Before a validator owner starts, collect:

```text
release commit or tag
network name
validator wallet path
validator public address
public host or DNS name
public TCP port, default 8339
bootstrap endpoint, usually validator 1
canonical genesis validator address
chain data directory
wallet passphrase delivery method
```

For the current tested servers, the launch-testnet examples are:

```text
node 1: 192.81.209.230:8339  pcpq1_bea711ba725254459f479f9b48f1b292b7688304
node 2: 137.184.129.231:8339  pcpq1_1ac303aedac0d0424273765ec07de487d08c0859
node 3: 67.205.172.245:8339   pcpq1_d7ff659cfe6b511c8eb6985b6438b08e6c57402d
```

Do not treat these as final mainnet-candidate parameters until the genesis freeze document is published.

## Server Requirements

Minimum operational requirements:

- Linux server with persistent disk,
- public TCP reachability on the validator port,
- systemd available for long-running service management,
- `git`, `cmake`, and a C++17 compiler installed,
- secure storage for the validator wallet and passphrase,
- regular backups of wallet files and chain data.

Firewall/security group must allow inbound TCP to the validator port, normally `8339`.

Public validator security baseline: run only the normal validator service profile unless debugging. Do not enable `--enable-factorization-helper` on an internet-facing validator as a default setting. Release commit `b6c453e2cfbb` hardens helper factorization by bounding it to the replayed frontier, rejects off-frontier prime submissions before expensive Pratt proof verification, and counts composite-lottery signing as a write command for connection limiting. The launch-testnet-1 validators currently keep `GET_FACTORIZATION` disabled.

## Build Or Update The Validator

Run as the `primechain` user unless the command explicitly uses `sudo`.

```bash
cd ~/primechain
git fetch origin
git checkout main
git pull --ff-only
cmake -S . -B build
cmake --build build --target primechain-sync-server -- -j2
cmake --build build --target primechain-client -- -j2
cmake --build build --target primechain-composite-commitment -- -j2
./build/primechain-client version
```

The reported `git_commit` must match the release commit or tag announced for the candidate network.

## Wallet And Address

Create a validator wallet only if one does not already exist:

```bash
cd ~/primechain
./build/primechain-client new-miner /home/primechain/validator.wallet
./build/primechain-client address /home/primechain/validator.wallet
```

For an existing validator, verify the address:

```bash
./build/primechain-client address /home/primechain/validator.wallet
```

Back up the wallet file before launch. Losing the validator wallet means losing the ability to sign validator votes and endpoint updates.

## Install Or Update The Systemd Service

Use `primechain-ops install-validator-service`; do not hand-edit the `ExecStart` line unless debugging.

Validator 1, with no bootstrap peer:

```bash
cd ~/primechain
sudo ./scripts/primechain-ops install-validator-service \
  --identity /home/primechain/validator.wallet \
  --genesis-validator <genesis-validator-address> \
  --port 8339 \
  --data /home/primechain/primechain-launch-data/chain.dat
```

Validator 2 or later, with bootstrap peer:

```bash
cd ~/primechain
sudo ./scripts/primechain-ops install-validator-service \
  --identity /home/primechain/validator.wallet \
  --genesis-validator <genesis-validator-address> \
  --bootstrap <bootstrap-host>:8339 \
  --port 8339 \
  --data /home/primechain/primechain-launch-data/chain.dat
```

The generated service enables on-chain endpoint peer discovery by default through `--use-chain-endpoints`.

Preview the service without writing it:

```bash
./scripts/primechain-ops print-validator-service \
  --identity /home/primechain/validator.wallet \
  --genesis-validator <genesis-validator-address> \
  --bootstrap <bootstrap-host>:8339
```

## Start From Clean Chain Data

For a new candidate genesis or dry run, archive old data before starting. Do not mix old launch-testnet data with a new candidate chain.

```bash
sudo systemctl stop primechain-launch-node || true

ts=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p /home/primechain/old-launch-chains

if [ -d /home/primechain/primechain-launch-data ]; then
  cp -a /home/primechain/primechain-launch-data \
    /home/primechain/old-launch-chains/primechain-launch-data-$ts
fi

rm -rf /home/primechain/primechain-launch-data
mkdir -p /home/primechain/primechain-launch-data
```

Then install/start the service with the frozen genesis parameters.

## Health Checks

Local validator check:

```bash
cd ~/primechain
./scripts/primechain-ops doctor-validator \
  --host 127.0.0.1 \
  --port 8339 \
  --identity /home/primechain/validator.wallet
```

Expected result has all `OK` lines, including:

```text
OK service-active
OK port-listening 8339
OK node-status
OK validators-query
OK validator-epoch-query
OK validator-endpoints-query
OK peers-query
```

Network check from an operator machine:

```bash
cd ~/primechain
./scripts/primechain-ops version-network \
  --validator 192.81.209.230:8339 \
  --validator 137.184.129.231:8339 \
  --validator 67.205.172.245:8339

./scripts/primechain-ops doctor-network \
  --validator 192.81.209.230:8339 \
  --validator 137.184.129.231:8339 \
  --validator 67.205.172.245:8339
```

Expected result:

```text
VERSION_NETWORK_OK validators=3
NETWORK_OK validators=3 frontier=<n> hash=<hash>
```

## Publish Or Change Endpoint

The validator address is the identity. The IP address or DNS name is mutable routing metadata and should be published on-chain.

Each validator signs its own endpoint update. Run on the validator host:

```bash
cd ~/primechain
status=$(./build/primechain-client query 127.0.0.1 8339 GET_STATUS)
echo "$status"
```

Extract:

```text
record_integer = field 7 + 1
previous_hash  = field 8
```

Sign and submit the endpoint update:

```bash
endpoint=$(./build/primechain-composite-commitment sign-endpoint \
  /home/primechain/validator.wallet \
  <previous_hash> \
  <record_integer> \
  <public-host-or-dns> \
  8339 \
  <sequence>)

./build/primechain-client query 127.0.0.1 8339 $endpoint
```

Then mine or wait for the next accepted arithmetic record. Verify from the operator machine:

```bash
./scripts/primechain-ops doctor-network \
  --validator 192.81.209.230:8339 \
  --validator 137.184.129.231:8339 \
  --validator 67.205.172.245:8339
```

Use a higher endpoint `sequence` for later IP/DNS changes. Do not reuse a lower sequence.

## Recovery And Rejoin

If a validator falls behind but still starts, first let peer sync catch up:

```bash
sudo systemctl restart primechain-launch-node
sleep 3
cd ~/primechain
./build/primechain-client status 127.0.0.1 8339
```

If local chain data is corrupt or disagrees with quorum, use `chain-recover` from an operator machine with at least two healthy sources:

```bash
cd ~/primechain
./scripts/primechain-ops chain-recover \
  --workdir ~/pc-launch-testnet \
  --validator 192.81.209.230:8339 \
  --validator 137.184.129.231:8339 \
  --validator 67.205.172.245:8339
```

For a validator host, stop the service, replace its local chain with the recovered chain or sync from a healthy peer, then restart. Volatile sidecar caches such as `.commitments`, `.phases`, `.epochs`, `.finalization`, and `.rounds` are recoverable startup caches; the canonical `.dat` chain file remains strict.

## Logs

Use these commands when a service does not come up:

```bash
sudo systemctl status primechain-launch-node --no-pager -l
sudo journalctl -u primechain-launch-node -n 120 --no-pager
sudo grep '^ExecStart=' /etc/systemd/system/primechain-launch-node.service
```

Common fatal issues:

- wrong release commit or stale binary,
- wrong genesis validator anchor,
- wallet address does not belong to the active validator set,
- firewall blocks TCP port,
- local chain file belongs to a different network or candidate chain,
- missing bootstrap peer for non-genesis startup.

## Evidence After Onboarding

After validators are online, mine a short range and inspect participation:

```bash
cd ~/primechain
./build/primechain-client board-report ~/pc-launch-testnet/data/chain.dat --from <start> --to <end>
./build/primechain-client latest-records ~/pc-launch-testnet/data/chain.dat --last 20
```

Expected evidence after all validators are active:

```text
VALIDATOR_EVIDENCE_SUMMARY active=3 historical=0 bootstrap_dev=0
```

Recent composite records should normally show:

```text
finalization_votes=3 commit_phase_votes=3
```

Recent prime records should show:

```text
finalization_votes=3 commit_phase_votes=0
```

## Owner Handoff Checklist

Before handing a validator to an owner, confirm:

```text
[ ] owner has the validator wallet backup
[ ] owner knows the wallet passphrase procedure
[ ] server is on the frozen release commit
[ ] service is installed through primechain-ops
[ ] public TCP port is reachable
[ ] endpoint update is on-chain
[ ] doctor-validator passes locally
[ ] doctor-network passes from operator machine
[ ] owner knows how to restart and read logs
[ ] owner knows that reserve slashing is planned, not yet automatic
```
