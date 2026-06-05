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
- the stored factorization of `g - 1` helps miners construct a Pratt certificate for frontier integer `g`,
- miners may use any search algorithm, while nodes independently verify submitted proofs,
- a controlled multi-node testnet can initially use 2/3 validator voting to finalize competing valid submissions.

This architecture is still under design. Important unresolved questions include validator membership, deterministic candidate selection, Sybil resistance, spam limits, reward attribution, and multi-node synchronization.

See [docs/development-log.md](docs/development-log.md) for the current design discussion and decisions.

The first development format specification is [docs/protocol-formats-v0.md](docs/protocol-formats-v0.md). It defines draft object formats for arithmetic records, composite proofs, Pratt prime proofs, transaction batches, addresses, and controlled 2-of-3 validator finalization.

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

This local tool writes one arithmetic record for every integer from `2` through the requested limit. Composites get factor proofs and full recursive factorizations. Primes get small-number Pratt proofs using the stored factorization of `p - 1`.

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

The sync server exposes the binary record store over localhost TCP.

Terminal 1:

```bash
./build/primechain-sync-server 18889 ./data/sequential-500.dat
```

Terminal 2:

```bash
./build/primechain-sync-query 127.0.0.1 18889 GET_STATUS
./build/primechain-sync-query 127.0.0.1 18889 GET_RECORD 500
./build/primechain-sync-query 127.0.0.1 18889 GET_RECORD_RANGE 490 500
```

Bootstrap-download records into a fresh local store:

```bash
./build/primechain-sync-download 127.0.0.1 18889 2 500 ./data/downloaded-500.dat
./build/primechain-store-inspect ./data/downloaded-500.dat
```

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

Next milestones:

1. Add an arithmetic-record benchmark that persists one classification record per integer.
2. Add synthetic transaction batches and Merkle roots to arithmetic records.
3. Specify canonical `CompositeRecord`, `PrattPrimeRecord`, `ValidatorVote`, and `FinalizedRecord` formats.
4. Add an indexed factor database and recursive factorization reconstruction.
5. Implement small-number Pratt certificate generation and verification.
6. Add strict message, connection, proof-window, and pool-size limits.
7. Implement three-node synchronization and simulated 2/3 voting.
8. Add commit-reveal and contributor authentication.
9. Later add wallets, rewards, production hashing, and post-quantum signatures.

The first engineering principle is simple: keep consensus small, explicit, and testable before adding network complexity.

## Production Backlog

The staged implementation backlog is documented in [docs/production-roadmap-v0.md](docs/production-roadmap-v0.md).

Current production-track priority:

```text
canonical disk records -> sequential node -> peer sync -> controlled validator finalization
```
