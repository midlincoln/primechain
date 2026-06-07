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
- hashing uses the current development hash until SHA3-256 is introduced,
- signatures are placeholder fields until a real signature layer is added,
- Pratt certificates are the target prime-proof format for test-sized integers,
- validator voting is specified structurally but not yet a production consensus mechanism.

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

v0 development hash:

```text
devHash256(canonical_bytes)
```

Target production hash:

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

Target production address format:

```text
pc1_<encoded_hash_of_public_key>
```

Production address encoding is deferred until the signature scheme is selected.

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

- `amount_den > 0`.
- `amount_num > 0` for every non-empty transfer amount.
- all referenced `prime` values must already have finalized prime records.
- sender must have sufficient unspent ownership for every input and fee.
- sum of outputs plus fee must equal sum of inputs for each prime.
- no floating-point arithmetic is allowed.

Signature rules are placeholder in v0. The canonical signature preimage is the transaction serialized with `signature` encoded as an empty byte string.

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
- `provider_address` is syntactically valid.
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
- `state_root` matches the state transition after applying transactions and rewards.

Genesis:

```text
height = 0
integer = 2
record type = PRIME
previous_record_hash = Hash256{0}
```

## 8. Finalization Proof

v0 controlled testnet finalization uses a fixed validator set and 2-of-3 voting.

```text
FinalizationProof {
    rule: String,
    votes: [ValidatorVote]
}
```

v0 rule string:

```text
fixed-2-of-3-dev
```

Validator vote:

```text
ValidatorVote {
    validator_address: Address,
    record_hash: Hash256,
    round: UInt64,
    signature: Bytes
}
```

Validation rules:

- validator addresses must belong to the configured validator set,
- at least two distinct validators must vote for the same `record_hash`,
- signatures are placeholder fields in v0,
- later versions must replace placeholder signatures with real cryptographic validation.

This finalization model is for controlled synchronization tests only. It is not permissionless consensus.

## 9. Record Hashing

Record hash:

```text
record_hash = Hash256(canonical_record_bytes_without_transport_wrapper)
```

All fields are serialized in the order shown in this document.

The `finalized_by` field is included in the record hash unless a later version explicitly separates candidate-record hash from finalized-record hash.

For validator voting, validators vote on the candidate record hash with `finalized_by` encoded as an empty vote list. The finalized record hash then commits to the actual vote set.

## 9.1 Ed25519 Composite Contributor Authentication

Authenticated composite mining uses key-derived addresses:

```text
address = pc1_ || first_20_bytes(Hash(public_key))
```

The current implementation uses Ed25519 keys and domain-separated canonical
payloads:

```text
CommitSignaturePayload = Encode(
    "primechain-composite-commit-signature-v1",
    integer,
    commitment_hash,
    provider_address
)

RevealSignaturePayload = Encode(
    "primechain-composite-reveal-signature-v1",
    integer,
    d,
    e,
    nonce,
    provider_address
)
```

A finalized `pc1_` composite proof stores the 32-byte public key, 8-byte nonce,
and 64-byte Ed25519 reveal signature in its proof signature field. Replay must
verify the address derivation and reveal signature before assigning contributor
credit. Ed25519 is an interim classical scheme, not the planned post-quantum
signature layer.

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
