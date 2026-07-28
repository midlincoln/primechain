# Primechain Production Backlog v0

This document turns the unresolved production items into an implementation order.

The current code is a development/testnet prototype. The goal is to build a real v0 experimental node without overstating production security.

## Stage 1: Canonical Disk Records And Indexes

Status: implemented for the canonical arithmetic-record store. Atomic local
replay snapshots are implemented. Pruning remains prohibited until state roots
are consensus-enforced; factor indexes, the expanded mining workbench, and large-dataset compaction remain
future work.
Temporary coordination sidecars also use synchronized atomic replacement and
validated stale-temp recovery.

Purpose: make the sequential arithmetic chain persistent and replayable.

Deliverables:

- binary record-store envelope,
- append-only finalized record log,
- replay with record-hash verification,
- latest frontier lookup,
- `integer -> record metadata` index,
- synchronized append with incomplete-tail recovery,
- atomic tip replacement and validated peer-sync installation,
- automatic index validation and rebuild,
- checksummed replay snapshots anchored to an exact record hash,
- suffix-only replay with full-replay fallback for stale or corrupt snapshots,
- later: factor and prime indexes.

Why first:

- sync depends on records,
- voting signs record hashes,
- transactions commit to record state,
- wallet reconstruction depends on replay.

## Stage 2: Sequential Node Core

Purpose: replace one-off tools with a reusable node object.

Deliverables:

- `SequentialNode`,
- append candidate composite record,
- append candidate prime record,
- verify next-integer rule,
- update disk store,
- expose status/frontier.

## Stage 3: Multi-Node Sync

Purpose: let a new node download already-mined records from peers and verify locally.

Deliverables:

- `GET_STATUS`,
- `GET_RECORD`,
- `GET_RECORD_RANGE`,
- batch sync,
- replay verification from genesis.

## Stage 4: Controlled 2-of-3 Finalization

Purpose: resolve competing proof candidates in the controlled testnet.

Deliverables:

- validator config,
- validator vote object,
- vote verification placeholder,
- 2-of-3 finalization rule,
- rejected duplicate/conflicting candidate handling.

This is not permissionless consensus.

## Stage 5: Signatures And Addresses

Purpose: replace placeholders with real identity binding.

Deliverables:

- development keypair format,
- signed proof submissions,
- signed validator votes,
- signed transactions,
- NIST ML-DSA-65 signatures for authenticated protocol identities.

## Stage 6: Transaction State And Wallet Ownership

Purpose: make prime-indexed assets transferable.

Deliverables:

- UTXO or account model decision,
- transaction validation,
- sparse prime ownership index,
- address balances,
- transaction batch Merkle roots.

Reserve-backed validator governance depends on encrypted/passphrase-protected
protocol wallets and a clean public transfer UX. The validator economy design is
tracked in `docs/validator-economy-v0.md`.

## Stage 7: Rewards

Purpose: pay contributors deterministically.

Deliverables:

- prime-discovery reward,
- composite-proof reward pool,
- transaction fee distribution,
- contributor accounting.

Future reward work should split newly minted prime assets into discovery,
validator-finality, and treasury pools according to policy-epoch parameters.
The current hardcoded development reward split remains a prototype rule.

## Stage 8: Spam And DoS Protection

Purpose: make public TCP testing survivable.

Deliverables:

- maximum message sizes,
- proof-window limits,
- per-peer rate limits,
- candidate pool limits,
- cheap rejection before storage,
- temporary bans.

## Stage 9: Reorg/Fork Handling

Purpose: handle conflicting histories cleanly.

Deliverables:

- controlled-testnet conflict rules,
- candidate branch storage,
- rollback/replay,
- finalized-record immutability after vote threshold.

## Current Priority

Implement Stage 1 first. Everything else depends on durable canonical records.

## Stage 8: Unified Mining Client

Status: first operator wrapper, local math workbench, persistent workdir
workflow, resumable mining job state, and a rebuildable composite-proof index
implemented. The current `primechain-client` combines workdir initialization,
peer sync, add/run/clear mining jobs, job status with pending/complete/failed
state, authenticated frontier mining, balance reporting, reward totals and
per-record reward history, cached factorization and Pratt construction from
downloaded records, status, query,
range sync, store inspection, miner identity creation, primality checks, and
divisor search.

Remaining work: reward filtering/export, broader local indexes, and future
Bitcoin-mapping experiments.
