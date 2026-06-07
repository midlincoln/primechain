# Development Log

This file records important project decisions and unresolved questions so the architecture does not depend on chat history.

## 2026-06-04: Cooperative Arithmetic Chain Direction

### Working Prototype

The repository currently implements:

- a C++17 TCP node,
- a terminal block miner,
- a separate composite miner,
- a node-side composite proof pool,
- sharded composite mining for multiple contributors,
- contributor logging when prime blocks close,
- an append-only disk-backed prime-block chain log,
- restart recovery,
- a chain-scale estimator.

The cooperative test has been demonstrated with two composite miners and one block miner. Accepted prime blocks preserve and display proof-contributor attribution.

### Proposed Core Model

The emerging design treats the database itself as the blockchain. Every integer after the current frontier receives one permanent finalized classification record:

```text
PRIME or COMPOSITE
```

A composite classification contains a valid nontrivial factorization:

```text
g = d * e
```

A prime classification contains a valid primality certificate, initially expected to be a Pratt certificate for test-sized integers.

Example:

```text
7  PRIME
8  COMPOSITE: 2 * 4
9  COMPOSITE: 3 * 3
10 COMPOSITE: 2 * 5
11 PRIME: Pratt certificate
```

In this model, a composite proof is similar to a mini-block. It advances the certified integer frontier by one and creates a pending contributor reward.

### Why Stored Composite Proofs Matter

One stored nontrivial factor pair per composite is sufficient to recursively reconstruct its complete prime factorization, provided the factor records ultimately reach previously certified primes.

For example:

```text
30 = 2 * 15
15 = 3 * 5
therefore 30 = 2 * 3 * 5
```

When testing frontier integer `g`, the record for `g - 1` already exists. Its complete factorization can therefore be reconstructed quickly from the arithmetic chain.

That factorization directly helps construct a Pratt certificate for `g`.

### Frontier Classification Work

Miners may run different strategies simultaneously:

- composite miners search for a divisor of `g`,
- Pratt miners reconstruct the factorization of `g - 1` and search for a valid Pratt witness,
- specialized third-party miners may use any algorithm that produces a valid protocol proof.

Nodes do not trust miner software. Nodes independently verify submitted proofs.

### Pratt Certificate Relationship

Given the complete prime factorization of `g - 1`, a Pratt/Lucas-style proof searches for a witness `a` satisfying the required modular-order conditions.

If a valid witness is submitted, `g` is certified prime.

Failure to find a witness after a limited search does not prove compositeness. An exhaustive proof that no witness exists would imply compositeness, but directly finding a divisor will usually be more useful.

### Competing Composite Proofs

Several miners may submit different valid proofs for the same frontier integer. Mathematical validity alone does not determine contributor credit.

Timestamps cannot establish a trustworthy global first submission in an asynchronous network:

- miner clocks can lie,
- node clocks differ,
- network latency creates different local arrival orders.

Bitcoin also does not use timestamps to determine which competing block arrived globally first.

### Initial Consensus Direction

For the controlled multi-node testnet, the preferred initial direction is a fixed validator set using 2/3 voting:

1. Miners broadcast proof candidates.
2. Validators independently verify candidates for the current frontier integer.
3. Validators vote on a candidate.
4. A candidate with 2/3 approval becomes the finalized arithmetic record.
5. All nodes advance to the next integer.
6. Contributor credit becomes pending or finalized according to later reward rules.

With three controlled validator nodes, two matching votes finalize a record.

This is intentionally not yet a permissionless consensus mechanism. It provides a concrete way to test arithmetic-record finalization and node synchronization.

### Deterministic Proof Selection Ideas

Ideas discussed for selecting among multiple valid composite proofs include:

- first candidate finalized by 2/3 validator voting,
- canonical factorization based on the smallest divisor or largest cofactor,
- deterministic selection among eligible commitments,
- divided rewards among eligible contributors.

No final rule has been selected. A deterministic factor rule can define canonical mathematical representation, but it does not by itself determine which miner deserves attribution.

### Spam And Resource Risks

Before public networking, nodes require:

- bounded proof work windows,
- maximum message sizes,
- connection and per-IP rate limits,
- proof-pool size limits,
- duplicate rejection,
- cheap validation before storage,
- safe parsing,
- temporary bans for repeated invalid submissions,
- separation between temporary candidates and finalized disk records.

The current prototype must remain local or controlled until these protections exist.

### Storage Model

Node roles should eventually include:

- archival nodes storing all finalized arithmetic records and contributor history,
- pruned full nodes retaining current state and required recent/history proofs,
- light miners requesting proofs and factor data from full nodes,
- research nodes maintaining optimized searchable factor indexes.

At large scale, arithmetic records may be comparable in size to existing blockchain datasets. Compact factor-only records can be much smaller than rich signed mini-block records.

### Immediate Engineering Plan

1. Define canonical sequential arithmetic record structures.
2. Persist every finalized composite or prime record.
3. Build a disk-backed factor index.
4. Implement recursive complete-factorization reconstruction.
5. Implement small-number Pratt certificate generation and verification.
6. Add strict anti-spam limits.
7. Implement three-node synchronization and simulated 2/3 voting.
8. Revisit contributor selection and reward finalization before public testing.

### Important Distinction

The current code still uses a temporary proof pool and prime-closing blocks. The sequential arithmetic-record chain described here is the proposed next architecture and has not yet been implemented.

## 2026-06-05: Transaction Batching And Bitcoin Mirror Test Direction

### Transaction Model Direction

The project should not require exactly one transaction per composite proof. That rule is mathematically clean, but it does not scale well and is especially awkward for mirroring Bitcoin, where a single Bitcoin block can contain thousands of transactions.

The better working direction is:

```text
one finalized arithmetic record
    -> one proof of PRIME or COMPOSITE
    -> zero or more transactions
    -> transaction Merkle root
```

For a composite record:

```text
CompositeRecord {
    integer: g,
    factor: d,
    cofactor: e,
    transaction_count,
    transaction_merkle_root,
    proof_provider
}
```

For a prime record:

```text
PrimeRecord {
    integer: g,
    primality_certificate,
    transaction_count,
    transaction_merkle_root,
    prime_provider
}
```

Prime records should also allow transaction batches. Otherwise transaction inclusion would pause whenever the next frontier integer is prime.

### Bitcoin Mirror Test

A useful long-running test is to mirror Bitcoin into Primechain as an external reference stream.

The simplest coherent rule is:

```text
one Bitcoin block
    -> one Primechain prime-to-prime interval

all Bitcoin transactions in that block
    -> deterministically distributed across the interval's arithmetic records

final prime record
    -> commits to the Bitcoin block hash and Bitcoin transaction Merkle root
```

This means a Bitcoin block is not mapped to one composite proof. Its transactions are batched across the composite and prime records belonging to that interval.

This test would exercise:

- disk-backed arithmetic record storage,
- transaction batching,
- deterministic replay,
- restart recovery,
- Bitcoin reorganization handling,
- multi-node agreement on the same external history.

It would not prove Primechain's independent permissionless security, because Bitcoin supplies the external checkpoints. It is still valuable as a realistic synchronization and storage stress test.

### Energy And Consensus Position

The strongest defensible comparison is with proof-of-work systems, not proof-of-stake systems.

Primechain does not eliminate all wasted computation. Duplicate work, losing proof candidates, and already-known arithmetic still exist. The intended improvement is that accepted work becomes a reusable verified arithmetic history rather than being discarded hash search.

Bitcoin's wasteful hash race also provides permissionless Sybil resistance. Primechain must still define an equally credible permissionless consensus mechanism before it can claim production-grade security. The fixed 2/3 validator model is only a controlled testnet mechanism.

### Next Engineering Step

The next code milestone should be an arithmetic-record benchmark mode:

1. Generate sequential arithmetic records from the current frontier.
2. Store each finalized integer classification on disk.
3. Allow each record to contain a synthetic transaction batch.
4. Measure records per second, transactions per second, disk growth, and restart replay speed.
5. Keep the current prime-block miner working until the sequential record model replaces it cleanly.

This benchmark is more important than wallets or post-quantum signatures right now, because it tests whether the proposed core database can evolve quickly and persist reliably.

## 2026-06-07: Composite Commit-Reveal Prototype

The first TCP commit-reveal path is implemented:

```text
SUBMIT_COMMIT g commitment_hash provider_address
SUBMIT_COMPOSITE_REVEAL g d e nonce provider_address
```

The canonical development commitment binds `g`, `d`, `e`, `nonce`, and the
provider address under a domain-separated hash. Nodes keep a bounded in-memory
commitment pool, propagate commitments to known peers, reject unmatched
reveals, and perform normal composite arithmetic validation before finalizing a
record. The frontier miner now commits before revealing composite factors.

This milestone provides reveal-time proof-theft resistance, but not global
commit ordering. Commitments are not yet persistent consensus records, and node
arrival times are not a globally reliable ordering source. A later protocol
step must define a commit phase boundary plus quorum/finality rules before the
project claims that the earliest network commitment deterministically receives
the composite reward.
