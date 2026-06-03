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

## Development Roadmap

1. Replace development hash with SHA3-256.
2. Add canonical serialization for every consensus object.
3. Add Merkle roots for composite proofs and transactions.
4. Implement commit-reveal objects and validation.
5. Add sparse wallet state and fractional transfer validation.
6. Add cooperative proof pool and separate composite miner.
7. Add real primality certificates.
8. Add multi-peer networking beyond localhost miner submissions.

The first engineering principle is simple: keep consensus small, explicit, and testable before adding network complexity.
