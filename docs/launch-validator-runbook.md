# Launch Validator Runbook

This runbook records the controlled launch flow tested on the 8339 launch chain. It is for operator testing, not a production launch promise.

The tested path is:

```text
one-validator genesis
desktop miner builds work and reserves
validator 2 applies and is admitted
validator 3 applies and is admitted
all three validators finalize records together
validator endpoints are published on-chain
```

## Roles

Use separate terminals for long-running validator servers and for commands.

```text
validator server terminal = runs primechain-sync-server and waits for requests
validator command terminal = signs applications, endpoints, votes, and queries
desktop terminal          = mines, locks reserves, sponsors validator work
```

Do not press Ctrl-C in a validator server terminal unless you intend to stop that validator. A foreground server that appears idle is normally just waiting for network requests.

## Addresses Used In The 8339 Test

```text
validator 1:
  host:    192.81.209.230
  port:    8339
  wallet:  /home/primechain/validator-1.wallet
  address: pcpq1_bea711ba725254459f479f9b48f1b292b7688304

validator 2:
  host:    137.184.129.231
  port:    8339
  wallet:  /home/primechain/validator-2.wallet
  address: pcpq1_1ac303aedac0d0424273765ec07de487d08c0859

validator 3:
  host:    67.205.172.245
  port:    8339
  wallet:  /home/primechain/validator-3.wallet
  address: pcpq1_d7ff659cfe6b511c8eb6985b6438b08e6c57402d

desktop miner:
  workdir: /home/ovanes1/pc-launch-testnet
  wallet:  /home/ovanes1/pc-launch-testnet/wallets/prime.wallet
  address: pcpq1_cbe7f859271cc4ae085a26ca9a3bb06710a14884
```

## Start Validator 1 Genesis

On validator 1, start the launch chain with only validator 1 in the genesis validator set:

```bash
cd ~/primechain
set -a
source /home/primechain/primechain-wallet.env
set +a

rm -rf /home/primechain/primechain-launch-data
mkdir -p /home/primechain/primechain-launch-data

v1=pcpq1_bea711ba725254459f479f9b48f1b292b7688304

~/primechain/build/primechain-sync-server \
  8339 \
  /home/primechain/primechain-launch-data/chain.dat \
  --bind 0.0.0.0 \
  --genesis-validator-set "$v1" \
  --validator-identity /home/primechain/validator-1.wallet \
  --use-chain-endpoints \
  --finalization-timeout-ms 5000
```

From the desktop, check that the node is reachable:

```bash
cd ~/primechain

~/primechain/build/primechain-client status 192.81.209.230 8339
~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATORS
~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATOR_EPOCH
```

If local queries work on the server but remote desktop queries hang, check the firewall and TCP reachability:

```bash
nc -vz 192.81.209.230 8339
```

## Initialize Desktop Miner

On the desktop:

```bash
cd ~/primechain
export PRIMECHAIN_WALLET_PASSPHRASE='<test passphrase>'

rm -rf ~/pc-launch-testnet
~/primechain/build/primechain-client init-workdir ~/pc-launch-testnet 192.81.209.230 8339
~/primechain/build/primechain-client sync-peer ~/pc-launch-testnet
```

Mine enough early records to create reserve funds and mining history:

```bash
~/primechain/build/primechain-client add-mine-job ~/pc-launch-testnet --target 31
~/primechain/build/primechain-client run-jobs ~/pc-launch-testnet
~/primechain/build/primechain-client sync-peer ~/pc-launch-testnet

~/primechain/build/primechain-client balance \
  ~/pc-launch-testnet/data/chain.dat \
  ~/pc-launch-testnet/wallets/prime.wallet
```

## Lock Reserve For A Candidate

Validator reserves are sent from a spendable miner wallet to a deterministic reserve address:

```text
pcreserve_validator_<validator-address>
```

The current test minimum is:

```text
5,000,000 micro-units
```

Reserve locks spend normal wallet balances and pay normal transfer fees. The lock amount must leave enough balance for the fee.

Example command shape:

```bash
candidate=<validator-address>

~/primechain/build/primechain-send reserve-lock \
  192.81.209.230 8339 \
  ~/pc-launch-testnet/wallets/prime.wallet \
  "$candidate" \
  <prime-asset> \
  <lock-micro-units> \
  <fee-micro-units> \
  <nonce>
```

Mine the next record to include pending reserve-lock transactions, then check:

```bash
~/primechain/build/primechain-client add-mine-job ~/pc-launch-testnet --target <next-integer>
~/primechain/build/primechain-client run-jobs ~/pc-launch-testnet
~/primechain/build/primechain-client sync-peer ~/pc-launch-testnet

~/primechain/build/primechain-client validator-reserve \
  ~/pc-launch-testnet/data/chain.dat \
  "$candidate"
```

## Sponsor Candidate Work

In the launch test, the desktop miner owned the mining history and sponsored validator 2 and validator 3. That keeps validator servers from mining while still creating replayable evidence that the operator has earned work history.

On the desktop:

```bash
candidate=<validator-address>

status=$(~/primechain/build/primechain-client status 192.81.209.230 8339)
previous=$(echo "$status" | awk '{print $8}')
next=$(echo "$status" | awk '{print $7 + 1}')

binding=$(~/primechain/build/primechain-composite-commitment sign-work-binding \
  ~/pc-launch-testnet/wallets/prime.wallet \
  "$previous" \
  "$next" \
  "$candidate" \
  1)

~/primechain/build/primechain-client query 192.81.209.230 8339 $binding
~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATOR_WORK_BINDINGS
```

## Submit Validator Application

Run this on the candidate validator server command terminal:

```bash
cd ~/primechain
set -a
source /home/primechain/primechain-wallet.env
set +a

status=$(~/primechain/build/primechain-client status 192.81.209.230 8339)
previous=$(echo "$status" | awk '{print $8}')
next=$(echo "$status" | awk '{print $7 + 1}')

application=$(~/primechain/build/primechain-composite-commitment sign-application \
  /home/primechain/<candidate-wallet>.wallet \
  "$previous" \
  "$next" \
  <candidate-host> \
  8339 \
  1 \
  100 \
  100)

~/primechain/build/primechain-client query 192.81.209.230 8339 $application
~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATOR_APPLICATIONS
```

The `100 100` values are the observed endpoint-success count and total observation count used for this controlled test.

## Vote Admission Epoch

The active validators sign an epoch transition. With one active validator, validator 1 alone can admit validator 2. With two active validators, both validators 1 and 2 should sign to admit validator 3.

Command shape:

```bash
status=$(~/primechain/build/primechain-client status 127.0.0.1 8339)
previous=$(echo "$status" | awk '{print $8}')
next=$(echo "$status" | awk '{print $7 + 1}')

vote=$(~/primechain/build/primechain-composite-commitment sign-epoch \
  /home/primechain/<active-validator-wallet>.wallet \
  "$previous" \
  "$next" \
  <next-epoch-number> \
  <sorted-next-validator-addresses...>)

~/primechain/build/primechain-client query 127.0.0.1 8339 $vote
~/primechain/build/primechain-client query 127.0.0.1 8339 GET_EPOCH_VOTES
```

Mine the voted integer from the desktop to put the epoch transition on-chain:

```bash
~/primechain/build/primechain-client add-mine-job ~/pc-launch-testnet --target <voted-integer>
~/primechain/build/primechain-client run-jobs ~/pc-launch-testnet
~/primechain/build/primechain-client sync-peer ~/pc-launch-testnet

~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATORS
~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATOR_EPOCH
```

## Start Newly Admitted Validators

A newly admitted validator syncs from a bootstrap peer, but its command line gives only the genesis validator anchor. Replay derives the active validator set from on-chain epoch transitions. With `--use-chain-endpoints`, the node also learns active validator peer addresses from on-chain endpoint records.

Use this pattern on validator 2 or validator 3:

```bash
cd ~/primechain
set -a
source /home/primechain/primechain-wallet.env
set +a

rm -rf /home/primechain/primechain-launch-data
mkdir -p /home/primechain/primechain-launch-data

v1=pcpq1_bea711ba725254459f479f9b48f1b292b7688304

~/primechain/build/primechain-sync-server \
  8339 \
  /home/primechain/primechain-launch-data/chain.dat \
  --bind 0.0.0.0 \
  --peer 192.81.209.230 8339 \
  --validator-set "$v1" \
  --validator-identity /home/primechain/<validator-wallet>.wallet \
  --finalization-timeout-ms 5000
```

If the server prints `validator quorum enabled: 1-of-1` after syncing a later chain, verify with direct queries. The replay-derived state is authoritative:

```bash
~/primechain/build/primechain-client status 127.0.0.1 8339
~/primechain/build/primechain-client query 127.0.0.1 8339 GET_VALIDATORS
~/primechain/build/primechain-client query 127.0.0.1 8339 GET_VALIDATOR_EPOCH
```

A wrong genesis anchor usually fails with:

```text
peer genesis validator set differs from configured validator set
```

## Future Validator Startup

A future candidate can run before it is admitted. If its identity is not yet in the active validator set, the server starts as an inactive validator identity: it syncs, serves queries, gossips evidence, and waits. It does not sign finalization or quorum votes until replay shows an epoch where its address is active.

Generic candidate startup shape:

```bash
cd ~/primechain
set -a
source /home/primechain/primechain-wallet.env
set +a

v1=pcpq1_bea711ba725254459f479f9b48f1b292b7688304

~/primechain/build/primechain-sync-server \
  8339 \
  /home/primechain/primechain-launch-data/chain.dat \
  --bind 0.0.0.0 \
  --bootstrap-peer 192.81.209.230 8339 \
  --genesis-validator-set "$v1" \
  --validator-identity /home/primechain/<future-validator-wallet>.wallet \
  --use-chain-endpoints \
  --finalization-timeout-ms 5000
```

This is the target operational model: a candidate starts once, applies, waits through admission, and becomes active automatically after the on-chain epoch transition activates it.

## Connect Peers

After all validators are running, add peers in both directions:

```bash
~/primechain/build/primechain-client query 192.81.209.230 8339 ADD_PEER 137.184.129.231 8339
~/primechain/build/primechain-client query 192.81.209.230 8339 ADD_PEER 67.205.172.245 8339

~/primechain/build/primechain-client query 137.184.129.231 8339 ADD_PEER 192.81.209.230 8339
~/primechain/build/primechain-client query 137.184.129.231 8339 ADD_PEER 67.205.172.245 8339

~/primechain/build/primechain-client query 67.205.172.245 8339 ADD_PEER 192.81.209.230 8339
~/primechain/build/primechain-client query 67.205.172.245 8339 ADD_PEER 137.184.129.231 8339
```

Check:

```bash
~/primechain/build/primechain-client query 192.81.209.230 8339 GET_PEERS
~/primechain/build/primechain-client query 137.184.129.231 8339 GET_PEERS
~/primechain/build/primechain-client query 67.205.172.245 8339 GET_PEERS
```

## Publish Validator Endpoints On-Chain

Each validator signs its own endpoint update. Run the corresponding command on each validator command terminal.

Validator 1:

```bash
status=$(~/primechain/build/primechain-client status 127.0.0.1 8339)
previous=$(echo "$status" | awk '{print $8}')
next=$(echo "$status" | awk '{print $7 + 1}')

endpoint=$(~/primechain/build/primechain-composite-commitment sign-endpoint \
  /home/primechain/validator-1.wallet \
  "$previous" \
  "$next" \
  192.81.209.230 \
  8339 \
  1)

~/primechain/build/primechain-client query 127.0.0.1 8339 $endpoint
```

Validator 2:

```bash
status=$(~/primechain/build/primechain-client status 127.0.0.1 8339)
previous=$(echo "$status" | awk '{print $8}')
next=$(echo "$status" | awk '{print $7 + 1}')

endpoint=$(~/primechain/build/primechain-composite-commitment sign-endpoint \
  /home/primechain/validator-2.wallet \
  "$previous" \
  "$next" \
  137.184.129.231 \
  8339 \
  1)

~/primechain/build/primechain-client query 127.0.0.1 8339 $endpoint
```

Validator 3:

```bash
status=$(~/primechain/build/primechain-client status 127.0.0.1 8339)
previous=$(echo "$status" | awk '{print $8}')
next=$(echo "$status" | awk '{print $7 + 1}')

endpoint=$(~/primechain/build/primechain-composite-commitment sign-endpoint \
  /home/primechain/validator-3.wallet \
  "$previous" \
  "$next" \
  67.205.172.245 \
  8339 \
  1)

~/primechain/build/primechain-client query 127.0.0.1 8339 $endpoint
```

Mine one more record from the desktop, then check the endpoint registry:

```bash
~/primechain/build/primechain-client add-mine-job ~/pc-launch-testnet --target <next-integer>
~/primechain/build/primechain-client run-jobs ~/pc-launch-testnet
~/primechain/build/primechain-client sync-peer ~/pc-launch-testnet

~/primechain/build/primechain-client query 192.81.209.230 8339 GET_VALIDATOR_ENDPOINTS
~/primechain/build/primechain-client validator-endpoints ~/pc-launch-testnet/data/chain.dat
```

## Stability Run

After three validators are active and endpoints are on-chain, mine a short range and confirm all validators agree:

```bash
~/primechain/build/primechain-client add-mine-job ~/pc-launch-testnet --target 120
~/primechain/build/primechain-client run-jobs ~/pc-launch-testnet
~/primechain/build/primechain-client sync-peer ~/pc-launch-testnet

~/primechain/build/primechain-client status 192.81.209.230 8339
~/primechain/build/primechain-client status 137.184.129.231 8339
~/primechain/build/primechain-client status 67.205.172.245 8339
```

The tested successful result at frontier 120 was:

```text
STATUS 119 30 89 1 118 120 9f0be261c7d2ee1e1d22d8ed5df34672546130c105ada9b6c66f3a2703445650
```

All three validators reported the same status and latest hash.

## Audit Commands

Use these commands to collect replayable evidence from the local desktop chain:

```bash
~/primechain/build/primechain-client inspect ~/pc-launch-testnet/data/chain.dat
~/primechain/build/primechain-client validator-registry ~/pc-launch-testnet/data/chain.dat
~/primechain/build/primechain-client validator-endpoints ~/pc-launch-testnet/data/chain.dat

~/primechain/build/primechain-client validator-reserve \
  ~/pc-launch-testnet/data/chain.dat \
  pcpq1_1ac303aedac0d0424273765ec07de487d08c0859

~/primechain/build/primechain-client validator-reserve \
  ~/pc-launch-testnet/data/chain.dat \
  pcpq1_d7ff659cfe6b511c8eb6985b6438b08e6c57402d

~/primechain/build/primechain-client validator-eligibility \
  ~/pc-launch-testnet/data/chain.dat \
  pcpq1_1ac303aedac0d0424273765ec07de487d08c0859 \
  --reserve auto \
  --observed 100 \
  --total 100

~/primechain/build/primechain-client validator-eligibility \
  ~/pc-launch-testnet/data/chain.dat \
  pcpq1_d7ff659cfe6b511c8eb6985b6438b08e6c57402d \
  --reserve auto \
  --observed 100 \
  --total 100
```

## Common Operator Errors

- A shell prompt of `>` means the previous command is unfinished, usually because a line ended with `\`. Press Ctrl-C in that command terminal and run the full command again.
- Do not paste prose such as `Then check:` into the shell.
- Desktop mining commands should run on the desktop workdir, not on validator servers.
- Validator command terminals can query and sign. Validator server terminals should be left running.
- When starting validator 2, validator 3, or a future validator from a chain that began with one validator, use the genesis validator anchor in `--genesis-validator-set`; replay derives the later active set.
- If a node listens locally but remote clients hang, test the port with `nc -vz <host> 8339` and open the firewall if needed.
