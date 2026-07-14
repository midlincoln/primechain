# Development Log

This file records important project decisions and unresolved questions so the architecture does not depend on chat history.

## 2026-07-14: Validator Eligibility Report

Added `primechain-client validator-eligibility <record-store> <address> --reserve
<micro-units> --observed <ok> --total <count>`. The command checks replayed
Primechain work history against the v0 work-score formula, then combines it
with explicit reserve and endpoint-observation inputs. This is still a report,
not an admission record, but it makes the proof-of-work qualification rule
inspectable before adding on-chain candidate and admission events.

## 2026-07-14: Replay-Derived Validator Registry Foundation

Added a read-only validator registry replay layer. It derives active validators
from finalized chain data: anchored genesis validator configuration and signed
validator epoch transitions. The new `primechain-client validator-registry
<record-store>` command reports current epoch, active validators, and registry
history events.

This is the first implementation slice of the validator/gossip architecture. It
does not yet add candidate, reserve-lock, admission-vote, endpoint-update, or
gossip behavior. It makes validator membership an explicit replayed chain view
so those records can be added without relying on local `--validator-set` config
as the long-term authority.

## 2026-07-14: Validator Gossip And Admission Architecture

Recorded the target architecture for validator membership, validator-owned
coordination, and gossip in `docs/validator-gossip-architecture-v0.md`.
Validator authority should come from replayed chain records, not local service
files. Gossip should be runtime liveness state, not consensus authority.

The design uses frontier-integer epochs so validator membership and policy are
constant during an epoch. Validator admission should be on-chain and require
address-bound work history, reserve lock, endpoint observation, validator vote,
and delayed activation. The target coordination model moves quorum collection
out of client miners and into validator-owned gossip/finality.

## 2026-07-14: Encrypted Protocol Wallets

Protocol wallets created by `primechain-wallet new-miner` and
`primechain-client new-miner` now use encrypted wallet format
`pc-miner-mldsa65-v3`. The ML-DSA-65 private key is encrypted with
scrypt-derived 256-bit key material and AES-256-GCM. Public wallet metadata
(address, algorithm, public key, KDF parameters, salt, nonce, and tag) remains
cleartext so addresses can be inspected without unlocking the signing key.

Signing operations require a passphrase. Interactive use prompts for it;
unattended validators and miners should set `PRIMECHAIN_WALLET_PASSPHRASE` in
their service environment. Legacy plaintext v2 protocol wallets remain readable
for transitional tests, but new public testnets should use fresh encrypted
wallets and can reset the old experimental chain.

## 2026-07-13: Board Report And Validator Reputation Commands

Added first-pass `primechain-client board-report <record-store> --from <n> --to <m>`
and `primechain-client validator-reputation <record-store> <address>` commands.
These commands do not change consensus. They replay existing finalized records
and produce the facts needed for policy-board review: record counts,
prime/composite mix, discovery and fee rewards, unique miners, validator
finalization evidence, and address-bound mining history. This is the measurable
layer that should precede validator applications, reserve locks, and policy
votes.

## 2026-07-13: Reserve-Backed Validator Economy Direction

Primechain's architecture separates arithmetic discovery from settlement
finality. Miners produce prime and composite evidence; validators provide public
endpoints, finality, coordination, timeout recovery, sync service, and policy
governance. The emerging economic model therefore prices those roles
separately.

The current direction is a reserve-backed validator system:

- validator operators use separate signing, reserve, income, and optional admin
  wallets,
- mining history is address-bound and non-transferable,
- validator reserve funds are locked and later unbonded only after delay,
- validator rewards are paid to income wallets, not reserve wallets,
- board meetings are represented as integer-range policy epochs,
- validators review network data and vote on reward/reserve parameters within
  hard protocol bounds,
- admission/removal will require explicit on-chain events and delayed
  activation.

The detailed design target is recorded in `docs/validator-economy-v0.md`.

Before implementing reserve governance, the wallet layer should gain
passphrase-protected protocol wallets and a clean transfer UX. The current
public testnet remains experimental and can be reset before a reserve/governance
testnet.

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

## 2026-06-07: Deterministic Composite Candidate Selection

Nodes now select the lexicographically smallest
`(commitment_hash, provider_address)` pair among commitments known for the
frontier integer. `GET_COMMIT_WINNER g` exposes that selection. Composite
reveals from other commitments are rejected. A two-node integration test sends
the same commitments in opposite orders and verifies identical selection.

This closes arrival-order nondeterminism for an identical candidate set. It is
not yet a fairness proof: nonce grinding is possible, commitments are not
persistent, and finalization has no quorum-defined commit-phase boundary.

## 2026-06-07: Persistent Frontier Commitments

Unresolved commitments are now stored in a separate canonical binary sidecar
`<record-store>.commitments`. The file is atomically replaced, loaded on node
startup, synchronized from peers with `GET_COMMITMENTS g`, and pruned when the
corresponding integer is finalized or the frontier advances. Tests cover
restart recovery, peer import, reveal after recovery, and post-finalization
cleanup.

These records remain candidate state rather than finalized blockchain history.
Signatures and a quorum-defined commit/reveal phase are still required for a
production fairness claim.

## 2026-06-07: Signed Composite Miner Identities

Added Ed25519 `pc1_` miner identities, signed commitment and reveal commands,
and frontier-miner support through `--composite-identity`. Nodes verify address
derivation and signatures before accepting messages. Commitment persistence
and peer synchronization retain the public key and commit signature. Finalized
composite records retain the public key, nonce, and reveal signature, which are
verified again during blockchain replay before contributor rewards are applied.

Tests cover identity creation, valid signed flow, forged signatures, restart,
peer synchronization, signed frontier mining, and reward replay. Ed25519 is
classical rather than post-quantum; `devHash256`, prime authentication,
transaction signatures, and validator finalization remain future migrations.

## 2026-06-07: Signed 2-of-3 Commit-Phase Quorum

Added an opt-in controlled-testnet quorum mode configured with three fixed
Ed25519 validator addresses and one local validator identity per node. The first
signed vote freezes the exact canonical commitment snapshot; two distinct valid
votes close the phase and permit only the deterministic winning commitment to
reveal. Unsigned commitments/reveals, late commitments, direct composite
submission, outsider votes, conflicting snapshots, and mismatched peer records
are rejected.

Votes use domain-separated Ed25519 signatures, persist atomically in
`<record-store>.phases`, survive restart, synchronize from peers, and propagate
across a three-node topology. Tests cover the full OPEN/CLOSING/CLOSED flow,
bypass rejection, unauthorized validators, restart recovery, and end-to-end
record propagation.

This remains controlled-testnet machinery. Validator membership is manual,
`devHash256` still hashes snapshots, and the quorum certificate is sidecar state
rather than part of the permanent arithmetic record. Embedding and replaying the
certificate from chain history is the next consensus-format step.

## 2026-06-07: Embedded Commit-Phase Certificates

Quorum-mode composite records now use version 1 and permanently embed the
canonical signed commitment snapshot, the three-address validator set, and the
2-of-3 signed phase votes. Replay and peer ingestion independently verify miner
commit signatures, the snapshot hash, validator membership within the embedded
set, validator signatures, deterministic winner selection, and the winning
signed reveal.

The three-node integration test now stops all validators, removes the temporary
`.commitments` and `.phases` sidecars, and restarts a node from the arithmetic
record store alone. Unit tests reject altered snapshots, validator signatures,
and reveal signatures.

This removes sidecar dependence from finalized history. It does not yet solve
validator-set authorization: the controlled testnet still configures three
validators manually, and production consensus must anchor membership in genesis
or signed validator-epoch transitions.

## 2026-06-07: Genesis-Anchored Validator Set

Prime record version 1 now supports `GenesisConfigV1`, which commits the
canonical three-address Ed25519 validator set into the height-zero record.
`SequentialNode` derives authorization from genesis and rejects composite
certificates whose embedded validator set differs, including during standalone
historical replay without quorum server configuration.

Empty quorum nodes create anchored genesis immediately. Peer bootstrap accepts
and reproduces that exact genesis record. Quorum startup rejects legacy
version-0 genesis and any configured replacement validator set. `GET_VALIDATORS`
reports the validator set derived from chain state.

Tests cover serialization and duplicate-address rejection, fresh peer bootstrap,
legacy-genesis refusal, and an attempted A/B/C to A/B/X validator replacement.
Peer sync validates the downloaded genesis in a temporary store and rejects a
mismatched validator anchor before replacing local chain data.
This anchors initial controlled-testnet membership; signed validator epochs and
permissionless validator selection remain open.

## 2026-06-07: Signed Validator Epoch Transitions

Prime and composite record version 2 can now embed a signed validator epoch
transition. Two or three members of the currently active Ed25519 validator set
must sign the next sequential epoch, the next-integer activation point, and the
canonical replacement three-validator set. The old set authorizes the containing
record; the replacement set becomes active for the following integer.

`SequentialNode` enforces transitions during append and historical replay and
reconstructs the active epoch after restart. The TCP server now separates the
immutable genesis validator anchor from the replay-derived active set, preserving
the original trust root across rotation and peer synchronization.

Tests accept a valid 2-of-3 rotation and reject insufficient quorum and altered
signatures. Low-level `SUBMIT_RECORD` ingestion supports version-2 records; a
user-facing proposal and vote command is the next operational step.

## 2026-06-08: Validator Epoch Operator Workflow

Added `GET_VALIDATOR_EPOCH`, `SUBMIT_EPOCH_VOTE`, and `GET_EPOCH_VOTES`, plus the
`primechain-composite-commitment sign-epoch` command. Current validators can now
create a 2-of-3 signed replacement proposal through TCP without constructing a
binary record manually.

Epoch votes are validated against the current replayed tip, stored atomically in
`<record-store>.epochs`, and propagated to configured peers. The next accepted
prime or composite record automatically embeds a ready transition as record
version 2, activates the replacement set for the following integer, and removes
the temporary vote state.

Startup now distinguishes the immutable genesis trust anchor from the active
validator epoch. This permits a newly rotated-in validator to start with the
original genesis set plus its new active identity. The TCP integration test covers
vote persistence across restart, quorum completion, prime-record finalization,
epoch activation, sidecar cleanup, and startup by the replacement validator.

## 2026-06-08: SHA3-256 Consensus Hash Migration

Replaced the development FNV-derived hash with OpenSSL SHA3-256 across canonical
record hashes, transaction hashes and roots, commit snapshots, composite
commitments, address derivation, record-store verification, synchronization, and
conflict ordering. Standard SHA3-256 vectors for the empty string and `abc` are
now part of the protocol tests.

This is an intentional pre-testnet format break: existing `.dat`, `.commitments`,
`.phases`, `.epochs`, and Ed25519 identity/address test artifacts created under
the former hash must be regenerated. The remaining cryptographic milestone is
replacing synthetic development finalization votes with validator signatures over
the complete candidate record hash.


## 2026-06-08: Authenticated Record Finalization

Replaced synthetic quorum-record finalization with canonical Ed25519 2-of-3
signatures over the complete candidate record hash. Validator peers receive the
full candidate plus the proposing active validator's signature, independently run
non-mutating consensus and ledger validation, and sign only if it extends their
current tip. Prime and composite records are
appended only after two valid active-epoch signatures are collected.

Added the domain-separated `primechain-record-finalization-v1` signing payload,
public keys in validator votes, replay verification, and the internal
`SIGN_RECORD_CANDIDATE` validator command. Pending signed choices are atomically
stored in `<record-store>.finalization`; this prevents restart-based equivocation
and is cleared when the finalized record is appended or synchronized.

At this milestone the protocol deliberately favored safety over liveness: a
validator would not sign a second candidate for the same integer. Timeout and
round-change support was the next milestone. Ed25519 remains the
controlled-testnet algorithm and can later be replaced behind the signature
interface by ML-DSA.

## 2026-06-08: Prime and transaction authentication

TCP prime submissions and transactions now use Ed25519 `pc1_` identities. A prime signature binds the previous finalized record hash, prime, Pratt witness, complete `p - 1` factorization, and reward address. This prevents certificate copying from redirecting the prime-discovery reward and prevents replay at a different frontier. TCP nodes reject unsigned `SUBMIT_PRIME`.

Transaction signatures bind the canonical unsigned transaction under a separate domain, including inputs, outputs, fee, nonce, sender address, and sender public key. Mempool admission, propagation, record validation, replay, and synchronized nodes verify the signature. Development `pcdev1_` signatures remain only for explicitly unanchored development fixtures. Ed25519 identity files are currently reused by miners and transaction senders; post-quantum key formats remain a later migration.

## 2026-06-08: Signed Finalization Round Changes

Added a controlled 2-of-3 round-change protocol for stalled record
finalization. Validators sign a domain-separated transition bound to the
current record hash, next integer, target round, and validator identity. A
candidate in round 2 or later must embed two or three canonical round-change
votes and collect its finalization signatures in that exact round.

Anti-equivocation state is now keyed by `(integer, round)`. Round-change votes
are atomically stored in `<record-store>.rounds`, survive restart, and are
cleared with `.finalization` state after the record is accepted or synchronized.
Historical replay verifies the embedded certificate and does not depend on
either sidecar.

`--finalization-timeout-ms` enables one automatic next-round retry after a
failed collection attempt; `0` retains fail-fast behavior. A network test first
locks one validator to a competing round-1 candidate, then proves that two
validators authorize round 2, finalize a different candidate, converge, clear
temporary state, and replay from the arithmetic record alone.

## 2026-06-08: Operational Transaction Fees and Nonces

Transaction replay now maintains a per-sender nonce, starting at `1` and
requiring exact contiguous increments across and within arithmetic records.
Inputs must equal outputs plus the declared fee for each prime asset, using
integer micro-units with checked aggregation. After a batch executes, its fees
are credited to that arithmetic record's authenticated proof provider without
changing total supply.

TCP mempool admission now checks signatures, current balances, ordered pending
transactions, fees, and nonces. Conflicting sender nonces are rejected, stale
transactions are pruned after append, tip replacement, or peer sync, and
`GET_NONCE address` reports both confirmed and next locally usable values.
`primechain-send submit` accepts an optional fee before the nonce while retaining
the prior zero-fee command form.

## 2026-06-08: ML-DSA-65 Protocol Migration

Added a generic signature interface and a portable `mldsa-native` ML-DSA-65
backend pinned as a git submodule. Authenticated transactions, prime proofs,
composite commitments and reveals, phase votes, validator epochs, record
finalization, and round changes now use NIST ML-DSA-65 with `pcpq1_` addresses
and domain-separated v2 payloads. Ed25519 remains available only through the
generic crypto API for compatibility tests; it is no longer the live protocol
algorithm.

The migration intentionally changes wallet, address, transaction, record, and
sidecar formats. Existing development wallets and chain databases must be
regenerated. Because ML-DSA public keys and signatures exceed the original line
transport limit, TCP commands and responses now use a bounded `FRAME <size>`
envelope above 4096 bytes, with a one-megabyte maximum. Record sync, mempool
exchange, quorum votes, validator epochs, and mining tools decode frames as one
logical protocol message.

## 2026-06-08: Crash-Recoverable Record Store And Index

Hardened the canonical arithmetic-record store without changing its `.dat`
wire format. Appends now use explicit POSIX writes and synchronize the chain
file before success. Startup and lookup recover a crash-interrupted append by
hash-verifying the complete prefix and truncating only an incomplete trailing
record. Invalid magic, kind, size, or payload hashes inside completed records
remain fatal corruption errors.

Added a persistent `.idx` sidecar mapping each integer to its record byte
offset. Latest, integer, and range queries use the index instead of loading and
hashing every payload. The index is non-consensus acceleration state: stale,
malformed, interrupted, or inconsistent files are automatically rebuilt from
the verified chain. Tip replacement and validated peer synchronization now
write and synchronize temporary stores before atomic rename, and the build
enables 64-bit file offsets on 32-bit Linux.

Fault tests cover incomplete append recovery, index corruption and rebuild,
atomic tip replacement, rejection of incomplete install sources, preservation
of the live store after failed installation, and detection of interior payload
corruption.

## 2026-06-08: Coordination Sidecar Crash Recovery

Unified durability behavior for `.commitments`, `.phases`, `.epochs`,
`.finalization`, and `.rounds`. Every replacement now synchronizes the complete
temporary file before atomic rename and synchronizes the containing directory
afterward.

Restart recovery follows one deterministic rule. A committed primary file is
authoritative and removes any stale temp. If the primary is absent, a temp that
fully passes the store's existing parser is promoted; an incomplete or malformed
temp is discarded. A corrupt primary remains a hard error and cannot be hidden
by a valid temp. Format-level tests apply this sequence independently to all
five sidecars, while multi-node tests continue to cover restart, propagation,
epoch activation, and finalization round recovery.

## 2026-06-09: Replay Snapshots And Pruning Boundary

Added an atomic `.snapshot` replay cache beside each canonical record store. It
contains the full ledger reconstruction state and active validator epoch at an
exact height, integer, and finalized record hash. Files carry a SHA3-256
checksum and use synchronized temporary-file replacement. A valid orphan temp
is recoverable after an interrupted rename.

Startup verifies the snapshot anchor against the indexed chain and checks state
invariants before restoring it. It then reads and validates only records after
the anchor. Missing, stale, malformed, checksum-invalid, or invariant-invalid
snapshots are discarded and rebuilt through full replay. Tests cover suffix
replay from a stale snapshot, corruption fallback, and interrupted-temp
recovery.

This is a local acceleration cache, not a consensus checkpoint. Historical
record pruning remains disabled because `state_root` is not yet enforced and
the arithmetic proof history is needed by miners. Safe pruning requires a
separate protocol milestone for deterministic state commitments, verifiable
checkpoints, and proof-history retention.

## 2026-06-30: Unified Client Wrapper

Added `primechain-client` as the first user-facing command for common operator
workflows. It supports `status`, arbitrary `query`, range `sync`, local
`inspect`, `new-miner`, `address`, `balance`, and signed frontier `mine`
subcommands. The implementation deliberately dispatches to the existing tested
tools instead of duplicating protocol logic, preserving the current consensus
and network behavior.

Integration tests cover miner identity creation, store inspection, remote status
and sync, and mining a loopback node to a target frontier through the client.
The remaining client work is the mathematical mining workbench: divisor search,
factorization helpers, Pratt attempts, resumable jobs, and later Bitcoin mapping
experiments.

## 2026-06-30: Client Math Workbench

Extended `primechain-client` with local mathematical commands: `is-prime`,
`divisor`, `factor`, and `pratt`. Factorization and Pratt construction load the
local record store, extract verified composite proofs, and reuse the existing
number-theory APIs rather than querying the node for expensive helper work.

Tests cover direct primality checks, divisor discovery, factorization from a
downloaded arithmetic history, Pratt construction for 97, and expected failure
when the local proof history is insufficient.

## 2026-06-30: Client Workdirs And Mine Jobs

Extended `primechain-client` with a persistent workdir workflow. `init-workdir`
creates the local directory structure, peer config, miner wallets, and chain
location. `sync-peer` updates the local chain from the configured peer,
`job-status` reports peer/local frontier/job target state, and `mine-job` runs
the authenticated frontier miner from stored wallets before syncing the local
chain copy.

Integration tests cover syncing a fresh workdir from a loopback peer and mining
a workdir-backed node to a target frontier while preserving simple job state.

## 2026-06-30: Client Reward Reporting

Added workdir `balances` and `rewards` commands to `primechain-client`.
`balances` loads the workdir chain and reports holdings for the stored prime and
composite miner wallets. `rewards` scans finalized records to attribute prime
mining rewards, composite reward shares, fee rewards, and pending composite
records to those wallets.

The workdir mining integration test now verifies the expected reward split after
mining through a prime/composite/prime sequence.

## 2026-07-03: Client Job Scheduler

Extended workdir mining from a one-shot `mine-job` command into persistent job
commands. `add-mine-job` records the target frontier, `run-jobs` syncs before
mining, skips work if the peer/local chain already reached the target, runs the
authenticated frontier miner when needed, syncs afterward, and records
pending/complete/failed state plus the last synced frontier. `clear-job` removes
the local job state. `mine-job` remains as an add-and-run convenience wrapper.

The workdir mining integration test now verifies the pending state, completed
state, last synced frontier, balances, and reward attribution after running the
stored job.

## 2026-07-03: Client Composite Proof Index

Added a rebuildable workdir proof index at `indexes/composite-proofs.idx`.
`update-indexes` scans the local `.dat` chain and writes verified composite
proofs plus chain metadata. `index-status` reports the cached frontier and proof
count. `factor-workdir` and `pratt-workdir` load the cache for factorization and
Pratt construction instead of rescanning the full chain.

The index is not consensus state. If it is missing or invalid, it can be rebuilt
from the canonical record store. Client tests now cover index creation, status,
cached factorization, and cached Pratt construction from a synced workdir.

## 2026-07-03: Client Reward History

Added workdir `reward-history` reporting to `primechain-client`. The command
scans the local finalized chain and emits per-record reward events attributable
to the workdir prime and composite wallets, including prime-miner rewards,
composite-provider shares, and record-provider fee rewards. `--last N` limits
the output to recent events for operator use.

The workdir mining integration test now verifies full reward history and recent
reward slicing after mining through a prime/composite/prime sequence.

## 2026-07-03: Frontier Miner Proof Store Bootstrap

Added optional `--proof-store <chain.dat>` support to `primechain-frontier-miner`.
The miner now loads verified composite proofs from an existing local chain before
entering the mining loop. This fixes the case where a workdir synced from a
public node at frontier 20 could mine composites up to 28 but fail to construct
the Pratt proof for 29 because earlier composite proofs were not present in the
miner's in-memory index.

`primechain-client run-jobs` now passes the workdir chain as the proof store
automatically. A regression test starts from a synced frontier-20 workdir and
mines through prime 29 to frontier 30.

## 2026-07-03: Client Record Decoder

Added `primechain-client decode-record <record-store> <integer>` for readable
local blockchain inspection. The decoder prints record kind, height, hashes,
prime or composite proof details, transaction batch summary, validator epoch
transition summary, and finalization summary without exposing the raw wire hex.

Client tests now cover decoding a prime record's Pratt factorization and a
composite record's divisor/cofactor fields.

## 2026-07-12: Quorum Client Mining Stability

Fixed the public-testnet client workflow that could mine a prime and one
composite, then stop on the next composite with a stalled finalization or
round-change error. The frontier miner now closes the commit phase across every
known validator and reveals through a validator that has observed quorum, rather
than stopping at the first node to report two phase votes.

Quorum-side propagation now distinguishes public submissions from peer-forwarded
commitments and phase votes, so validators store forwarded state without
synchronously rebroadcasting it back through the same request path. Validators
also retry candidate signing after a peer sync when their local chain is stale.
Automatic peer-of-peer discovery remains enabled for non-quorum nodes, while
quorum validator topology is explicit until node identity and advertised address
handling are added. A three-validator workdir regression now mines from genesis
to frontier 10 in one client run.

## 2026-07-12: Client Stale-Work Retry

The frontier miner now treats competing-client races as retryable stale work
instead of terminal job failure. If another miner advances the frontier, wins a
commit-reveal selection, or prunes the pending reveal sidecar before this miner
submits, the miner re-queries status and continues from the new frontier with a
bounded per-integer retry limit.

A two-workdir regression now runs two clients against one node to the same target
and requires both `run-jobs` processes to complete.

## 2026-07-12: Workdir Retry After Race Sync

Extended workdir mining retry behavior for live competing clients. The frontier
miner now treats `commit phase is closing or closed` as stale work and backs off
before retrying. `run-jobs` also syncs and restarts the frontier miner when a
miner failure was caused by another client advancing the chain and adding
composite proofs needed by the next Pratt certificate.

## 2026-07-12: Quorum Commit Warmup Before Phase Close

A live two-client testnet race exposed that validators could freeze integer 85 on
different commitment snapshots if a miner closed the phase immediately after
submitting its own commitment. The frontier miner now submits the same signed
commitment to all advertised validator peers and waits briefly before requesting
commit-phase votes, so validators are more likely to close over the same
snapshot instead of split-voting.

## 2026-07-12: Closed Commit Phase Winner Handling

A second live two-client test showed that after a commit phase had already
closed, the losing client kept asking validators to close the same integer and
received anti-equivocation rejections. The frontier miner now queries
`GET_COMMIT_PHASE` when the phase is already closed or quorum close fails: if
the local commitment won it reveals to the closed peer, and if another provider
won it clears its pending composite state and backs off instead of requesting
conflicting validator votes.

## 2026-07-12: Round-Change Duplicate Vote Semantics

Live two-client quorum mining exposed that ML-DSA round-change signatures for
the same validator and same round-change payload are not guaranteed to be
byte-identical. The sync server now treats a repeated valid round-change vote
from the same validator/public key as the same semantic vote, instead of
rejecting it as equivocation solely because the signature bytes differ.

## 2026-07-12: Mining View Command

Added `GET_MINING_VIEW [integer]` as a compact read-only coordination command
for miners and operators. It reports the local frontier, target integer, latest
record hash, commit-phase state, phase vote count, snapshot hash, current
winner, commitment count, active finalization round, validator count, and known
peer count. This is the first step toward clients coordinating across all
validators from one consistent mining view instead of inferring state from
several separate commands.
## 2026-07-12: Mining View Client Coordination

The frontier miner now consumes `GET_MINING_VIEW` from all discovered quorum
validators before submitting a composite commitment and again after commitment
warmup before requesting phase-close votes. A closed phase with a different
winner causes the miner to clear pending composite state and back off; a closing
phase causes it to retry after syncing instead of adding another competing
commitment. Added a two-workdir quorum race regression test so concurrent
clients must both reach the same target without wedging validator sidecars.

## 2026-07-12: Workdir Mining Waits for Race Winners

When a workdir miner loses a quorum commit phase, `run-jobs` now treats the
failure as a possible propagation race instead of immediately failing if the
first sync does not advance. It records `waiting-for-race-winner`, polls and
syncs briefly for the winning finalized record, then continues once the local
frontier advances. This lets the losing client in a two-workdir public-demo race
finish automatically after the winning client finalizes the integer.

## 2026-07-12: Commit-Phase Timeout Recovery

Three concurrent public-demo clients exposed a liveness gap before record
finalization: validators could close or partially close a commit phase for a
winning commitment, while the winning client failed to reveal and finalize the
record. Added a domain-separated ML-DSA commit-phase timeout vote and
`TIMEOUT_COMMIT_PHASE`. Any client can request the timeout, but validators only
clear temporary commitments and phase votes for the current frontier after two
validator signatures certify abandoning the stalled phase. `run-jobs` now calls
this timeout path after waiting for a race winner that never propagates. This is
a controlled-testnet liveness mechanism; embedding commit-timeout evidence into
final records remains a future auditability improvement.

## 2026-07-12: Workdir Retry After Commit-Phase Timeout

`run-jobs` now waits longer for a race winner to propagate before requesting a
commit-phase timeout, and treats a successful `TIMEOUT_COMMIT_PHASE` as progress
that reopens the same integer rather than waiting for the frontier to advance.
After a timeout clears a stalled phase, the workdir immediately restarts the
frontier miner and retries the same integer automatically.

## 2026-07-13: Commit-Phase State Machine

Documented the controlled-testnet commit-phase state machine as `OPEN ->
CLOSING -> CLOSED -> FINALIZED`, with certified timeout recovery back to
`OPEN`. The frontier miner now parses phase states through a named state enum
instead of scattered string checks, and `run-jobs` keeps cycling while competing
miners advance the frontier. A losing workdir no longer treats normal race
progress as a terminal failure; only repeated no-progress attempts fail the
job.

## 2026-07-13: Round-Numbered Commit-Phase Timeout

Commit-phase sidecars are now keyed by `(integer, commit_round, signer)`. A
certified timeout signs the exact transition from round `N` to round `N+1` and
clears only round `N` commitments and phase votes. This lets a validator that
remained stuck in an old `CLOSING` round consume the already-certified timeout
and reopen the same integer in the next round without manual sidecar deletion.
The finalized composite record still embeds the winning round's commitments and
phase votes using the existing record certificate format; abandoned timeout
certificates remain controlled-testnet liveness evidence rather than permanent
replay evidence.
