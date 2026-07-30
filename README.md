# Primechain

Primechain is a deterministic arithmetic-record blockchain prototype. The chain advances one integer at a time: each finalized record classifies the next integer as prime or composite, carries a locally verifiable mathematical proof, and may include a transaction batch.

The current implementation is a C++17 launch-testnet prototype with authenticated mining submissions, ML-DSA-65 wallet and validator identities, controlled validator quorum, on-chain validator endpoint records, transaction replay, validator fee pools, and append-only chain storage.

## Current Launch-Testnet Status

Implemented and tested in the current public repository:

- sequential arithmetic records for prime and composite integers
- SHA3-256 record hashing and ML-DSA-65 protocol signatures
- signed Pratt prime submissions with authenticated reward addresses
- signed composite commit/reveal submissions with factor evidence
- transaction submission, mempool sync, contiguous sender nonces, and fee accounting
- controlled validator quorum with replay-derived active validator epochs
- on-chain validator endpoint updates and reserve-backed validator admission workflows
- deterministic validator fee-pool and validator reward-pool distribution
- crash-recoverable append-only chain store, rebuildable indexes, and replay snapshots
- TCP peer sync, record propagation, mempool propagation, peer health, and peer state tools
- operator tooling for validator service setup, network doctors, release checks, launch summaries, wallet dashboards, transaction lookup, and address reports

The tested one-validator-to-three-validator launch procedure is documented in [`docs/launch-validator-runbook.md`](docs/launch-validator-runbook.md). Validator admission, reserve, quorum, reward, fee, and slashing-status rules are summarized in [`docs/validator-economics.md`](docs/validator-economics.md). The active public-launch and Bitcoin-readiness plan is tracked in [`docs/working-plan.md`](docs/working-plan.md), and the lower-level production backlog is tracked in [`docs/production-roadmap-v0.md`](docs/production-roadmap-v0.md).

Still not production-ready:

- permissionless validator selection and Sybil-resistance economics
- authenticated/encrypted peer transport
- production-scale archival strategy and deterministic state-root pruning
- formal external protocol specification and conformance vectors
- ECPP or APR-CL certificate formats for larger prime certificates
- packaged releases and reproducible binary builds

## Quick Start

```bash
git clone https://github.com/midlincoln/primechain.git
cd primechain
git submodule update --init
cmake -S . -B build
cmake --build build -- -j2
./build/primechain-client version
```

## Run Tests

```bash
cmake --build build -- -j2
cd build
ctest --output-on-failure
```

## Operator Health Check

After configuring a launch workdir and validators, the compact readiness check is:

```bash
./scripts/primechain-ops release-check \
  --workdir ~/pc-launch-testnet \
  --validator 192.81.209.230:8339 \
  --validator 137.184.129.231:8339 \
  --validator 67.205.172.245:8339
```

It runs build-version agreement, network agreement, mempool agreement, local chain storage checks, fee-distribution status, and a launch summary. A healthy run ends with `RELEASE_CHECK_OK`.

## Primechain Client

`primechain-client` is the operator-facing entry point for the common node,
wallet, sync, inspection, and mining workflows. It wraps the lower-level tools
that remain available for tests and protocol development.

```bash
./build/primechain-client init-workdir ./pc-work 127.0.0.1 18889
./build/primechain-client sync-peer ./pc-work
./build/primechain-client add-mine-job ./pc-work --target 100
./build/primechain-client job-status ./pc-work
./build/primechain-client run-jobs ./pc-work
./build/primechain-client job-status ./pc-work
./build/primechain-client balances ./pc-work
./build/primechain-client rewards ./pc-work
./build/primechain-client reward-history ./pc-work --last 20
./build/primechain-client board-report ./pc-work/data/chain.dat --from 2 --to 100
./build/primechain-client validator-reputation ./pc-work/data/chain.dat <address>
./build/primechain-client update-indexes ./pc-work
./build/primechain-client index-status ./pc-work
./build/primechain-client factor-workdir ./pc-work 84
./build/primechain-client pratt-workdir ./pc-work 97
./build/primechain-client inspect ./pc-work/data/chain.dat
./build/primechain-client decode-record ./pc-work/data/chain.dat 29
./build/primechain-client is-prime 97
./build/primechain-client divisor 91
./build/primechain-client factor ./pc-work/data/chain.dat 84
./build/primechain-client pratt ./pc-work/data/chain.dat 97
```

A client workdir stores peer configuration, a local downloaded chain copy,
prime and composite miner wallets, and persistent mining job state. Protocol
wallets created by `new-miner` are encrypted ML-DSA-65 wallet files. Interactive
commands prompt for a passphrase when signing is required; unattended miners,
validators, and tests should provide it through `PRIMECHAIN_WALLET_PASSPHRASE`.
The public address can be read from wallet metadata without unlocking the
private key.

The current validator registry can be inspected from a local record store:

```bash
./build/primechain-client validator-registry ./data/chain.dat
```

This reports the replay-derived genesis/epoch validator history. Validator
admission, reserve locks, endpoint updates, transfer fees, and the active
validator minimum reserve are replay-derived chain state.

Economic policy can be inspected from a local store:

```bash
./build/primechain-client economic-policy ./data/chain.dat
```

A running validator also reports the current policy proposal target:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_ECONOMIC_POLICY
# ECONOMIC_POLICY transfer_fee_micro_units=<fee> validator_min_reserve_micro_units=<reserve> next_integer=<n> previous_hash=<hash>
```

To change the transfer fee or validator minimum reserve, at least two active
validators sign the same policy vote. The next accepted arithmetic record embeds
the quorum certificate and activates the policy at the following integer:

```bash
vote=$(./build/primechain-composite-commitment sign-policy   ./wallets/validator-a.wallet <previous_hash> <record_integer>   <transfer_fee_micro_units> <validator_min_reserve_micro_units> <sequence>)
./build/primechain-sync-query 127.0.0.1 18889 $vote
```

The tested one-validator-to-three-validator launch procedure is documented in
[`docs/launch-validator-runbook.md`](docs/launch-validator-runbook.md). Validator economics and admission rules are summarized in
[`docs/validator-economics.md`](docs/validator-economics.md). For service setup, use
`./scripts/primechain-ops install-validator-service` instead of hand-editing
systemd unit files. Use `./scripts/primechain-ops doctor-network` from a desktop
or operator host to verify all validators agree on frontier, hash, peers, and endpoints.

## Validator Incentives And Threat Model

Validators lock reserves before admission. The reserve creates economic
commitment, and validator fee distributions create continuing upside for honest
participation. Current launch-testnet behavior is deliberately narrower than a
full proof-of-stake security claim:

- validator admission is controlled by active-validator governance,
- validator reserve locks and the active minimum reserve are replay-derived
  chain state,
- validator actions are ML-DSA-signed and replay-verifiable,
- validator fee-pool and validator reward-pool distributions are deterministic and reward eligible
  participating validators,
- downtime and missed signatures are reputation/removal evidence, not automatic
  reserve forfeiture.

Planned production work includes explicit double-sign evidence, validator
removal/disable records, unbonding delays, and reserve forfeiture for
cryptographically provable misbehavior. Until those rules are implemented and
tested, Primechain should be described as a controlled reserve-backed validator
network, not permissionless or fully Byzantine-secure mainnet consensus.

`add-mine-job` records the target frontier, `run-jobs` syncs before mining,
runs the authenticated frontier miner only when the target is still ahead, then
syncs again and records pending/complete/failed state. `mine-job` remains a
convenience wrapper for add-and-run. `balances` reports current wallet holdings
from replay, `rewards` summarizes totals, and `reward-history` lists per-record
prime, composite, and fee reward events from the local chain. `update-indexes` builds
a rebuildable local composite-proof cache under `indexes/`, and
`factor-workdir` / `pratt-workdir` use that cache instead of rescanning the
chain. `board-report` prints replay-derived meeting-range metrics for records,
rewards, miner distribution, and validator evidence. `validator-reputation`
prints replay-derived mining history and validator participation for one address.
`decode-record` prints a human-readable view of a local prime or composite chain record. The lower-level direct commands remain available for tests and debugging:

```bash
./build/primechain-client status 127.0.0.1 18889
./build/primechain-client new-miner ./wallets/prime.wallet
./build/primechain-client new-miner ./wallets/composite.wallet
./build/primechain-client mine 127.0.0.1 18889 20 \
  --prime-identity ./wallets/prime.wallet \
  --composite-identity ./wallets/composite.wallet
./build/primechain-client sync 127.0.0.1 18889 2 20 ./data/downloaded.dat
```

The client wraps lower-level tools for network and mining operations, and
contains a local math workbench for primality checks, divisor search, cached
factorization from downloaded composite records, and Pratt proof construction.

## Record Store Durability

The canonical `<record-store>.dat` file remains byte-compatible with earlier
prototype stores. Successful appends are synchronized before returning. If a
process or machine stops during an append, the next open verifies the complete
prefix and truncates only an incomplete trailing record; hash corruption inside
a completed record remains a hard error.

`<record-store>.dat.idx` stores canonical integer-to-byte-offset entries for
latest, single-record, and range lookup. It is an acceleration cache rather
than consensus state: missing, stale, malformed, or inconsistent indexes are
rebuilt from the hash-verified chain. Tip replacement and validated peer-sync
installation write and synchronize a temporary chain before atomically renaming
it over the live store. Builds use 64-bit file offsets on 32-bit Linux.

`<record-store>.dat.snapshot` is an atomic, SHA3-checksummed replay cache. It
stores balances, supply, nonces, pending composite contributors, and the active
validator epoch at an exact record hash. A matching snapshot lets startup replay
only later records. Missing, stale, malformed, or checksum-invalid snapshots are
discarded and rebuilt by full replay. Snapshot write failure never changes chain
acceptance because the canonical `.dat` file remains authoritative.

Pruning is deliberately disabled. The current record `state_root` field is
reserved but not yet consensus-enforced, and miners need historical arithmetic
proofs. Nodes must retain the complete `.dat` chain. Deleting old records becomes
safe only after deterministic state roots, independently verifiable checkpoints,
and a separate proof-history retention policy are implemented.

Temporary consensus coordination files (`.commitments`, `.phases`, `.epochs`,
`.finalization`, and `.rounds`) use the same durable replacement rule. Their
temporary file is synchronized before rename and the parent directory is
synchronized afterward. On restart, an existing primary always wins and any
stale temp is removed. If the primary is absent, a fully parseable temp is
promoted; an incomplete temp is discarded. Corrupt primary files remain hard
startup errors and are never hidden by temp recovery.

## Run Demo Node

```bash
./build/primechain-node
```

Expected behavior: the node starts from frontier prime `2`, listens on `127.0.0.1:18888`, and stores accepted blocks in `data/chain.log`.

To choose a data directory:

```bash
./build/primechain-node 18888 ./test-data
```

On restart, the node replays `chain.log` from the data directory and restores the latest frontier prime.

## Run A Miner

Open a second terminal while the node is running:

```bash
./build/primechain-miner 127.0.0.1 18888 10 terminal-miner-a
```

Arguments:

- `127.0.0.1`: node host
- `18888`: node port
- `10`: number of blocks to mine
- `terminal-miner-a`: test miner address

Expected behavior: the miner asks the node for the current tip, constructs the next-prime block, submits composite proofs, and prints `ACCEPTED` responses as the node advances.

Example accepted sequence from genesis:

```text
2 -> 3 -> 5 -> 7 -> 11 -> 13 -> 17 -> 19 -> 23 -> 29 -> 31
```

## Run Cooperative Mining

The cooperative test flow is:

```text
composite miners submit composite proofs first
block miner closes blocks using pooled proofs
node logs proof contributors per accepted block
```

Terminal 1:

```bash
./build/primechain-node 18888 ./coop-data
```

Terminal 2:

```bash
./build/primechain-composite-miner 127.0.0.1 18888 composite-a 1 2 0
```

Terminal 3:

```bash
./build/primechain-composite-miner 127.0.0.1 18888 composite-b 1 2 1
```

Terminal 4:

```bash
./build/primechain-miner 127.0.0.1 18888 10 block-miner
```

The final two numeric arguments to `primechain-composite-miner` are optional sharding controls:

- `2`: number of shards
- `0` or `1`: shard handled by that miner

This lets multiple composite miners split a work window for testing.

## Emerging Sequential Arithmetic Chain

The current implementation still groups composite proofs into prime-to-prime blocks. The proposed next architecture treats every classified integer as a permanent sequential blockchain record:

```text
7  PRIME
8  COMPOSITE: 2 * 4
9  COMPOSITE: 3 * 3
10 COMPOSITE: 2 * 5
11 PRIME: Pratt certificate
```

Under this model:

- a composite proof acts like a mini-block that advances the integer frontier,
- a Pratt certificate records a prime checkpoint,
- stored composite records form reusable factor-decomposition trees,
- the derived factorization of `g - 1` helps miners construct a Pratt certificate for frontier integer `g`,
- miners may use any search algorithm, while nodes independently verify submitted proofs,
- a controlled multi-node testnet can initially use 2/3 validator voting to finalize competing valid submissions.

This architecture is still under design. Important unresolved questions include validator membership, deterministic candidate selection, Sybil resistance, spam limits, reward attribution, and multi-node synchronization.

See [docs/development-log.md](docs/development-log.md) for the current design discussion and decisions.

The first development format specification is [docs/protocol-formats-v0.md](docs/protocol-formats-v0.md). It defines draft object formats for arithmetic records, composite proofs, Pratt prime proofs, transaction batches, addresses, and controlled 2-of-3 validator finalization.

## Consensus Data Vs Client Tools

The blockchain consensus layer should stay minimal:

- one record per classified integer,
- one immediate composite proof `g = d * e` for composite records,
- one locally verifiable prime certificate for prime records,
- transaction/event commitments attached to arithmetic records,
- deterministic replay and balance updates.

Full recursive factorization is not consensus state. It is derived client data.
A miner or researcher may download the full arithmetic record database, build a
local proof index, and ask local tools for:

- factors of a covered integer `m`,
- full factorization of `m - 1`,
- Pratt proof construction data for candidate prime `m`,
- cached factorization results for repeated local mining work.

This keeps server nodes focused on serving records and validating submitted
proofs. Expensive factorization work belongs to miners/clients unless a node
explicitly opts into helper service for development.

## Transaction Batches In Arithmetic Records

The current design direction is that every finalized arithmetic record can also carry a transaction batch:

```text
CompositeRecord(g = d * e) + transaction_merkle_root
PrimeRecord(g is prime)    + transaction_merkle_root
```

This separates arithmetic progress from transaction throughput. One proof does not need to mean one transaction. A single composite or prime record may commit to many transactions through a Merkle root.

This matters for the proposed Bitcoin mirror test:

```text
one Bitcoin block
    -> one Primechain prime-to-prime interval

Bitcoin block transactions
    -> deterministically batched across that interval's arithmetic records

final prime record
    -> commits to the Bitcoin block hash
```

The mirror test is intended as a storage, replay, and synchronization stress test. It is not a replacement for Primechain's own consensus.

## Estimate Long-Run Chain Scale

The estimator approximates how large the frontier prime becomes after a given number of years and block rate:

```bash
./build/primechain-estimator 10 1
```

This means 10 years at 1 block per second.

More examples:

```bash
./build/primechain-estimator 10 0.0166666667
./build/primechain-estimator 10 1000000
```

## Run Arithmetic Record Benchmark

This benchmark writes one sequential classification record per integer and gives each record a synthetic transaction batch. It does not use TCP yet.

```bash
./build/primechain-arithmetic-bench 100000 100 3 ./bench-data/arithmetic.log
```

Arguments:

- `100000`: number of arithmetic records to write
- `100`: synthetic transactions per record
- `3`: first integer to classify
- `./bench-data/arithmetic.log`: output log path

It reports records per second, synthetic transactions per second, prime/composite counts, log size, and bytes per record.

## Run Sequential Chain To 500

This local tool writes one arithmetic record for every integer from `2` through the requested limit. Composites get immediate factor proofs. Primes get small-number Pratt proofs using factorization of `p - 1` derived from earlier composite records.

```bash
./build/primechain-sequential 500 ./data/sequential-500.log ./data/sequential-500.dat
```

You can direct generated mining rewards to specific development addresses:

```bash
./build/primechain-sequential 500 ./data/wallet-chain.log ./data/wallet-chain.dat \
  --prime-miner pcdev1_373830813f57da0581a814963cf165b3 \
  --composite-miner pcdev1_composite_miner
```

You can inject one development transfer into a chosen arithmetic record:

```bash
./build/primechain-sequential 20 ./data/tx-chain.log ./data/tx-chain.dat \
  --prime-miner pcdev1_373830813f57da0581a814963cf165b3 \
  --composite-miner pcdev1_composite_miner \
  --transfer ./wallets/miner.wallet pcdev1_1654a887b941f792dd86094f19e90479 3 250000 4
```

This sends `250000` micro-units of prime `3` from `miner.wallet` to Alice's address and embeds the transfer in record `4`. The sender must already own the units before the target record is applied.

Shortcut using `primechain-send`:

```bash
./build/primechain-send 20 ./data/send-chain.log ./data/send-chain.dat \
  ./wallets/miner.wallet pcdev1_1654a887b941f792dd86094f19e90479 3 250000 4
```

`primechain-send` uses the sender wallet address as the prime miner address for this fresh generated test chain, so the sender owns prime `3` before record `4` spends it.

Submit an authenticated ML-DSA-65 transaction to a running TCP node:

```bash
./build/primechain-wallet new-miner ./wallets/sender.wallet
./build/primechain-wallet new-miner ./wallets/alice.wallet
alice=$(./build/primechain-wallet address ./wallets/alice.wallet)
./build/primechain-sync-server 18889 ./data/send-chain.dat --enable-ack-mempool
sender=$(./build/primechain-wallet address ./wallets/sender.wallet)
./build/primechain-sync-query 127.0.0.1 18889 GET_NONCE "$sender"
./build/primechain-send submit 127.0.0.1 18889 \
  ./wallets/sender.wallet "$alice" 3 250000 1000 1
```

The optional value before the nonce is the fee in integer micro-units of the
transferred prime; omitting it creates a zero-fee transaction. `GET_NONCE`
returns `NONCE <address> <confirmed> <next>`, where `next` includes contiguous
transactions already in the local mempool.

The TCP node verifies the ML-DSA-65 signature, key-derived sender address,
confirmed balance, fee conservation, and contiguous sender nonce before storing
the transaction in its in-memory mempool. The input amount must equal outputs
plus the fee for each prime asset. When an arithmetic record finalizes the
batch, transaction fees are credited to the deterministic validator fee-pool
address for the active validator epoch, for example
`pcpool_validator_fees_epoch_0`. Peers forward accepted transactions with the
same `SUBMIT_TX` command. Duplicate hashes and conflicting sender nonces are
rejected, and stale mempool entries are pruned after record acceptance or
synchronization.

Validator fee-pool balances can be inspected and distributed by a protocol
transaction. The distribution transaction has no wallet signature; replay
accepts it only when it spends the full selected fee-pool asset balance and pays
eligible validators by sorted address order. Validators with a zero share are
omitted from the transaction outputs.

Validator-set chains reserve 10% of each non-genesis prime asset for the active
epoch's deterministic validator reward pool. The remaining 90% stays with
mathematical discovery: 45% to the prime prover and 45% to composite providers
since the previous prime. If there were no composite providers, the prime prover
receives the full 90% discovery allocation. The validator reward pool is spent
manually with the same deterministic full-pool distribution rule. Existing
launch-testnet chains that used the older reward rule should be treated as
disposable evidence chains, not upgraded in place for mainnet economics.

```bash
./build/primechain-client fee-pool ./data/send-chain.dat
./build/primechain-client validator-reward-pool ./data/send-chain.dat
./build/primechain-client validator-reward-distribution-status ./data/send-chain.dat 1000
./build/primechain-send distribute-fee-pool 127.0.0.1 18889 \
  0 401 2 1 \
  pcpq1_validator_a pcpq1_validator_b pcpq1_validator_c
./build/primechain-send distribute-validator-reward-pool 127.0.0.1 18889 \
  0 401 100000 1 \
  pcpq1_validator_a pcpq1_validator_b pcpq1_validator_c
```

Create a cryptographic miner identity for signed composite mining:

```bash
./build/primechain-wallet new-miner ./wallets/composite-miner.wallet
./build/primechain-wallet miner-address ./wallets/composite-miner.wallet
```

This creates an ML-DSA-65 keypair and a key-derived `pcpq1_...` address. The private
key remains in the local identity file. Generate and submit signed messages:

```bash
commit=$(./build/primechain-composite-commitment sign-commit \
  ./wallets/composite-miner.wallet 4 2 2 44)
reveal=$(./build/primechain-composite-commitment sign-reveal \
  ./wallets/composite-miner.wallet 4 2 2 44)

./build/primechain-sync-query 127.0.0.1 18889 $commit
./build/primechain-sync-query 127.0.0.1 18889 $reveal
```

Wire formats:

```text
SUBMIT_SIGNED_COMMIT g commitment_hash provider_address public_key signature
SUBMIT_SIGNED_REVEAL g d e nonce provider_address public_key signature
```

The node derives the address from the public key and verifies both ML-DSA-65
signatures before accepting the commitment or reveal. The public key, nonce,
and reveal signature are retained in the finalized composite record, allowing
independent verification during chain replay and synchronization. Signed commitments retain their authentication evidence in the persistent commitment
sidecar while the phase is open. Once a quorum-authorized reveal is finalized,
composite record version 1 embeds the complete canonical commitment snapshot,
validator set, and signed 2-of-3 phase certificate.

The frontier miner can use the identity directly:

```bash
./build/primechain-frontier-miner 127.0.0.1 18889 100 \
  --prime-identity ./wallets/composite-miner.wallet \
  --composite-identity ./wallets/composite-miner.wallet
```

Authenticated protocol identities use NIST ML-DSA-65. Commitments, snapshots,
records, addresses, and transaction roots use SHA3-256. Existing Ed25519-era
wallets and chain stores are intentionally incompatible and must be regenerated.

### Controlled 2-of-3 Commit Phase

Quorum mode is opt-in and intended for a controlled testnet. Create three
ML-DSA-65 identities and configure every validator with the same set. The node
sorts the addresses canonically and commits them into version-1 genesis:

```bash
a=$(./build/primechain-wallet new-miner ./wallets/validator-a.wallet)
b=$(./build/primechain-wallet new-miner ./wallets/validator-b.wallet)
c=$(./build/primechain-wallet new-miner ./wallets/validator-c.wallet)

./build/primechain-sync-server 18889 ./data/validator-a.dat \
  --validator-set $a $b $c \
  --validator-identity ./wallets/validator-a.wallet
```

After signed miner commitments have propagated, validators close the phase:

```bash
./build/primechain-sync-query 127.0.0.1 18889 CLOSE_COMMIT_PHASE 4
./build/primechain-sync-query 127.0.0.1 18889 GET_COMMIT_PHASE 4
./build/primechain-sync-query 127.0.0.1 18889 GET_PHASE_VOTES 4
./build/primechain-sync-query 127.0.0.1 18889 GET_VALIDATORS
```

The first valid vote changes the phase from `OPEN` to `CLOSING` and freezes the
canonical commitment snapshot. A second distinct configured validator signature
for that exact snapshot changes it to `CLOSED`. Only then may the selected
commitment reveal. Late commitments, unsigned commitments/reveals, direct
composite submissions, unconfigured validators, conflicting snapshots, and
non-winning peer records are rejected in quorum mode.

```text
CLOSE_COMMIT_PHASE g
GET_COMMIT_PHASE g
GET_PHASE_VOTES g
SUBMIT_PHASE_VOTE g snapshot_hash validator_address public_key signature
```

Votes are persisted atomically in `<record-store>.phases`, restored after
restart, synchronized from peers, and propagated between configured nodes.
When the winning reveal is accepted, the finalized composite record embeds the
canonical commitments, validator set, snapshot hash, and signed quorum votes.
Historical replay therefore verifies the phase certificate without either
`.commitments` or `.phases` sidecars.
The helper can sign a vote for controlled tests:

```bash
./build/primechain-composite-commitment \
  sign-phase ./wallets/validator-b.wallet 4 $snapshot_hash
```

The genesis record permanently authorizes the initial three-validator set.
Every version-1 composite certificate must match that set during append, peer
sync, and historical replay. A quorum node refuses legacy unanchored genesis or
a command-line validator set that differs from chain history. Use a fresh record
store when creating a quorum testnet; an existing version-0 test chain is not
automatically upgraded.

This is still not permissionless consensus. Validator selection is manual;
controlled validator rotation is described below.

Submit a composite proof using the development commit-reveal flow. First,
the miner chooses a nonce and computes the canonical commitment locally:

```bash
commitment=$(./build/primechain-composite-commitment \
  4 2 2 44 pcdev1_composite_miner)
```

The committed fields are domain-separated and canonically encoded as:

```text
H("primechain-composite-commit-v0" || g || d || e || nonce || provider_address)
```

Submit the commitment without exposing the factors:

```bash
./build/primechain-sync-query 127.0.0.1 18889 \
  SUBMIT_COMMIT 4 $commitment pcdev1_composite_miner
```

Then reveal the factors and nonce:

```bash
./build/primechain-sync-query 127.0.0.1 18889 \
  SUBMIT_COMPOSITE_REVEAL 4 2 2 44 pcdev1_composite_miner
```

Formats:

```text
SUBMIT_COMMIT g commitment_hash provider_address
GET_COMMITMENTS g
GET_COMMIT_WINNER g
SUBMIT_COMPOSITE_REVEAL g d e nonce provider_address
```

The node requires a prior commitment for the same `(g, provider_address)`,
recomputes the commitment, verifies `d * e = g`, and requires `g` to extend the
current frontier. Commitments are bounded, atomically persisted in a separate
`<record-store>.commitments` file, restored after restart, synchronized from
known peers, and pruned after their integer is finalized. Newly accepted
commitments are also propagated to known peers. For all commitments currently known for `g`, the selected candidate is
the lexicographically smallest `(commitment_hash, provider_address)` pair. This
selection is independent of message arrival order. A reveal without a
commitment, with different factors, nonce, or provider, or from a non-selected
commitment is rejected. `GET_COMMIT_WINNER g` reports the currently selected
candidate.

Current success responses:

```text
COMMIT_ACCEPTED <g> <commitment_hash>
COMPOSITE_ACCEPTED <g> <record_hash>
```

`SUBMIT_COMPOSITE g d e provider_address` remains as a legacy development path
for existing tests. It does not provide proof-theft protection and must not be
treated as the production miner entry point.

Important limitation: commitments are persistent candidate records, but they
are not finalized consensus records. This version
prevents an ordinary peer that first learns the factors at reveal time from
claiming the same reveal without a prior matching commitment. It does not establish that the selected commitment was globally earliest.
The current lowest-hash rule gives deterministic convergence once nodes know
the same candidate set, but miners can vary nonces to grind for a lower hash.
The opt-in controlled quorum mode freezes that candidate set before reveal;
the production fairness rule and permissionless validator selection remain open.

A correct peer independently validates every finalized composite record. For
example, `2 * 2 = 5` is rejected and cannot advance an honest node. Competing
valid finalized records for the same current tip still use the existing
lower-record-hash conflict rule.

Submit authenticated prime proofs with the frontier miner:

```bash
./build/primechain-wallet new-miner ./wallets/miner.wallet
./build/primechain-frontier-miner 127.0.0.1 18889 20 \
  --prime-identity ./wallets/miner.wallet \
  --composite-identity ./wallets/miner.wallet \
  --proof-store ./data/frontier-node.dat
```

Signed prime wire format:

```text
SUBMIT_SIGNED_PRIME p witness factor_count factor_1 exponent_1 ... provider_address public_key signature
```

The ML-DSA-65 signature binds the previous finalized record hash, the prime, Pratt witness, complete `p - 1` factorization, and reward address. A peer that copies a Pratt certificate cannot replace the provider address or replay the signature at another frontier. Unsigned `SUBMIT_PRIME` is rejected by TCP nodes; it remains only as an internal offline-development representation.

The current prototype accepts Pratt certificates only. Future protocol versions can add certificate-type fields for ECPP/APR-CL without changing the authentication rule. Successful submission returns `PRIME_ACCEPTED <p> <record_hash>`.

Ask a running TCP node for the recursive factorization it can reconstruct from
stored arithmetic records. This is a development/helper command, not normal
node consensus work, and is disabled by default:

```bash
./build/primechain-sync-server 18889 ./data/frontier-node.dat --enable-factorization-helper
./build/primechain-sync-query 127.0.0.1 18889 GET_FACTORIZATION 12
```

Response format:

```text
FACTORIZATION <n> FACTORS <k> PRIME <p1> EXP <e1> PRIME <p2> EXP <e2> ...
```

Examples:

```text
FACTORIZATION 12 FACTORS 2 PRIME 2 EXP 2 PRIME 3 EXP 1
FACTORIZATION 19 FACTORS 1 PRIME 19 EXP 1
```

The labels are intentional: `PRIME 2 EXP 2` means `2^2`, not two separate
factor entries. A client must still verify that factors are prime, strictly
increasing, exponents are positive, and the product equals `<n>`. If the node
does not have enough prior composite records to recursively factor `<n>`, it
returns:

```text
ERROR factorization unavailable
```

Production rule: nodes should not be required to spend CPU recursively
factorizing numbers for remote clients. Miners that need this data should
download the arithmetic record database, build their own local proof index, and
compute/cache factorizations on their own machines. The chain stores proofs;
client tools derive full factorization knowledge.

Run the prototype frontier miner loop against a TCP sync node:

```bash
./build/primechain-sync-server 18889 ./data/frontier-node.dat
./build/primechain-wallet new-miner ./wallets/miner.wallet
./build/primechain-frontier-miner 127.0.0.1 18889 20 \
  --prime-identity ./wallets/miner.wallet \
  --composite-identity ./wallets/miner.wallet \
  --proof-store ./data/frontier-node.dat
```

The frontier miner repeatedly:

- asks the node for `GET_STATUS`;
- tests `frontier + 1`;
- submits `SUBMIT_COMPOSITE` when the next integer is composite;
- submits `SUBMIT_SIGNED_PRIME` with an authenticated Pratt proof when the next integer is prime;
- stops when the node frontier reaches the requested limit.

This is the first real mining flow for the sequential arithmetic chain. The optional `--proof-store` argument bootstraps the miner's local composite-proof index from an existing downloaded chain, so a miner can continue from a previously synced frontier instead of needing to start from genesis in the same process. `primechain-client run-jobs` passes the workdir chain as the proof store automatically.

When `ADVANCE_TO` creates new arithmetic records, the node also forwards each finalized record to configured peers with `SUBMIT_RECORD`. The receiving peer replays normal record validation before appending anything locally. Exact duplicate records are ignored. Same-tip conflicts are resolved deterministically: if two records have the same integer and same previous record hash, the lower finalized record hash replaces the local tip after replay validation. Continuous peer sync remains a fallback for peers that were offline or too far behind during direct propagation.

Current development conflict responses:

```text
RECORD_DUPLICATE <hash>
RECORD_REPLACED <incoming_hash> <old_local_hash>
RECORD_CONFLICT_WORSE <incoming_hash> <local_hash>
RECORD_CONFLICT_FORK <incoming_hash> <local_hash>
```

Only the current tip can be replaced. Deep rollback is not implemented.

Inspect the TCP mempool:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_MEMPOOL
```

Generate a fresh chain that pulls TCP mempool transactions into record `4`:

```bash
./build/primechain-sequential 20 ./data/mempool-chain.log ./data/mempool-chain.dat \
  --prime-miner pcdev1_373830813f57da0581a814963cf165b3 \
  --composite-miner pcdev1_composite_miner \
  --mempool 127.0.0.1 18889 4
```

After the generated chain successfully reload-validates, the generator sends `ACK_MEMPOOL` for included transaction hashes. The TCP node removes acknowledged transactions from its in-memory mempool.

Advance the running TCP node's own record store and include pending mempool transactions directly:

```bash
miner=$(./build/primechain-wallet address ./wallets/miner.wallet)
./build/primechain-sync-query 127.0.0.1 18889 ADVANCE_TO 20 $miner pcdev1_composite_miner 4
```

`ADVANCE_TO limit prime_miner composite_miner mempool_target_integer` is a development command. It creates verified arithmetic records up to `limit`, embeds the current mempool at the chosen target record, reload-validates the resulting store, and removes included transactions from the mempool. The TCP server rejects it by default; start the server with `--enable-advance` for local development tests. If you spend prime `3`, use target record `4` or later because record `3` mints the asset before it can be spent.

`ACK_MEMPOOL` is also disabled by default because it removes pending transactions. Start the server with `--enable-ack-mempool` only for local development tests that intentionally acknowledge included transactions.

Expected summary:

```text
sequential chain complete
output_path: ./data/sequential-500.log
record_store_path: ./data/sequential-500.dat
limit: 500
prime_miner_address: pcdev1_prime_miner
composite_miner_address: pcdev1_composite_miner
prime_records: 95
composite_records: 404
```

The `.log` file is human-readable. The `.dat` file is the binary finalized-record store used by `SequentialNode` replay.

Inspect the binary store:

```bash
./build/primechain-store-inspect ./data/sequential-500.dat
```

Expected key fields:

```text
records: 499
prime_records: 95
composite_records: 404
frontier_integer: 500
```

Look up one stored integer record:

```bash
./build/primechain-store-inspect ./data/sequential-500.dat 500
```

Example fields:

```text
integer: 500
kind: COMPOSITE
payload_bytes: ...
```

Look up a range of stored records:

```bash
./build/primechain-store-inspect ./data/sequential-500.dat --range 490 500
```

This prints one line per stored record:

```text
integer height kind hash16 payload_bytes
490 ...
...
500 498 COMPOSITE ...
```

## Run Sync Server

The sync server exposes the binary record store over TCP. By default it binds to `127.0.0.1`, which is safe for local tests.

Terminal 1:

```bash
./build/primechain-sync-server 18889 ./data/sequential-500.dat
```

For a public-server test, bind to all IPv4 interfaces and ensure the server firewall/security group allows the port:

```bash
./build/primechain-sync-server 18889 ./data/public-node.dat --bind 0.0.0.0
```

Peers should connect using the machine's public IPv4 address:

```bash
./build/primechain-sync-server 18890 ./data/node-b.dat --peer PUBLIC_IP 18889 --sync-interval 5
```

Do not expose development write commands on a public port unless the test is intentionally controlled. `ADVANCE_TO` and `ACK_MEMPOOL` remain disabled unless `--enable-advance` or `--enable-ack-mempool` is supplied. Expensive helper work is also opt-in: `GET_FACTORIZATION` remains disabled unless `--enable-factorization-helper` is supplied.

Current TCP safety limits:

```text
max unframed line size: 8192 bytes
max framed message size: 1048576 bytes
max GET_RECORD_RANGE count: 10000 records
max in-memory mempool: 1000 transactions
max known peers: 32
outbound peer connect timeout: 1500 ms
socket read/write timeout: 3000 ms
max commands per TCP connection: 128
max write commands per TCP connection: 16
```

These are first-pass development limits, not production DoS protection. If a configured peer is dead or slow, the node logs a warning and continues trying other known peers. If one TCP connection sends too many commands, the server returns `ERROR rate limit exceeded` and closes that connection.

Peer discovery is first-pass and development-only. Nodes can list and add peers:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_PEERS
./build/primechain-sync-query 127.0.0.1 18889 ADD_PEER 127.0.0.1 18890
```

On startup and during periodic sync, a non-quorum node asks known peers for their peer lists and adds discovered peers up to the current cap. Validator quorum nodes currently use only explicitly configured peers; this avoids same-port self-discovery and synchronous rebroadcast loops until peer identities and advertised public addresses are formalized.

### Current Identity Model

The protocol authenticates consensus objects, but not TCP sessions.

Current identifiers are development-level only:

- peers are known by IPv4 address and port, for example `127.0.0.1:18889`;
- offline fixtures may still use development strings such as `pcdev1_...`;
- authenticated miners, senders, and validators use key-derived `pcpq1_` addresses;
- peer transport is not encrypted or session-authenticated.

This means the current rate-limit layer is intentionally simple:

- per TCP connection;
- per source address if added later;
- global caps such as max mempool size and max known peers.

Composite contributors, prime discoverers, transaction senders, and controlled-testnet validators use NIST ML-DSA-65 `pcpq1_` identities. Prime signatures bind the frontier and complete Pratt proof; transaction signatures bind the complete unsigned transaction. Legacy `pcdev1_` records remain supported only in explicitly unanchored development chains.

Terminal 2:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_STATUS
./build/primechain-sync-query 127.0.0.1 18889 GET_RECORD 500
./build/primechain-sync-query 127.0.0.1 18889 GET_RECORD_RANGE 490 500
./build/primechain-sync-query 127.0.0.1 18889 GET_BALANCE pcdev1_prime_miner
./build/primechain-sync-query 127.0.0.1 18889 GET_PEERS
```

Bootstrap-download records into a fresh local store:

```bash
./build/primechain-sync-download 127.0.0.1 18889 2 500 ./data/downloaded-500.dat
./build/primechain-store-inspect ./data/downloaded-500.dat
```

Download records after a TCP node advanced its own store:

```bash
rm -f ./data/tcp-node-copy.dat
./build/primechain-sync-download 127.0.0.1 18889 2 20 ./data/tcp-node-copy.dat
./build/primechain-wallet balance ./data/tcp-node-copy.dat ./wallets/alice.wallet
```

This is the current node-to-node replay test: the second store is reconstructed only from downloaded records, then wallet balances are derived by replaying those records locally.

Query a running node's replayed wallet state directly:

```bash
alice=$(./build/primechain-wallet address ./wallets/alice.wallet)
./build/primechain-sync-query 127.0.0.1 18889 GET_BALANCE $alice
```

Expected shape:

```text
BALANCE pcdev1_... 1
HOLDING 3 250000
END_BALANCE
```

## Run A Local 3-Node Convergence Test

This is the current bridge toward a real multi-node testnet. It uses three independent localhost TCP servers and three independent record stores.

Terminal 1, node A:

```bash
cd ~/primechain
rm -f ./data/node-a.dat ./data/node-b.dat ./data/node-c.dat
./build/primechain-sync-server 18889 ./data/node-a.dat --enable-advance
```

Terminal 2, advance node A:

```bash
cd ~/primechain
miner=$(./build/primechain-wallet address ./wallets/miner.wallet)
alice=$(./build/primechain-wallet address ./wallets/alice.wallet)

./build/primechain-send submit 127.0.0.1 18889 ./wallets/miner.wallet $alice 3 250000 1
./build/primechain-sync-query 127.0.0.1 18889 ADVANCE_TO 20 $miner pcdev1_composite_miner 4
```

Terminal 3, node B starts and automatically syncs from node A. With `--sync-interval 5`, it keeps checking node A every five seconds:

```bash
cd ~/primechain
./build/primechain-sync-server 18890 ./data/node-b.dat --peer 127.0.0.1 18889 --sync-interval 5
```

Terminal 4, node C starts and automatically syncs from node A:

```bash
cd ~/primechain
./build/primechain-sync-server 18891 ./data/node-c.dat --peer 127.0.0.1 18889 --sync-interval 5
```

Query all three nodes:

```bash
alice=$(./build/primechain-wallet address ./wallets/alice.wallet)
./build/primechain-sync-query 127.0.0.1 18889 GET_BALANCE $alice
./build/primechain-sync-query 127.0.0.1 18890 GET_BALANCE $alice
./build/primechain-sync-query 127.0.0.1 18891 GET_BALANCE $alice
```

Each node should report:

```text
HOLDING 3 250000
```

This proves three separate node stores converge when follower nodes sync from a peer. Continuous sync is polling-based: followers periodically ask peers for status and download missing records. It is not full peer gossip yet.

Resume download in batches:

```bash
./build/primechain-sync-download 127.0.0.1 18889 2 250 ./data/resume-500.dat
./build/primechain-sync-download 127.0.0.1 18889 251 500 ./data/resume-500.dat
./build/primechain-store-inspect ./data/resume-500.dat
```

The downloader rejects duplicate or skipped ranges. A non-empty destination store must resume exactly at `frontier + 1`.

During sync download, the client currently verifies:

- the peer returned the requested range header
- every record payload hash matches the transmitted record hash
- each record arrives in exact integer order
- each record height matches `integer - 2`
- the local output store accepts the record hash
- the completed store replays through `SequentialNode`

During replay, `SequentialNode` currently verifies:

- composite records deserialize and satisfy `d * e = g`
- prime records deserialize and satisfy the stored Pratt proof
- embedded transaction lists match the record transaction count/root
- transaction sender addresses derive from sender public keys
- transaction signatures match the ML-DSA-65 or explicit offline-development rule
- transaction inputs equal outputs plus fees per prime asset
- sender nonces are contiguous and fees are paid to the record proof provider
- record payloads link to the previous record hash
- single-node development records satisfy the deterministic development finalization rule
- quorum records contain two or three canonical ML-DSA-65 validator signatures over the candidate record hash
- every quorum signer belongs to the replay-derived active validator epoch
- mining rewards reconstruct into the in-memory ledger state

Startup and continuous peer sync use a temporary store before replacing the local store. If a hostile peer serves records whose payload hashes are correct but whose arithmetic is invalid, the temporary replay fails and the real local store is left unchanged. This prevents partial poisoning of the local chain during sync.

Single-node development stores retain `fixed-2-of-3-dev`. A validator-anchored chain uses `fixed-2-of-3-mldsa65-v2`: the proposing validator signs first, then collects a second signature over the complete candidate record before append and gossip. Each validator persists its pending signed choice in `<record-store>.finalization`, preventing a restart from permitting a second vote for the same integer and round. The sidecar is cleared after the finalized record arrives.

Enable controlled round recovery with:

```bash
./build/primechain-sync-server 18889 ./data/validator-a.dat \
  --validator-set $a $b $c \
  --validator-identity ./wallets/validator-a.wallet \
  --finalization-timeout-ms 1000
```

If vote collection stalls, the proposer waits for the configured timeout and
requests a signed transition to the next round. Two current validators must
authorize the same frontier hash, integer, and round. The next candidate embeds
that certificate and may differ from the candidate signed in the abandoned
round. Temporary votes are stored in `<record-store>.rounds`; finalized records
remain replayable after `.rounds` and `.finalization` are removed. The default
timeout is `0`, which preserves fail-fast behavior.

This is controlled-testnet finalization, not permissionless Sybil resistance.
The timeout is a local trigger and is not trusted consensus time. Existing
pre-migration quorum stores must be regenerated.

Development reward rule: every mined prime asset has `1,000,000` integer micro-units. Unanchored development chains keep the old discovery-only split: if no composite records appeared since the previous prime, the prime miner receives the full asset; otherwise the prime miner receives half and composite proof providers split the other half. Validator-set chains use the fixed launch/mainnet split for every non-genesis prime: 45% prime prover, 45% composite providers, and 10% credited to `pcpool_validator_rewards_epoch_<epoch>` for later deterministic validator distribution. Do not upgrade old economics chains in place; start a fresh chain when changing reward policy.

Development wallet/address tools:

```bash
./build/primechain-wallet new ./data/alice.wallet
./build/primechain-wallet address ./data/alice.wallet
./build/primechain-wallet balance ./data/sequential-500.dat ./data/alice.wallet
./build/primechain-balance ./data/sequential-500.dat pcdev1_prime_miner
```

Wallet addresses are local and are not recorded on-chain when created. A key-derived address appears in the ledger only when a transaction or reward references it. Live TCP transactions use ML-DSA-65; deterministic development signatures remain only for unanchored offline fixtures.

## Recreate Current Prototype From GitHub

The current implementation does not depend on any private server state. The source of truth is the public GitHub repository. To recreate the working prototype on a suitable Linux machine:

```bash
git clone https://github.com/midlincoln/primechain.git
cd primechain
cmake -S . -B build
cmake --build build
cd build
ctest --output-on-failure
```

If the machine has an older CMake but supports C++17:

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
ctest --output-on-failure
```

Known minimum practical toolchain for this prototype:

```text
C++17 compiler, e.g. g++ 7+
CMake
make
git
```

Local `.dat` chain stores and `.wallet` files are test artifacts. They can be regenerated from the code and are not required to rebuild or continue development.

`SequentialNode` appends to the local `RecordStore` only after record validation succeeds. Rejected records do not advance the persisted frontier.

This is the first local peer-sync API. It is still plain TCP and development-only.


### Validator Epoch Rotation Workflow

A quorum node reports the exact target that validators must sign:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_VALIDATOR_EPOCH
# VALIDATOR_EPOCH <current_epoch> <next_integer> <current_tip_hash>
```

Create the three replacement validator identities first, sort their `pcpq1_`
addresses lexicographically, then have at least two validators from the current
set independently sign the same proposal:

```bash
vote=$(./build/primechain-composite-commitment sign-epoch \
  ./wallets/validator-a.wallet <current_tip_hash> <next_integer> <next_epoch> \
  <next_validator_a> <next_validator_b> <next_validator_c>)
./build/primechain-sync-query 127.0.0.1 18889 $vote
```

Inspect the pending certificate with:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_EPOCH_VOTES
```

Votes are validated, atomically stored in `<record-store>.epochs`, and gossiped
to configured peers. Once two current validators sign the same proposal, the
next accepted prime or composite record automatically becomes version 2 and
embeds the epoch transition. The old set authorizes that record; the new set is
active from the following integer. The temporary epoch-vote file is then cleared.

After rotation, nodes still pass the original genesis addresses to
`--validator-set`. A newly activated validator supplies its own identity through
`--validator-identity`; replay confirms that identity belongs to the active epoch.

## Development Roadmap

Completed prototype milestones:

- TCP node and block miner
- persistent prime-block chain log
- cooperative proof pool
- separate sharded composite miner
- contributor logging
- long-run chain scale estimator
- sequential arithmetic record store
- wallet/reward replay
- TCP transaction submission and mempool inspection
- startup and continuous peer sync
- development mempool propagation to configured peers
- development record propagation to configured peers
- same-tip record conflict classification
- safe same-tip replacement for lower-hash records
- two active writers converge on the lower same-tip record in local TCP tests
- bind-address option for public-server tests
- first-pass TCP message, range, and mempool limits
- first-pass peer discovery with `GET_PEERS` and `ADD_PEER`
- first-pass peer connection/read timeouts and dead-peer warnings
- basic per-connection TCP rate limits
- first miner-submitted composite flow with `SUBMIT_COMPOSITE`
- authenticated miner-submitted Pratt prime flow with `SUBMIT_SIGNED_PRIME`
- frontier miner loop using authenticated composite and prime submissions
- genesis-anchored 2-of-3 validator quorum
- signed validator epoch transitions embedded in version-2 arithmetic records
- replay-derived active validator set with next-integer activation
- ML-DSA-65 2-of-3 signatures over complete prime and composite candidate records
- persistent validator anti-equivocation state in `.finalization` sidecars
- signed 2-of-3 finalization round changes with embedded replay evidence
- optional timeout-driven retry through `--finalization-timeout-ms`
- operational transaction fees, validator fee pools, deterministic pool distribution, and contiguous sender nonces
- NIST ML-DSA-65 signatures for transactions, miners, validators, epochs, and round changes
- framed TCP messages for PQ-sized keys, signatures, and records
- synchronized chain appends with incomplete-tail recovery
- durable, automatically rebuilt integer-to-record-offset indexes
- atomic tip replacement and peer-sync store installation
- synchronized sidecar replacement and stale-temp recovery
- atomic replay snapshots with stale/corrupt fallback and suffix-only replay
- unified `primechain-client` for workdir setup, peer sync, persistent mining jobs, reward totals/history, local proof indexes, status, inspection, identity creation, balances, and local math workbench commands

Next milestones:

1. Expand `primechain-client` with reward filtering/export, broader local indexes, and Bitcoin mapping experiments.
2. Enforce deterministic state roots and design verifiable archival pruning.

The first engineering principle is simple: keep consensus small, explicit, and testable before adding network complexity.

## Production Backlog

The staged implementation backlog is documented in [docs/production-roadmap-v0.md](docs/production-roadmap-v0.md).

Current production-track priority:

```text
canonical records -> genesis validator anchor -> validator epochs -> production cryptography
```
