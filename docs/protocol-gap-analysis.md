# Primechain Protocol Gap Analysis

Status: living document, third pass (first pass: types/tx/proof formats and record fields, structurally; second pass: §8/§8.1 finalization and round-change logic, traced in detail; this pass: §10/§10.1-10.3 commit-phase quorum, genesis anchor, and epoch transitions). Analysis and documentation only -- **nothing in the source tree was changed while writing this**, and no finding below has been acted on. Scope: `docs/protocol-formats-v0.md` (both repos are `kaito-zero/primechain`'s `docs/protocol-gap-analysis` branch, based on `upstream/main` @ `1134489`) checked against the actual C++ implementation.

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

## Headline finding 3: the documented "later rounds use v3" statement contradicts current code, which always uses a newer v4 rule with cross-round locking that the spec doesn't mention at all

`protocol-formats-v0.md` §8 lists exactly three finalization rules and states:

> Validator-anchored round-1 records use `fixed-2-of-3-mldsa65-v2`; later rounds use the certificate-bearing v3 rule [`fixed-2-of-3-mldsa65-rounds-v3`].

Traced `finalizeRecordCandidate()` in `src/node/sync_server.cpp:4737` (the function that actually sets `record.finalized_by.rule` when proposing a candidate):

```cpp
if (round == 1) {
    record.finalized_by.rule = "fixed-2-of-3-mldsa65-v2";
    record.finalized_by.round_changes.clear();
} else {
    record.finalized_by.rule = "fixed-2-of-3-mldsa65-rounds-locks-v4";   // <-- not v3
    record.finalized_by.round_changes = certifiedRoundChanges(record.integer, round);
    ...
}
```

Every code path that advances past round 1 (the initial attempt, the timeout-triggered retry loop) sets the rule to `fixed-2-of-3-mldsa65-rounds-locks-v4` (`kLockedRoundFinalizationRule` in `records.cpp:23`) -- **never** to `kRoundFinalizationRule` (the v3 constant, `records.cpp:22`). `v3` is still *accepted* by verification code (`verifyRecordFinalization`, `verifyRoundChangeCertificate` both branch on `proof.rule == kRoundFinalizationRule || proof.rule == kLockedRoundFinalizationRule` throughout `records.cpp`), consistent with v3 being kept only so already-finalized historical records with that rule still replay correctly -- but nothing in the current code *produces* v3 for a new candidate. Round 1's behavior (`fixed-2-of-3-mldsa65-v2`, no round-change votes) does match the document exactly.

**What v4 actually adds, traced from `records.cpp:1560-1677` and `RoundChangeVoteV1`'s fields (`locked_round`, `locked_candidate_kind`, `locked_candidate_hash`, `locked_candidate_payload`, none of which appear anywhere in `protocol-formats-v0.md`):** a round-change vote under the v4 rule can optionally declare it's *locked* on a specific candidate it already saw partial support for in an earlier round. `verifyRoundChangeCertificate` requires locked-vote fields to be empty under v3 (rejects them: `records.cpp:1613`) and validates them (round ordering, record-kind match, payload deserializes and hashes to the declared `locked_candidate_hash`) under v4. `verifyRecordFinalization` then requires that if any round-change vote in the certificate carries the *highest* lock, the finalized candidate must be exactly that locked candidate (`records.cpp:1657-1676`) -- i.e. once enough validators have locked onto a candidate in a given round, a later round cannot finalize something else instead. This reads like a real safety property (a classic "locked round" pattern, structurally similar to what item 33 "cross-round locks" in the roadmap's Phase-B checklist is asking about) -- but it is currently **entirely unspecified**, not just under-specified: an independent implementer following `protocol-formats-v0.md` today would build a v3-only finalizer that current validators would likely reject or diverge from as soon as any round advanced past 1.

- **Document section:** §8, specifically the "later rounds use v3" sentence and the missing v4/lock mechanism.
- **Implementation:** `finalizeRecordCandidate()` (`sync_server.cpp:4737`), `verifyRecordFinalization`/`verifyRoundChangeCertificate` (`records.cpp:1560-1701`), rule constants (`records.cpp:20-23`).
- **Tests:** not identified this pass -- worth a targeted search for any test that actually forces a round change and asserts on the resulting `rule` string.
- **Status:** **DOCUMENTATION BUG** (the "later rounds use v3" claim is simply no longer true of current code) compounded with **UNDOCUMENTED MECHANISM** (the entire lock concept). This is more severe than Headline Finding 2's undocumented-but-additive fields, because here the document actively states something that contradicts observed code behavior rather than just omitting a mechanism.
- **Consensus-visible:** yes -- `finalized_by.rule` and the lock fields are part of the hashed/signed finalization certificate.
- **Conformance vector:** missing.
- **Confirmed matching the spec in the same pass** (so this isn't a wholesale rejection of §8): the core per-vote validation rules in `verifyRecordFinalization` were traced line-by-line and do match the document precisely -- quorum vote count check, public-key-derives-claimed-address check (`vote.validator_address != crypto::addressFromProtocolPublicKey(vote.public_key)`), vote target (record hash + round) check, and strict-ascending validator-address ordering (stronger than the doc's plain "sorted," since it also rejects duplicates). The signing payload's field order (record hash, round, validator address) also matches; the exact domain-separator string itself was not independently re-read against `crypto::recordFinalizationVoteSigningPayload`'s source this pass.
- **Action:** none taken -- **REQUIRES MAINTAINER DECISION** (at minimum on updating the document to describe v4 and the lock mechanism; possibly also on whether v3's continued acceptance-without-production is intentional or itself worth deprecating explicitly).

## Headline finding 4: three different quorum-threshold answers for the same three-validator network, across three sources

While tracing §10.2/§10.3, checking §8's repeated claim that the active validator set contains "exactly three" addresses led to `core::validValidatorSetSize()`:

```cpp
bool validValidatorSetSize(std::size_t validator_count) {
    return validator_count >= 1;
}
```

This does **not** enforce "exactly three" -- any validator set size of 1 or more passes. That part turns out to be intentional and already documented, just not in `protocol-formats-v0.md`: `docs/validator-economics.md`'s "Quorum Formula" section explicitly says quorum is "formula-based, not hardcoded to two of three" and gives:

```text
required_quorum = floor(2n / 3) + 1
```

So far this is just another instance of Headline Finding 2's pattern (real behavior documented in `validator-economics.md` but absent from `protocol-formats-v0.md`). But checking that formula against what the code actually computes (`requiredValidatorQuorum()` in `src/core/consensus.cpp`):

```cpp
std::size_t requiredValidatorQuorum(std::size_t validator_count) {
    if (validator_count == 0) return 0;
    if (validator_count == 1) return 1;
    return (validator_count * 2 + 2) / 3;
}
```

**These two formulas disagree, and they disagree specifically at `n = 3` -- the validator count of the network actually running today:**

| n | `validator-economics.md`'s `floor(2n/3)+1` | actual code's `(2n+2)/3` |
|---|---|---|
| 1 | 1 | 1 |
| 2 | 2 | 2 |
| **3** | **3** | **2** |
| 4 | 3 | 3 |
| 5 | 4 | 4 |
| **6** | **5** | **4** |
| 7 | 5 | 5 |

They match at every `n` checked except multiples of 3, where the documented formula is exactly one vote higher than what the code requires. For the live 3-validator network, `validator-economics.md` says finalizing a record should require all three validators to sign; the actual code accepts two. (Confirmed by hand: `floor(2*3/3)+1 = floor(2)+1 = 3`; `(3*2+2)/3 = 8/3 = 2` under C++ integer truncation.)

`protocol-formats-v0.md` §8 itself doesn't give a formula -- it just says "two or three distinct active validators sign," which happens to match what the *code* actually does for n=3, even though it directly contradicts its own "exactly three" validator-set-size claim elsewhere in the same section (the code allows other sizes) and doesn't mention the general formula at all.

- **Document section:** `docs/validator-economics.md` "Quorum Formula" (not `protocol-formats-v0.md`, which has no formula to check against here -- this finding is specifically a cross-doc-vs-code mismatch rather than a formats-doc gap like the others).
- **Implementation:** `requiredValidatorQuorum()`, `src/core/consensus.cpp:31`. Used by `hasQuorumVoteCount()` (`records.cpp:41`), which gates `verifyRecordFinalization`, `verifyValidatorEpochTransition`, and (via `validatorQuorumRequired()`/`phaseClosed()`) the commit-phase CLOSED transition -- i.e. this one formula is the actual safety threshold for essentially every quorum decision in the system.
- **Tests:** not searched this pass for one that would catch this specifically (would need a validator-count-6-or-9 style fixture, or a fixture that checks the *exact* vote count required rather than just "enough votes eventually arrive").
- **Status:** **DOCUMENTATION BUG** in `validator-economics.md` (states a formula the code doesn't implement) -- or, alternatively framed, **the code doesn't implement the documented policy**, which is the more serious reading given this is a safety threshold, not a formatting detail. Genuinely ambiguous which side is "wrong" without maintainer input on which formula was actually intended.
- **Consensus-visible:** yes -- this is about as consensus-critical as anything in the system: how many validator signatures are actually required to finalize a record, transition an epoch, or close a commit-phase round.
- **Conformance vector:** missing.
- **Action:** none taken -- **REQUIRES MAINTAINER DECISION**, and given the severity (a live 3-validator network's actual safety margin is at stake, not just a wording nit), this is the single finding in this document most worth resolving first.

## Section-by-section status (first pass -- see confidence notes per row)

| §formats-v0 | Topic | Match status | Implementation | Confidence this pass |
|---|---|---|---|---|
| §2 | Primitive types (UInt64 LE, length-prefixed Bytes/String, Hash256=SHA3-256, dev/authenticated Address formats) | Struct/type shapes confirmed present (`types.hpp`, `records.hpp`); byte-level encoding of each primitive not individually re-verified this pass | `types.hpp`, `records.cpp` serialization helpers | Structural only |
| §3 | Transaction format + validation rules (positive integer amounts, referenced primes must be finalized, sufficient balance, sum-in==sum-out+fee, nonce starts at 1 and increments by exactly one, post-batch fee crediting, overflow rejection, no floats) | `TransactionV0` struct matches documented fields exactly. Nonce enforcement confirmed present (`sync_server.cpp`, mempool conflict + apply-time checks) but the exact "starts at 1, +1 only" rule not traced to one specific line this pass. Overflow/balance/fee rules not individually traced this pass. | `records.cpp` (serialize/hash/sign), `sync_server.cpp` (validation, mempool) | Partial -- struct shape confirmed, most validation rules not yet individually verified |
| §4 | Transaction batch / Merkle root | See Headline finding 1 above | `records.cpp:1061` | Fully verified -- confirmed mismatch |
| §5 | Composite proof format (`g>=4`, `1<d<g`, `1<e<g`, `d*e==g` no overflow, address syntax) | `CompositeProofV0` struct matches. Validation rule locations not individually traced this pass. | `records.cpp`, `math/number_theory.cpp` | Structural only |
| §6 | Pratt prime proof (canonical factorization, gcd/Fermat conditions, factor-base-must-be-earlier-finalized-PrimeRecord) | `PrattPrimeProofV0`/`PrimePowerV0` structs match. `gcd()` helper confirmed present and used in a primality-adjacent check (`number_theory.cpp:215`). The doc **already documents its own gap here**: factor-base verification currently uses a local `isPrime()` shortcut rather than checking for an earlier finalized `PrimeRecord` -- self-acknowledged IMPLEMENTATION GAP, not a new finding. | `math/number_theory.cpp`, `records.cpp` | Partial -- structs and the self-declared gap confirmed; full Fermat/gcd condition-by-condition trace not done this pass |
| §7 | Record formats (common validation: height==prev+1, integer==prev+1, prev-hash linkage, tx_batch root match, state_root reserved-not-enforced) + genesis | See Headline finding 2 for the field-list gap. `state_root`'s "reserved, not yet enforced" claim not independently re-verified this pass (plausible given README's "state-root construction" is listed as an Open Item in §12). | `sequential_node.cpp`, `sync_server.cpp` | Partial |
| §8 / §8.1 | Finalization proof (2-of-3 voting, round rules, round-change certificates, `.rounds`/`.finalization` sidecars) | **Traced in this pass (second pass), superseding the first pass's note below.** Round-1 behavior and the per-vote validation rules (quorum count, address-derives-from-pubkey, vote-target match, strict-ascending sort) all confirmed matching the document exactly, line-by-line in `verifyRecordFinalization`. But: see Headline Finding 3 -- the document's "later rounds use v3" claim is out of date (code always uses a v4 rule with an undocumented cross-round-lock mechanism instead; correcting my own first-pass note above, which incorrectly assumed the doc's §8.1 prose already covered the `locked_*` fields -- it does not, confirmed via a direct grep for "lock" in the document). The `.rounds`/`.finalization` sidecar-clearing claim and the `--finalization-timeout-ms` retry-then-round-change behavior were structurally confirmed present (`finalizeRecordCandidate`'s timeout/retry loop matches the doc's description reasonably closely) but not verified field-by-field. | `sync_server.cpp:4737` (`finalizeRecordCandidate`, the propose/round-change/timeout loop), `records.cpp:1560-1701` (verification) | Partial -- core vote validation and round-1 behavior verified in detail; v3-vs-v4 discrepancy verified in detail (Finding 3); sidecar file behavior and exact timeout/retry semantics only structurally checked; the commit-phase state machine in §10/§10.1-10.3 still not traced |
| §9 | Record hashing (candidate hash vs. finalized hash separation) | `candidateRecordHash`/`finalizedRecordHash`/`legacyCandidateRecordHashWithoutFinalization` all confirmed present as distinct functions in `records.hpp`, consistent with the doc's candidate-vs-finalized hash distinction. Byte-level field-ordering-matches-document claim not verified this pass. | `records.cpp` | Structural only |
| §9.1 | ML-DSA-65 composite contributor auth (address = pcpq1 + first 20 bytes of hash(pubkey), domain-separated commit/reveal payloads) | Address-derivation helper (`developmentAddressFromPublicKey`) exists but is named for the *development* address scheme (§2's `pcdev1_`), not obviously the authenticated `pcpq1_` one -- the authenticated derivation likely lives elsewhere (not traced this pass). Commit/reveal domain strings referenced in `sync_server.cpp` (`SUBMIT_COMPOSITE_REVEAL` handling) but not diffed against the doc's exact domain strings this pass. | `sync_server.cpp`, `wallet/miner_identity.cpp` (likely) | Structural only, address-derivation function location genuinely unclear this pass -- flag as **UNCLEAR**, worth a dedicated look |
| §10 / §10.1 / §10.2 / §10.3 | Commit-reveal, controlled commit-phase quorum state machine, genesis validator anchor, signed epoch transitions | **Traced in this pass (third pass), superseding the note below.** §10.1's state determination matches exactly: `GET_COMMIT_PHASE`/`GET_MINING_VIEW` compute CLOSED (`phaseVoteCount >= quorum`), CLOSING (`phaseVoteCount != 0` but below quorum), OPEN (neither) -- precisely the doc's OPEN→CLOSING→CLOSED description. Winner selection (`selectedCommitment()`) matches the "lexicographically smallest `(commitment_hash, provider_address)`" rule exactly, field-for-field. The `TIMED_OUT(N→N+1)` recovery transition is present (`CommitPhaseTimeoutVote`, `commitPhaseTimeoutCertified()` requiring `validatorQuorumRequired()` matching votes, `activeCommitPhaseRound()` walking forward through certified timeouts) and structurally matches, though the doc's wording "nodes clear only that round's temporary commitments/votes" implies deletion while the code appears to just advance which round is considered active without necessarily deleting the old round's stored data -- functionally equivalent (old-round data is never consulted again since every lookup is keyed by the now-current round) but not byte-identical to the doc's phrasing, not chased further this pass. §10.2 (genesis anchor) and §10.3 (epoch transitions) both verified matching closely: `verifyGenesisConfig`'s version/height gating and `verifyValidatorEpochTransition`'s epoch-plus-one/activation-plus-one/quorum/canonical-sort checks all line up with the document precisely. **But see Headline Finding 4**, found while checking §10.2's "exactly three validators" claim: the actual validator-set-size check only requires `>= 1` (matches `validator-economics.md`'s explicit non-3-hardcoded design, not `protocol-formats-v0.md`'s "exactly three" wording), and the quorum-count formula that decision depends on disagrees with `validator-economics.md`'s own documented formula specifically at the currently-deployed validator count of 3. The `SUBMIT_PHASE_VOTE_BUNDLE`/`SUBMIT_PHASE_VOTE_PEER`/`SUBMIT_PHASE_VOTE_BUNDLE_PEER` wire commands exist in code with no mention in the doc (which only lists plain `SUBMIT_PHASE_VOTE`) -- not investigated further this pass, likely a peer-propagation/batching optimization rather than a new consensus rule, but unconfirmed. | `sync_server.cpp` (state machine, `4110-5436` region), `records.cpp` (`verifyGenesisConfig:1339`, `verifyValidatorEpochTransition:1366`), `core/consensus.cpp` (quorum formula) | Detailed -- core state machine and both sub-sections traced; timeout sidecar-clearing semantics and the bundle/peer wire commands not fully chased |
| §11 | Bitcoin mirror payload | Self-declared "future optional" by the document itself. No `BitcoinMirror`-named struct found in `records.hpp`. | none found | Consistent -- doc correctly says this isn't built yet. **PLANNED**, not a gap. |
| §12 | Open items | Self-declared "intentionally unresolved" by the document itself (production address encoding, production signature scheme, real SHA3-256 integration, UTXO representation, reward allocation format, state-root construction, permissionless consensus, DoS limits). Worth noting: "real SHA3-256 integration" as an open item is confusing given `crypto::sha3_256` is called directly throughout `records.cpp` -- possibly stale wording (may mean "a from-scratch/audited implementation" vs. a library) rather than "SHA3-256 isn't used yet." Not resolved this pass -- **UNCLEAR, worth asking**. | n/a | Consistent with self-declaration; one wording item flagged |

## What this pass did NOT cover (explicitly, so the next pass knows where to start)

- §8/§8.1 (finalization/round-change) and §10/§10.1-10.3 (commit-phase quorum, genesis anchor, epoch transitions) are now both traced -- see Headline Findings 3 and 4 and the updated table rows. **What's left that's still purely structural, not traced:** §9.1's authenticated address derivation, §3's transaction validation rules beyond "the code exists somewhere," and byte-level serialization/domain-string verification everywhere.
- No verification of exact byte-level field ordering for any serialized structure against the document's field lists.
- No verification of exact domain-separation strings (e.g. `"primechain-transaction-signature-mldsa65-v2"`, the finalization-vote signing domain, the commit-phase-vote domain) against what's actually hashed/signed in code.
- No check of §3's transaction validation rules (nonce start/increment, overflow rejection, balance sufficiency) beyond confirming validation code exists somewhere.
- No check of §9.1's authenticated (`pcpq1_`) address derivation function location/correctness.
- No search yet for a test that actually forces a round change and asserts on the resulting rule/lock behavior (Finding 3), or that exercises a validator count other than 3 to observe the quorum-formula discrepancy directly (Finding 4).
- The `SUBMIT_PHASE_VOTE_BUNDLE`/`*_PEER` wire commands (found while tracing §10.1) were not investigated -- likely peer-propagation plumbing, not confirmed.

## Recommended next task

Four headline findings are now fully diagnosed with specific file/line evidence and don't need more tracing -- they need a maintainer decision. Given Finding 4's severity (the actual safety margin of the live 3-validator network, not a wording nit), **the recommended next step is (b): take Findings 1, 3, and 4 to the maintainer**, rather than continuing to trace further sections (§3's transaction rules, §9.1's address derivation) whose findings, if any, are unlikely to outweigh getting a decision on Finding 4 specifically. Extending `protocol-formats-v0.md` itself (documenting Finding 2's five mechanisms and Finding 3's v4/lock mechanism) remains available as parallel, lower-stakes, no-approval-needed work whenever convenient.
