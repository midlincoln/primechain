# Primechain Protocol Formats v0

This document defines the first development protocol formats for the sequential arithmetic-record chain.

Status: draft for implementation.

This is not the final production cryptographic specification. The goal is to remove ambiguity from the prototype so code, tests, and future nodes agree on the same object model.

## 1. Design Scope

The v0 protocol records one finalized classification per integer:

```text
g is COMPOSITE
g is PRIME
```

Each finalized arithmetic record may also commit to a batch of transactions.

The v0 chain is intentionally small-number and development-focused:

- integers use unsigned 64-bit values,
- all consensus-critical hashing uses SHA3-256,
- composite contributors and controlled-testnet validators use NIST ML-DSA-65 signatures,
- TCP transactions and prime-provider rewards use ML-DSA-65 `pcpq1_` identities,
- Pratt certificates are the target prime-proof format for test-sized integers,
- validator voting is authenticated but remains a controlled 2-of-3 mechanism, not permissionless consensus.

## 2. Primitive Types

### UInt64

Unsigned 64-bit integer.

Canonical binary encoding:

```text
8 bytes, little-endian
```

### Bytes

Variable-length byte string.

Canonical binary encoding:

```text
uint64 length
raw bytes
```

### String

UTF-8 string.

Canonical binary encoding:

```text
uint64 byte_length
UTF-8 bytes
```

### Hash256

32-byte hash.

Consensus hash:

```text
SHA3-256(canonical_bytes)
```

### Address

Development address format:

```text
pcdev1_<label>
```

Rules:

- ASCII only,
- maximum 64 characters,
- must begin with `pcdev1_`,
- labels are not cryptographic identities.

Authenticated protocol address format:

```text
pcpq1_<first_20_bytes_of_sha3_256_public_key>
```

ML-DSA-65 public keys are 1952 bytes, private keys are 4032 bytes, and
signatures are 3309 bytes.

## 3. Transaction Format

Transactions move fractional ownership of prime-indexed assets.

v0 transaction:

```text
Transaction {
    version: UInt64,
    inputs: [TxInput],
    outputs: [TxOutput],
    fee: FeeSpec,
    nonce: UInt64,
    sender_address: Address,
    sender_public_key: Bytes,
    signature: Bytes
}
```

Input:

```text
TxInput {
    prime: UInt64,
    amount_num: UInt64,
    amount_den: UInt64
}
```

Output:

```text
TxOutput {
    prime: UInt64,
    amount_num: UInt64,
    amount_den: UInt64,
    receiver_address: Address
}
```

Fee:

```text
FeeSpec {
    prime: UInt64,
    amount_num: UInt64,
    amount_den: UInt64
}
```

Validation rules:

- input and output amounts use positive integer micro-units: `amount_den = 1`
  and `amount_num > 0`.
- a fee uses integer micro-units: `amount_den = 1`; zero remains valid for
  compatibility, while a non-zero fee must name a valid prime asset.
- all referenced `prime` values must already have finalized prime records.
- sender must have sufficient unspent ownership for every input and fee.
- sum of outputs plus fee must equal sum of inputs for each prime.
- each sender starts at nonce `1`; every later transaction must use exactly the
  previous confirmed or earlier-in-batch nonce plus one.
- after the complete batch is applied, its fees are credited by prime asset to
  the authenticated proof provider of the arithmetic record. A transaction in
  the same batch cannot spend those newly collected fees.
- amount aggregation must reject unsigned 64-bit overflow.
- no floating-point arithmetic is allowed.

Authenticated TCP transactions use version `2` and ML-DSA-65. The sender address must equal `pcpq1_` plus the first 40 hexadecimal characters of `SHA3-256(sender_public_key)`. The signature payload is the canonical transaction serialized with `signature` encoded as an empty byte string, wrapped in the domain `primechain-transaction-signature-mldsa65-v2`. Any change to inputs, outputs, fee, nonce, sender address, or public key invalidates the signature. Legacy version-`0` `pcdev1_` transaction signatures are accepted only by unanchored offline development-chain tooling.

## 4. Transaction Batch

Each arithmetic record commits to zero or more transactions.

```text
TransactionBatch {
    transaction_count: UInt64,
    transaction_merkle_root: Hash256
}
```

Merkle root rule:

- If `transaction_count == 0`, use `Hash256{0}`.
- Otherwise, hash each canonical transaction as a leaf.
- Pair leaves left-to-right.
- If a level has an odd number of leaves, duplicate the final leaf.
- The final hash is `transaction_merkle_root`.

The full transaction list may be transmitted with the record or fetched separately. Consensus checks the root.

## 5. Composite Proof Format

A composite proof certifies that `g` is not prime.

```text
CompositeProof {
    g: UInt64,
    d: UInt64,
    e: UInt64,
    provider_address: Address,
    signature: Bytes
}
```

Validation rules:

- `g >= 4`.
- `1 < d < g`.
- `1 < e < g`.
- `d * e == g`, checked without overflow.
- `provider_address` is syntactically valid.

The proof does not need to contain a complete factorization. Complete factorization can be reconstructed recursively from earlier finalized records for `d` and `e`.

## 6. Pratt Prime Proof Format

A Pratt proof certifies that `p` is prime.

### Canonical Factorization Format

All full factorizations are represented as a canonical ordered list of prime powers:

```text
Factorization {
    factors: [PrimePower]
}
```

Canonical rules:

- factors are sorted by strictly increasing `prime`,
- every `prime >= 2`,
- every `prime` must itself be prime,
- every `exponent >= 1`,
- repeated prime entries are invalid,
- the empty factor list is valid only when a surrounding rule explicitly allows product `1`.

Prime-power factor:

```text
PrimePower {
    prime: UInt64,
    exponent: UInt64
}
```

Canonical binary encoding:

```text
uint64 factor_count
for each factor in increasing-prime order:
    uint64 prime
    uint64 exponent
```

The product represented by the factorization is:

```text
product(prime^exponent)
```

Validators must reject a factorization if multiplying the prime powers overflows the integer domain or does not equal the target integer required by the surrounding proof.

### Pratt Proof

For v0, a Pratt proof is represented as:

```text
PrattPrimeProof {
    p: UInt64,
    witness: UInt64,
    factors_of_p_minus_1: Factorization,
    provider_address: Address,
    signature: Bytes
}
```

Validation rules:

- `p >= 2`.
- `provider_address` is a key-derived ML-DSA-65 `pcpq1_` address for every non-genesis network submission.
- `signature` packs the 1952-byte public key followed by the 3309-byte ML-DSA-65 signature.
- the signature payload uses domain `primechain-prime-proof-signature-mldsa65-v2` and binds `previous_record_hash`, `p`, `witness`, every `(prime, exponent)` factor pair, and `provider_address`.
- `factors_of_p_minus_1` multiply exactly to `p - 1`.
- every `prime` in `factors_of_p_minus_1` has an earlier finalized prime record.
- `witness > 1` and `witness < p`.
- `witness^(p - 1) mod p == 1`.
- for every distinct prime factor `q` of `p - 1`:

```text
gcd(witness^((p - 1) / q) - 1, p) == 1
```

For `p = 2`, the certificate is the genesis prime rule and may use an empty factor list and witness `0`.

Pratt proof generation can use the stored arithmetic chain to reconstruct the factorization of `p - 1`.

v0 implementation note:

- `verifyPrattProof` verifies the submitted canonical factorization, the Fermat/order condition, and the gcd condition.
- The current small-number implementation confirms factor bases using local `isPrime`.
- A later consensus implementation must replace that shortcut with checks that every factor base has an earlier finalized `PrimeRecord`.

## 7. Arithmetic Record Formats

Every finalized integer classification uses one of two payloads.

### CompositeRecord

```text
CompositeRecord {
    version: UInt64,
    height: UInt64,
    previous_record_hash: Hash256,
    integer: UInt64,
    proof: CompositeProof,
    tx_batch: TransactionBatch,
    state_root: Hash256,
    finalized_by: FinalizationProof
}
```

### PrimeRecord

```text
PrimeRecord {
    version: UInt64,
    height: UInt64,
    previous_record_hash: Hash256,
    integer: UInt64,
    proof: PrattPrimeProof,
    tx_batch: TransactionBatch,
    state_root: Hash256,
    finalized_by: FinalizationProof
}
```

Validation rules common to both:

- `height == previous_height + 1`.
- `integer == previous_integer + 1`, except genesis.
- `previous_record_hash` matches the canonical hash of the previous finalized record.
- `tx_batch.transaction_merkle_root` matches included transactions.
- `state_root` is reserved in the current prototype record format. Consensus
  does not yet compute or enforce it; implementations must not use it to justify
  pruning historical records.

Genesis:

```text
height = 0
integer = 2
record type = PRIME
previous_record_hash = Hash256{0}
```

## 8. Finalization Proof

Controlled-testnet finalization uses a genesis-anchored validator set and 2-of-3 voting.

```text
FinalizationProof {
    rule: String,
    round_changes: [RoundChangeVote],
    votes: [ValidatorVote]
}
```

Rules:

```text
fixed-2-of-3-dev
fixed-2-of-3-mldsa65-v2
fixed-2-of-3-mldsa65-rounds-v3
```

`fixed-2-of-3-dev` is restricted to unanchored single-node development records
and deterministic genesis construction. Validator-anchored round-1 records use
`fixed-2-of-3-mldsa65-v2`; later rounds use the certificate-bearing v3 rule.

```text
ValidatorVote {
    validator_address: Address,
    public_key: Bytes,
    record_hash: Hash256,
    round: UInt64,
    signature: Bytes
}
```

The signed payload is:

```text
Encode(
    "primechain-record-finalization-mldsa65-v2",
    candidate_record_hash,
    round,
    validator_address
)
```

Validation rules:

- the active validator set contains exactly three canonical `pcpq1_` addresses,
- two or three distinct active validators sign the identical candidate hash,
- the public key derives the claimed validator address,
- votes are sorted by validator address,
- round 1 uses `fixed-2-of-3-mldsa65-v2` and has no round-change votes,
- later rounds use `fixed-2-of-3-mldsa65-rounds-v3`,
- validators persist one signed candidate per `(frontier integer, round)` to prevent equivocation across restart.

The proposing validator signs first and sends the complete candidate record, an empty vote list, and its authorization vote to validator peers. A peer rejects requests not authorized by an active validator, then independently validates arithmetic, certificates, transactions, rewards, epoch changes, tip linkage, and any embedded round-change certificate before signing. The finalized record includes the collected votes and has a different finalized record hash.

### 8.1 Finalization Round Change

If round 1 stalls, validators may authorize a later round without permitting
equivocation inside the earlier round:

```text
RoundChangeVote {
    validator_address: Address,
    public_key: Bytes,
    previous_record_hash: Hash256,
    integer: UInt64,
    new_round: UInt64,
    signature: Bytes
}
```

The signed payload is:

```text
Encode(
    "primechain-finalization-round-change-mldsa65-v2",
    previous_record_hash,
    integer,
    new_round,
    validator_address
)
```

Two or three distinct active validators must sign the same frontier hash,
integer, and target round. The next candidate embeds those canonical votes and
is signed in that round. The round-change certificate is part of the candidate
hash and permanent record history. Temporary votes are atomically stored in
`<record-store>.rounds`; per-round signed choices remain in
`<record-store>.finalization`. Both sidecars are cleared after finalization and
are not needed for historical replay.

`--finalization-timeout-ms N` enables automatic recovery. After a failed vote
collection attempt, the proposer waits `N` milliseconds, obtains a 2-of-3
certificate for the next round, and retries the candidate once. A later
submission may repeat the process for another round. A value of `0`, the
default, preserves fail-fast behavior.

This remains controlled-testnet consensus. The timeout is only a local liveness
trigger; safety comes from the embedded validator signatures, not synchronized
clocks.

## 9. Record Hashing

Record hash:

```text
record_hash = Hash256(canonical_record_bytes_without_transport_wrapper)
```

All fields are serialized in the order shown in this document.

The `finalized_by` field is included in the record hash unless a later version explicitly separates candidate-record hash from finalized-record hash.

For validator voting, validators vote on the candidate record hash with `finalized_by` encoded as an empty vote list. The finalized record hash then commits to the actual vote set.

## 9.1 ML-DSA-65 Composite Contributor Authentication

Authenticated composite mining uses key-derived addresses:

```text
address = pcpq1_ || first_20_bytes(Hash(public_key))
```

The current implementation uses ML-DSA-65 keys and domain-separated canonical
payloads:

```text
CommitSignaturePayload = Encode(
    "primechain-composite-commit-signature-mldsa65-v2",
    integer,
    commitment_hash,
    provider_address
)

RevealSignaturePayload = Encode(
    "primechain-composite-reveal-signature-mldsa65-v2",
    integer,
    d,
    e,
    nonce,
    provider_address
)
```

A finalized `pcpq1_` composite proof stores the 1952-byte public key, 8-byte nonce,
and 3309-byte ML-DSA-65 reveal signature in its proof signature field. Replay must
verify the address derivation and reveal signature before assigning contributor
credit.

## 10. Commit-Reveal Candidate Messages

Commit-reveal is not required for local benchmark mode, but the wire format target is:

```text
ProofCommit {
    version: UInt64,
    integer: UInt64,
    proof_kind: String,
    commitment_hash: Hash256,
    provider_address: Address,
    signature: Bytes
}
```

For composite proofs:

```text
commitment_hash = Hash256(integer || d || e || provider_address || nonce)
```

Reveal:

```text
ProofReveal {
    version: UInt64,
    integer: UInt64,
    proof_kind: String,
    proof_bytes: Bytes,
    nonce: Bytes,
    provider_address: Address,
    signature: Bytes
}
```

Validation rules:

- reveal must match a prior commit,
- proof must independently verify,
- provider address must match,
- candidate must target the current frontier integer,
- among the commitments currently known for an integer, select the
  lexicographically smallest `(commitment_hash, provider_address)` pair,
- only the selected provider may finalize the composite record.

This candidate rule is independent of local arrival order, but it is provisional.
Nodes persist unresolved frontier commitments in a separate canonical binary
snapshot, exchange them with `GET_COMMITMENTS`, and remove them after the
integer is finalized. Persistence does not itself create consensus ordering.
The rule still permits nonce grinding and cannot ensure that all nodes have the
same candidate set before finalization. Production ordering and reward
attribution require signed commitments plus a quorum-defined commit boundary.

### 10.1 Controlled Commit-Phase Quorum

A node started with a fixed three-address validator set uses these phase states:

```text
OPEN -> CLOSING -> CLOSED
```

- `OPEN`: signed composite commitments are accepted.
- `CLOSING`: the first valid validator vote freezes the canonical snapshot.
- `CLOSED`: two distinct configured validators signed the same snapshot.

The snapshot hash commits to the integer and the canonical ordered list of full
commitment records, including miner authentication data. Validator signatures
use the domain `primechain-commit-phase-vote-mldsa65-v2` and bind the integer, snapshot
hash, and validator address.

```text
SUBMIT_PHASE_VOTE g snapshot_hash validator_address public_key signature
```

In quorum mode, only signed commitments are accepted. Reveals are rejected until
`CLOSED`; late commitments and direct `SUBMIT_COMPOSITE` calls are rejected. A
peer-submitted current composite record must contain a valid signed reveal that
reconstructs the selected commitment hash.

Votes are stored in `<record-store>.phases` and exchanged using
`GET_PHASE_VOTES` while an integer is unresolved. A finalized quorum-mode
composite uses record version 1 and embeds:

- the integer and snapshot hash,
- the canonical three-address validator set,
- the complete canonical signed commitment list,
- two or three signed validator votes.

The candidate record hash commits to this certificate. Historical replay checks
all miner and validator signatures, recomputes the snapshot, selects the lowest
`(commitment_hash, provider_address)` entry, and binds the reveal to that winner.
The `.commitments` and `.phases` files are therefore temporary coordination
state, not required blockchain history.

### 10.2 Genesis Validator Anchor

A controlled quorum chain uses prime record version 1 at height 0. Its
`GenesisConfigV1` contains exactly three distinct, lexicographically sorted
ML-DSA-65 `pcpq1_` validator addresses. The genesis record hash commits to this set.

During append, peer synchronization, and replay, every version-1 composite
certificate must contain exactly the validator set authorized by genesis. A
quorum server also requires its configured local set to match the replayed
genesis set before it begins serving requests. Version-0 genesis remains valid
for legacy non-quorum test chains, but quorum mode refuses it.

```text
GET_VALIDATORS
VALIDATORS 3 validator_a validator_b validator_c
```

This anchors the initial controlled-testnet membership.

### 10.3 Signed Validator Epoch Transitions

A non-genesis prime or composite record may use version 2 and embed one
`ValidatorEpochTransitionV1`:

```text
epoch
activation_integer
next_validator_set[3]
votes[2..3] { validator_address, public_key, signature }
```

The active validator set signs a domain-separated payload containing the prior
record hash, containing-record integer, next sequential epoch number, activation
integer, canonical next set, and voter address. The transition is valid only when:

- the epoch is exactly the current epoch plus one;
- activation is exactly the containing record integer plus one;
- the next set contains three distinct sorted ML-DSA-65 `pcpq1_` addresses;
- two or three distinct members of the current set provide valid signatures.

The containing record is authorized by the old set. The new set becomes active
only after that record is accepted, so it authorizes the following integer. Replay
derives the same active set and epoch from genesis plus all accepted transitions.
The genesis set remains the immutable peer-sync trust anchor.

The controlled-testnet coordination commands are:

```text
GET_VALIDATOR_EPOCH
VALIDATOR_EPOCH current_epoch next_integer current_tip_hash
SUBMIT_EPOCH_VOTE previous_hash record_integer epoch activation_integer next_a next_b next_c voter public_key signature
GET_EPOCH_VOTES
```

Votes are bound to the current tip and next integer, stored atomically in the
`.epochs` sidecar, and propagated to configured peers. Two matching current-set
votes make the proposal ready. The next accepted prime or composite record embeds
the transition and clears temporary votes. `SUBMIT_RECORD` also accepts a fully
constructed valid version-2 record. Permissionless validator selection remains
future work.

## 11. Bitcoin Mirror Payload

A future optional Bitcoin mirror may attach external-reference payloads to arithmetic records.

```text
BitcoinMirrorPayload {
    bitcoin_height: UInt64,
    bitcoin_block_hash: Hash256,
    bitcoin_previous_block_hash: Hash256,
    bitcoin_tx_merkle_root: Hash256,
    tx_assignment_root: Hash256
}
```

Mirror rule:

```text
one Bitcoin block
    -> one Primechain prime-to-prime interval
```

Transactions from the Bitcoin block are deterministically batched across arithmetic records in that interval. The final prime record commits to the Bitcoin block payload.

This payload must not transfer native Primechain assets unless explicitly represented as Primechain transactions.

## 12. Open Items

The following are intentionally unresolved:

- production address encoding,
- production signature scheme,
- exact binary serialization helper functions in C++,
- real SHA3-256 integration,
- transaction UTXO representation,
- reward allocation format,
- state-root construction,
- validator membership configuration,
- permissionless consensus beyond the controlled 2-of-3 testnet,
- DoS limits and network ban policy.

The next implementation target is to turn `CompositeRecord`, `PrimeRecord`, `PrattPrimeProof`, and `TransactionBatch` into C++ types and add canonical serialization tests.
