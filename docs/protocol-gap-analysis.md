# Primechain Protocol Gap Analysis

Status: living document, first pass. Analysis and documentation only -- **nothing in the source tree was changed while writing this**, and no finding below has been acted on. Scope: `docs/protocol-formats-v0.md` (both repos are `kaito-zero/primechain`'s `docs/protocol-gap-analysis` branch, based on `upstream/main` @ `1134489`) checked against the actual C++ implementation.

Purpose, per [[primechain-engineering-roadmap]] Phase B: determine whether an independent engineer could reproduce current Primechain behavior from `protocol-formats-v0.md` alone, without reading the C++. This is explicitly **not** a redesign exercise, and finding a mismatch here is not itself a decision to change anything -- see the classification scheme below.

## How entries are classified

For each normative rule: **document section**, **implementing file/function**, **existing tests** (if identified), **match status**, **conformance vector status**, **consensus-visible?**, and if there's a mismatch, one of:

- **DOCUMENTATION BUG** -- the doc is simply wrong/outdated about what the code does.
- **IMPLEMENTATION GAP** -- code doesn't yet do what the doc says is the target (the doc itself often already says so, e.g. the Pratt `isPrime` shortcut).
- **UNDOCUMENTED MECHANISM** -- real, active, consensus-visible code with no corresponding entry in `protocol-formats-v0.md` at all (it may still be documented elsewhere, e.g. `validator-economics.md`, at a policy level rather than wire-format precision).
- **UNCLEAR / REQUIRES MAINTAINER DECISION** -- can't tell from code + docs + tests alone which side is "right."

No finding here should be read as "this is broken" -- several of these are pre-existing, self-acknowledged, intentional prototype simplifications.

## Headline finding 1: the transaction Merkle root is not a Merkle tree

`protocol-formats-v0.md` §4 documents a real binary Merkle tree:

> - Otherwise, hash each canonical transaction as a leaf.
> - Pair leaves left-to-right.
> - If a level has an odd number of leaves, duplicate the final leaf.
> - The final hash is `transaction_merkle_root`.

The actual implementation, `transactionMerkleRoot()` in `src/protocol/records.cpp`:

```cpp
Hash256 transactionMerkleRoot(const std::vector<TransactionV0>& transactions) {
    if (transactions.empty()) { return {}; }
    std::vector<std::uint8_t> payload;
    appendString(payload, "primechain-dev-tx-root-v0");
    appendUint64(payload, transactions.size());
    for (const auto& tx : transactions) {
        appendHash(payload, transactionHash(tx));
    }
    return crypto::sha3_256(payload);
}
```

This is a **flat sequential hash** (domain tag + count + every tx hash concatenated, hashed once) -- there is no pairing, no tree levels, no odd-leaf duplication. The function's own domain-separator string, `"primechain-dev-tx-root-v0"`, contains `"-dev-"`, suggesting this was understood internally as a placeholder even at the time it was written.

- **Document section:** §4 Transaction Batch.
- **Implementation:** `transactionMerkleRoot()`, `src/protocol/records.cpp:1061`.
- **Tests:** not specifically searched for a test asserting the *tree* structure (as opposed to just "the root changes when transactions change," which a flat hash also satisfies) -- worth checking for in a follow-up pass.
- **Status:** DOCUMENTATION / IMPLEMENTATION GAP.
- **Conformance vector:** missing.
- **Consensus-visible:** **yes** -- the spec says "Consensus checks the root," and `transaction_merkle_root` is part of the hashed record.
- **Action:** none taken. This is exactly the kind of finding that must go to the maintainer rather than being silently "fixed" either direction (rewriting the doc to match code vs. rewriting code to match a real Merkle tree are both live options with different consequences) -- **REQUIRES MAINTAINER DECISION**.

## Headline finding 2: five real, consensus-visible record fields have no entry in protocol-formats-v0.md

`CompositeRecordV0`/`PrimeRecordV0` (`include/primechain/protocol/records.hpp`) carry substantially more than §7's documented field list (`version, height, previous_record_hash, integer, proof, tx_batch, state_root, finalized_by`). The extra fields are all real -- each has a corresponding `verify*` function actually implemented in `records.cpp` (confirmed present, not just declared), and each is referenced by this session's own live test/log output:

| Field / type | Verifying function | Mentioned elsewhere in `docs/` | In `protocol-formats-v0.md`? |
|---|---|---|---|
| `composite_lottery` (`CompositeLotteryProofV1`: round, win_bps, subject_hash, assigned_validator) | `verifyCompositeLotteryProof` | Yes -- `development-log.md`, `launch-validator-runbook.md`, `mainnet-validator-onboarding.md` (operational mentions only) | **No** -- zero mentions in any of the 12 numbered sections |
| `validator_applications` (`ValidatorApplicationV1`: candidate_address, host, port, observed_successful/observed_total) | `verifyValidatorApplications` | Yes -- `validator-economics.md` (policy level: admission rules) | **No** |
| `validator_endpoints` (`ValidatorEndpointUpdateV1`: host, port, effective_integer, sequence) | `verifyValidatorEndpointUpdates` | Yes -- `validator-economics.md`, `validator-gossip-architecture-v0.md` | **No** |
| `economic_policy` (`EconomicPolicyUpdateV1`: transfer_fee_micro_units, validator_min_reserve_micro_units) | `verifyEconomicPolicyUpdate` | Yes -- `validator-economics.md`, `validator-economy-v0.md` | **No** |
| `validator_work_bindings` (`ValidatorWorkBindingV1`) | `verifyValidatorWorkBindings` | Not found in a targeted grep this pass | **No** |

- **Status:** UNDOCUMENTED MECHANISM (all five). Not a code bug -- these are real, working, tested features (composite lottery in particular is directly responsible for the "provider is in winner cooldown" / `LOTTERY_LOST` behavior seen constantly in this session's live logs). The gap is specifically that `protocol-formats-v0.md`, which states its own purpose as giving an independent implementer everything needed without reading the C++, currently cannot do that for any of these five.
- **Consensus-visible:** yes, all five (they're embedded fields on the hashed record).
- **Conformance vectors:** missing for all five.
- **Action:** none taken. Whether to extend `protocol-formats-v0.md` to cover these (documentation-only, low risk) vs. treat this as lower priority than the still-open items in its own §12 is a maintainer call, not an engineering one -- **REQUIRES MAINTAINER DECISION** on sequencing, though documenting existing behavior is itself explicitly a safe/no-approval-needed contribution area per the roadmap.

## Section-by-section status (first pass -- see confidence notes per row)

| §formats-v0 | Topic | Match status | Implementation | Confidence this pass |
|---|---|---|---|---|
| §2 | Primitive types (UInt64 LE, length-prefixed Bytes/String, Hash256=SHA3-256, dev/authenticated Address formats) | Struct/type shapes confirmed present (`types.hpp`, `records.hpp`); byte-level encoding of each primitive not individually re-verified this pass | `types.hpp`, `records.cpp` serialization helpers | Structural only |
| §3 | Transaction format + validation rules (positive integer amounts, referenced primes must be finalized, sufficient balance, sum-in==sum-out+fee, nonce starts at 1 and increments by exactly one, post-batch fee crediting, overflow rejection, no floats) | `TransactionV0` struct matches documented fields exactly. Nonce enforcement confirmed present (`sync_server.cpp`, mempool conflict + apply-time checks) but the exact "starts at 1, +1 only" rule not traced to one specific line this pass. Overflow/balance/fee rules not individually traced this pass. | `records.cpp` (serialize/hash/sign), `sync_server.cpp` (validation, mempool) | Partial -- struct shape confirmed, most validation rules not yet individually verified |
| §4 | Transaction batch / Merkle root | See Headline finding 1 above | `records.cpp:1061` | Fully verified -- confirmed mismatch |
| §5 | Composite proof format (`g>=4`, `1<d<g`, `1<e<g`, `d*e==g` no overflow, address syntax) | `CompositeProofV0` struct matches. Validation rule locations not individually traced this pass. | `records.cpp`, `math/number_theory.cpp` | Structural only |
| §6 | Pratt prime proof (canonical factorization, gcd/Fermat conditions, factor-base-must-be-earlier-finalized-PrimeRecord) | `PrattPrimeProofV0`/`PrimePowerV0` structs match. `gcd()` helper confirmed present and used in a primality-adjacent check (`number_theory.cpp:215`). The doc **already documents its own gap here**: factor-base verification currently uses a local `isPrime()` shortcut rather than checking for an earlier finalized `PrimeRecord` -- self-acknowledged IMPLEMENTATION GAP, not a new finding. | `math/number_theory.cpp`, `records.cpp` | Partial -- structs and the self-declared gap confirmed; full Fermat/gcd condition-by-condition trace not done this pass |
| §7 | Record formats (common validation: height==prev+1, integer==prev+1, prev-hash linkage, tx_batch root match, state_root reserved-not-enforced) + genesis | See Headline finding 2 for the field-list gap. `state_root`'s "reserved, not yet enforced" claim not independently re-verified this pass (plausible given README's "state-root construction" is listed as an Open Item in §12). | `sequential_node.cpp`, `sync_server.cpp` | Partial |
| §8 / §8.1 | Finalization proof (2-of-3 voting, round rules, round-change certificates, `.rounds`/`.finalization` sidecars) | `FinalizationProofV0`, `RoundChangeVoteV1`, `ValidatorVoteV0` structs match documented fields (including `RoundChangeVoteV1`'s cross-round-lock fields: `locked_round`, `locked_candidate_kind`, `locked_candidate_hash`, `locked_candidate_payload` -- these ARE present in the doc's §8.1 prose, structurally consistent). `verifyRoundChangeCertificate`/`verifyRecordFinalization` confirmed implemented. Deep semantic trace of quorum/round-change logic inside `sync_server.cpp` (8221 lines) **not done this pass** -- this is the single largest remaining unverified area and the most consensus-critical. | `sync_server.cpp` (majority of logic), `records.cpp` (structs + verify functions) | Structural only -- flagged as highest-priority follow-up |
| §9 | Record hashing (candidate hash vs. finalized hash separation) | `candidateRecordHash`/`finalizedRecordHash`/`legacyCandidateRecordHashWithoutFinalization` all confirmed present as distinct functions in `records.hpp`, consistent with the doc's candidate-vs-finalized hash distinction. Byte-level field-ordering-matches-document claim not verified this pass. | `records.cpp` | Structural only |
| §9.1 | ML-DSA-65 composite contributor auth (address = pcpq1 + first 20 bytes of hash(pubkey), domain-separated commit/reveal payloads) | Address-derivation helper (`developmentAddressFromPublicKey`) exists but is named for the *development* address scheme (§2's `pcdev1_`), not obviously the authenticated `pcpq1_` one -- the authenticated derivation likely lives elsewhere (not traced this pass). Commit/reveal domain strings referenced in `sync_server.cpp` (`SUBMIT_COMPOSITE_REVEAL` handling) but not diffed against the doc's exact domain strings this pass. | `sync_server.cpp`, `wallet/miner_identity.cpp` (likely) | Structural only, address-derivation function location genuinely unclear this pass -- flag as **UNCLEAR**, worth a dedicated look |
| §10 / §10.1 / §10.2 / §10.3 | Commit-reveal, controlled commit-phase quorum state machine, genesis validator anchor, signed epoch transitions | All corresponding structs (`CommitCertificateEntryV1`, `CommitPhaseCertificateV1`, `GenesisConfigV1`, `ValidatorEpochTransitionV1`) and verify functions (`verifyCommitPhaseCertificate`, `verifyGenesisConfig`, `verifyValidatorEpochTransition`) confirmed present. The commit-phase OPEN→CLOSING→CLOSED→FINALIZED/TIMED_OUT state machine's actual transition logic lives in `sync_server.cpp` and was **not traced this pass**. | `sync_server.cpp`, `records.cpp` | Structural only |
| §11 | Bitcoin mirror payload | Self-declared "future optional" by the document itself. No `BitcoinMirror`-named struct found in `records.hpp`. | none found | Consistent -- doc correctly says this isn't built yet. **PLANNED**, not a gap. |
| §12 | Open items | Self-declared "intentionally unresolved" by the document itself (production address encoding, production signature scheme, real SHA3-256 integration, UTXO representation, reward allocation format, state-root construction, permissionless consensus, DoS limits). Worth noting: "real SHA3-256 integration" as an open item is confusing given `crypto::sha3_256` is called directly throughout `records.cpp` -- possibly stale wording (may mean "a from-scratch/audited implementation" vs. a library) rather than "SHA3-256 isn't used yet." Not resolved this pass -- **UNCLEAR, worth asking**. | n/a | Consistent with self-declaration; one wording item flagged |

## What this pass did NOT cover (explicitly, so the next pass knows where to start)

- No line-by-line trace of `sync_server.cpp`'s finalization/round-change/commit-phase state machine (§8-§10) -- by far the largest remaining gap in this analysis, and the most consensus-critical.
- No verification of exact byte-level field ordering for any serialized structure against the document's field lists.
- No verification of exact domain-separation strings (e.g. `"primechain-transaction-signature-mldsa65-v2"`) against what's actually hashed/signed in code.
- No check of §3's transaction validation rules (nonce start/increment, overflow rejection, balance sufficiency) beyond confirming validation code exists somewhere.
- No check of §9.1's authenticated (`pcpq1_`) address derivation function location/correctness.

## Recommended next task

Given the size of what's left (particularly §8-§10's consensus logic inside `sync_server.cpp`), the next reasonably-bounded task is **not** "finish the whole gap analysis in one pass" -- it's picking one of: (a) trace §8's finalization/round-change logic specifically (highest consensus-criticality), or (b) resolve Headline Finding 1 (Merkle root) with the maintainer since it's already fully diagnosed and just needs a decision, or (c) extend `protocol-formats-v0.md` itself to document the five undocumented mechanisms from Headline Finding 2 (pure documentation, no approval needed, and doesn't require finishing the rest of this analysis first).
