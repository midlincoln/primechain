# Prime Mining Prototype

This repository is the implementation start for Prime Mining: a deterministic prime-chain proof-of-work protocol.

The current code is a small C++ consensus prototype. It validates local blocks that advance from one prime to the next and require composite proofs for every integer between the previous frontier prime and the proposed next prime.

## Current Scope

Implemented:

- CMake C++17 project structure
- core protocol data types
- development-only deterministic block hash
- small-integer primality checks
- composite proof generation and verification
- TCP node listening on localhost
- terminal miner that submits blocks to the TCP node
- append-only disk chain log for accepted test blocks
- consensus validation for:
  - previous hash linkage
  - next-prime rule
  - composite interval bounds
  - required composite proof coverage
- consensus tests

Not implemented yet:

- real SHA3-256
- real ECPP or Pratt certificates
- post-quantum signatures
- commit-reveal persistence
- wallet state and sparse fractional balances
- Merkle state commitments
- P2P networking

## Build

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

## Run Tests

```bash
cd build
ctest --output-on-failure
```

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

Submit a signed development transaction to a running TCP node:

```bash
./build/primechain-sync-server 18889 ./data/send-chain.dat --enable-ack-mempool
./build/primechain-send submit 127.0.0.1 18889 \
  ./wallets/miner.wallet pcdev1_1654a887b941f792dd86094f19e90479 3 250000 1
```

The TCP node currently validates the transaction signature/address and stores accepted transactions in an in-memory development mempool. If the node was started with `--peer`, newly accepted transactions are forwarded to configured peers with the same `SUBMIT_TX` command. Duplicate transaction hashes are ignored, which prevents simple propagation loops.

Create a cryptographic miner identity for signed composite mining:

```bash
./build/primechain-wallet new-miner ./wallets/composite-miner.wallet
./build/primechain-wallet miner-address ./wallets/composite-miner.wallet
```

This creates an Ed25519 keypair and a key-derived `pc1_...` address. The private
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

The node derives the address from the public key and verifies both Ed25519
signatures before accepting the commitment or reveal. The public key, nonce,
and reveal signature are retained in the finalized composite record, allowing
independent verification during chain replay and synchronization. Signed
commitments retain their authentication evidence in the persistent commitment
sidecar and during peer propagation.

The frontier miner can use the identity directly:

```bash
./build/primechain-frontier-miner 127.0.0.1 18889 100 \
  pcdev1_prime_miner --composite-identity ./wallets/composite-miner.wallet
```

Ed25519 is a real classical signature scheme, but it is not post-quantum. This
milestone establishes authenticated miner identity before the planned PQ
replacement. The current commitment and snapshot hashes remain `devHash256`; they are not production-ready.

### Controlled 2-of-3 Commit Phase

Quorum mode is opt-in and intended for a controlled testnet. Create three
Ed25519 identities and configure every validator with the same ordered set:

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
The helper can sign a vote for controlled tests:

```bash
./build/primechain-composite-commitment \
  sign-phase ./wallets/validator-b.wallet 4 $snapshot_hash
```

This is not permissionless consensus. Fixed validator membership is manual, and
the phase certificate is currently transient sidecar state rather than a
permanent field in the finalized arithmetic record. Embedding the certificate
in canonical records is required before an adversarial testnet.

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
are not finalized consensus records. The current `devHash256` implementation tests the
protocol flow but is not production cryptography; SHA3-256 or another selected
production hash must replace it before an adversarial testnet. This version
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

Submit one miner-produced Pratt prime proof to a running TCP node:

```bash
./build/primechain-sync-query 127.0.0.1 18889 \
  SUBMIT_PRIME 5 2 1 2 2 pcdev1_prime_miner
```

Format:

```text
SUBMIT_PRIME p witness factor_count factor_1 exponent_1 ... provider_address
```

The example certifies `5` using witness `2` and factorization `5 - 1 = 2^2`. For `11`, whose `p - 1` factorization is `2^1 * 5^1`, the line shape is:

```bash
./build/primechain-sync-query 127.0.0.1 18889 \
  SUBMIT_PRIME 11 2 2 2 1 5 1 pcdev1_prime_miner
```

The current prototype accepts Pratt certificates only. Future protocol versions can add certificate-type fields for ECPP/APR-CL without changing the basic rule that nodes verify the certificate locally before advancing.

Current success response:

```text
PRIME_ACCEPTED <p> <record_hash>
```

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
./build/primechain-frontier-miner 127.0.0.1 18889 20 \
  pcdev1_prime_miner pcdev1_composite_miner
```

The frontier miner repeatedly:

- asks the node for `GET_STATUS`;
- tests `frontier + 1`;
- submits `SUBMIT_COMPOSITE` when the next integer is composite;
- submits `SUBMIT_PRIME` with a Pratt proof when the next integer is prime;
- stops when the node frontier reaches the requested limit.

This is the first real mining flow for the sequential arithmetic chain. It is still a prototype: it keeps its composite proof index locally during the run, so it works best from a fresh node or a node whose needed composite proofs were mined by this same process.

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
max line size: 8192 bytes
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

On startup and during periodic sync, a node asks known peers for their peer lists and adds discovered peers up to the current cap.

### Current Identity Model

The prototype does not yet identify TCP clients cryptographically.

Current identifiers are development-level only:

- peers are known by IPv4 address and port, for example `127.0.0.1:18889`;
- wallet addresses are development strings such as `pcdev1_...`;
- miner and composite provider fields are address strings embedded in records;
- transaction records include a development signature/hash check, but this is not yet a production key/address scheme;
- peer messages are not signed, and nodes do not yet have persistent public-key identities.

This means the current rate-limit layer is intentionally simple:

- per TCP connection;
- per source address if added later;
- global caps such as max mempool size and max known peers.

Composite contributors and controlled-testnet validators may now use Ed25519 `pc1_` identities. Signed commit-phase votes freeze a shared candidate snapshot at a 2-of-3 threshold. Prime submissions, transactions, permanent in-record quorum certificates, post-quantum signatures, and legacy `pcdev1_` paths still require migration.

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
- development transaction sender addresses derive from sender public keys
- development transaction signatures match the deterministic dev signature rule
- transaction debits and credits balance per prime asset
- record payloads link to the previous record hash
- development validator votes point to the candidate record hash
- development validator signatures match the deterministic dev signature rule
- mining rewards reconstruct into the in-memory ledger state

Startup and continuous peer sync use a temporary store before replacing the local store. If a hostile peer serves records whose payload hashes are correct but whose arithmetic is invalid, the temporary replay fails and the real local store is left unchanged. This prevents partial poisoning of the local chain during sync.

The current finalization rule is development-only: `fixed-2-of-3-dev`. If this format changes, regenerate old local `.dat` stores with `primechain-sequential`.

Development reward rule: every mined prime asset has `1,000,000` integer micro-units. If no composite records appeared since the previous prime, the prime miner receives the full asset. Otherwise the prime miner receives half, and composite proof providers split the other half.

Development wallet/address tools:

```bash
./build/primechain-wallet new ./data/alice.wallet
./build/primechain-wallet address ./data/alice.wallet
./build/primechain-wallet balance ./data/sequential-500.dat ./data/alice.wallet
./build/primechain-balance ./data/sequential-500.dat pcdev1_prime_miner
```

Wallet addresses are local and are not recorded on-chain when created. A key-derived address appears in the ledger only when a transaction or reward references it. Current development transaction signatures are deterministic placeholders, not production cryptography.

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
- first miner-submitted Pratt prime flow with `SUBMIT_PRIME`
- first frontier miner loop using `SUBMIT_COMPOSITE` and `SUBMIT_PRIME`

Next milestones:

1. Add proof-index bootstrap so miners can resume from an existing node store.
2. Add sync rejection tests for corrupt prime proofs.
3. Add stricter propagation timing tests for active writers.
4. Add commit-reveal and contributor authentication.
5. Later add production hashing, real signatures, and post-quantum signatures.

The first engineering principle is simple: keep consensus small, explicit, and testable before adding network complexity.

## Production Backlog

The staged implementation backlog is documented in [docs/production-roadmap-v0.md](docs/production-roadmap-v0.md).

Current production-track priority:

```text
canonical disk records -> sequential node -> peer sync -> controlled validator finalization
```
