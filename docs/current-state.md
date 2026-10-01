# Primechain Current State

Status: living document. Snapshot as of 2026-08-13, git `frontier-miner-structured-logging` branch (fork: `kaito-zero/primechain`) off `upstream/main` (`midlincoln/primechain`) @ `1134489`, plus this branch's own commits.

**Repo/branch scope, stated explicitly to avoid conflating the two:** everything below describes the state of `kaito-zero/primechain`'s `frontier-miner-structured-logging` branch, which is `upstream/main` @ `1134489` plus this branch's own additions. Anything below that's only true of this branch (not yet on `upstream/main`) is called out as such -- most notably, **this branch is currently open as PR #14 against `midlincoln/primechain` and has not been merged** (structured/leveled logging for `primechain-frontier-miner` and `run-jobs`; explicitly no protocol/behavior change, only diagnostic-text presentation). Everything else described as "implemented" in this document (the ~19,954-line `src/` tree minus this branch's own diff, and all pre-existing `docs/*.md` files) already existed on `upstream/main` before this branch and is not this session's work. This document itself (`docs/current-state.md`) is new, exists only on this branch, and is not yet part of `upstream/main` either.

Purpose: ground the next phases of work (protocol gap analysis, conformance vectors, independent verifier, fuzzing, chaos testing) in what the repository *actually* contains today, rather than assumptions carried over from earlier sessions or older docs. This is observational only -- no source changes were made while writing it, and no item below should be read as "protocol work is complete." Status labels used throughout: **implemented** / **partial** / **planned** / **known issue** / **requires maintainer decision**. An additional distinction used where relevant: **on `upstream/main`** vs **on this branch / open PR, not yet upstream**.

## 1. Existing documentation inventory

`docs/` already contains substantially more than a bare README:

| File | Covers |
|---|---|
| `working-plan.md` | The project's own phased plan (Phase 1 Public Readiness → Phase 6 After Candidate Launch), P2P/mempool/fork/security/wallet/release sub-sections, and an "Immediate Next Items" list (miner quickstart, risk disclaimer, Metzdowd/BitcoinTalk/Discord). |
| `production-roadmap-v0.md` | Lower-level production backlog. |
| `protocol-formats-v0.md` | **status: partial, self-declared draft.** Canonical types (UInt64 little-endian, length-prefixed Bytes/String), SHA3-256 hashing, tx format, ML-DSA-65 signature domains, Pratt proof format, record structure, validator vote/finalization format. Explicitly states independent clients may use different algorithms and validators only trust submitted evidence + signatures, not the miner implementation -- i.e. the document's own philosophy already assumes an eventual independent verifier. It also documents at least one known implementation-vs-target gap itself: Pratt factor validation currently uses a local `isPrime(...)` shortcut where the documented target is verifying against an earlier finalized `PrimeRecord`. Do not treat this document as finished or as equivalent to a Phase-C `protocol-v1.md` -- it explicitly isn't one yet ("not the final production cryptographic specification"). The right next step against this document is a spec↔implementation gap audit (see "Recommended next task" below), not a rewrite.
| `validator-economics.md` | Validator roles, quorum formula, admission, reserve, work score, endpoint updates, economic policy voting, mining reward split, tx fees, validator reward distribution, slashing/removal status, launch position. This is a real head start on what Phase B/S would otherwise ask for from scratch. |
| `testnet-plan.md` | Current state, node count reasoning, local/private/public testnet stages, blockers for outside miners, practical sequence. |
| `scale-estimates.md` | Chain scale estimation, an estimator tool, design implications. |
| `launch-validator-runbook.md`, `mainnet-validator-onboarding.md` | Operational runbooks for standing up/onboarding validators. |
| `client-derived-indexes.md` | Design doc for the address/reward/participation derived-index feature. |
| `primescan-design.md` | Design doc for the explorer (thin client over `primechain-client`, no independent chain parsing). |
| `validator-gossip-architecture-v0.md`, `validator-economy-v0.md` | Earlier-stage design drafts (not verified against current code as part of this pass). |
| `review-response.md`, `development-log.md` | History/response records, not verified against current code as part of this pass. |

None of these together constitute the Phase-C `protocol-v1.md` this roadmap eventually wants (byte-level precision sufficient for an independent implementer without reading the C++), but `protocol-formats-v0.md` + `validator-economics.md` are a real, usable foundation for it.

## 2. Source tree inventory (by responsibility, not yet a line-by-line audit)

| Area | File(s) | Approx. size | Status |
|---|---|---|---|
| Crypto primitives | `crypto/hash.cpp`, `crypto/signature.cpp` | small | implemented; not independently re-verified against a second implementation this pass |
| Number theory (primality, Pratt, factoring) | `math/number_theory.cpp` | not measured | implemented; underlies both the miner and validator-side proof checks |
| Consensus core types | `core/consensus.cpp` | 131 lines | implemented, small -- likely shared types/helpers rather than the full consensus state machine (that appears to live mostly in `sync_server.cpp`, see below) |
| Record protocol (serialization, tx format) | `protocol/records.cpp` | 1887 lines | implemented; this is very likely the closest existing thing to "the canonical serialization code" that `protocol-formats-v0.md` describes -- has not yet been mapped clause-by-clause against that doc |
| Sequential node / chain replay | `node/sequential_node.cpp` | 1515 lines | implemented; local-only deterministic replay used by the CLI's `inspect`/`launch-report`/index-building commands |
| Sync server (validator/peer node) | `node/sync_server.cpp` | **8221 lines** | implemented, by far the largest and most consensus-relevant file. Contains: peer/networking primitives, wire-format parsing for records/commitments/phase-votes, finalization-round/subject-hash helpers, `providerCooldownSatisfied` (the composite winner-cooldown fairness rule), tip-replacement candidate validation, composite-proof-index loading, per-IP connection-count limiting (`g_active_remote_connections`). **This file has NOT had a deep line-by-line semantic audit in this pass** -- only a structural (function-signature) survey. This is the natural target of the next phase (protocol gap analysis) for anything quorum/finalization/round-change-related. |
| Validator registry | `node/validator_registry.cpp` | 89 lines | implemented, small -- likely a thin wrapper; the bulk of validator *logic* (admission, reserves, epochs) is described in `validator-economics.md` and presumably enforced from within `sync_server.cpp`, not confirmed line-by-line this pass |
| Storage (record store, commitments, finalization, phase votes, round-changes, validator epochs, replay snapshots, atomic file writes) | `storage/*.cpp` (7 files, 102-849 lines each) | implemented; `record_store.cpp` at 849 lines is the largest -- append-only chain storage with a rebuildable index, matches the README's claim of "crash-recoverable append-only chain store, rebuildable indexes" |
| Miner | `miner/frontier_main.cpp`, `miner/composite_main.cpp`, `miner/main.cpp` | on `upstream/main`: implemented, no logging changes. **On this branch / open PR #14, not yet upstream:** `frontier_main.cpp` and `client.cpp`'s `run-jobs` got structured/leveled logging (diagnostic-text-only, no protocol/behavior change per the PR description) | separately, in the `kaito-zero/primechain3` clone specifically (a different, local-only clone -- not this fork, not upstream, never pushed anywhere), there's uncommitted probe-batching/connection-capping and a winner-cooldown fast-path that also exist as separate named branches on the `kaito-zero/primechain` fork (`perf/parallel-network-io-and-cooldown-fix`, `perf/parallelize-frontier-miner-probes`, neither merged upstream, neither reconciled with the primechain3 clone's version of the same idea) -- **known issue: possible duplicate/diverged work across three places (primechain3 clone, two fork perf branches), not yet compared line-by-line** |
| CLI (`primechain-client`) | `tools/client.cpp` | 6134 lines, **65 subcommands** (`command == "..."` dispatch count) | implemented; large and actively developed this session (run-jobs, sync-peer, add-mine-job, and their logging all touched). Not all 65 subcommands were exercised this session. |
| Other CLI tools | `tools/{balance_query,send,sequential_chain,store_inspect,sync_download,sync_query,composite_commitment,estimator,arithmetic_bench}.cpp` | implemented, not individually audited this pass |
| Test-fixture-only tools | `tools/corrupt_sync_server.cpp`, `tools/write_corrupt_store.cpp` | implemented -- exist specifically to test corruption-recovery paths, not part of the public validator profile |
| Wallet | `wallet/miner_identity.cpp`, `wallet/wallet_main.cpp` | implemented |

## 3. Test suite

124 `add_test(...)` entries in `CMakeLists.txt`, plus a `tests/*.sh` directory of shell-driven smoke/integration tests (index-building, board reports, participation, rewards, validator admission, epoch voting, composite lottery, chaos-adjacent cases like `workdir_stale_commitment_recovers.sh`). These are real CLI/pipe-level integration tests (spin up `primechain-sync-server`, run `primechain-client`, grep the output) -- valuable, and exercised extensively this session, but they are **not** the kind of fixed-value, implementation-independent conformance vectors that Phase D of the roadmap wants. That's a real gap, not a duplicate of what already exists.

**Known issue, confirmed deterministic on a clean checkout (no local changes of any kind) this session:**
- `frontier_miner_uses_signed_composite_identity`, `frontier_miner_advances_to_20`, `frontier_miner_records_propagate_to_peer`, `client_workdir_mines_from_synced_history` all fail/hang at a specific point: a single-provider test fixture reaches an integer where the *only* available composite provider is already in winner-cooldown, and since cooldown only clears when a *different* provider wins a record, a fixture with just one provider can never satisfy it. This is **not** a cooldown bug -- Ovanes has described cooldown as an intentional fairness rule -- the accurate framing is "these test fixtures are incompatible with current cooldown semantics" (a test-design issue). Verified via `git stash`-based A/B testing: identical failure on a completely clean checkout, unrelated to any of this session's own changes.
- `frontier_miner_uses_signed_composite_identity` and two others separately fail with `could not construct Pratt proof for 5; start from a fresh node or add proof-index bootstrap` on a fresh single-miner run, also reproduced on a completely clean checkout. Root cause **not yet investigated** -- flagged here, not diagnosed.

Both should get a real GitHub issue (or at minimum a tracked TODO) rather than living only in session context.

## 4. What's genuinely not started (matches roadmap Phases D through H, no overlap found)

- No `test-vectors/` directory, no fixed conformance vectors of any kind.
- No Rust code anywhere in the repository (`find . -iname "*.rs"` empty) -- Phase F (independent verifier) is fully greenfield.
- No fuzzing harness (`find . -iname "*fuzz*"` empty) -- Phase H is fully greenfield.
- No `docs/protocol-gap-analysis.md`, `docs/audit-readiness.md`, or `docs/consensus-trust-model.md` yet.

## 5. Adjacent ecosystem (primewallet, primescan) -- brief, see their own repos for detail

- **primewallet**: Python stdlib HTTP server, 127.0.0.1-only by design, shells out to `primechain-client` for all crypto/consensus interaction rather than reimplementing it (matches roadmap Phase O's stated preference). This session added structured Activity-feed filtering (All/Sent/Received/Rewards/Fees) and fixed a real crash (`BrokenPipeError` on a client disconnecting mid-response cascading into a second, uncaught failure trying to report the first).
- **primescan**: also Python stdlib, explicitly designed to do "no chain-reading of its own" (parses `primechain-client`'s own text output only) -- matches roadmap Phase N's stated preference to avoid duplicating consensus logic. This session fixed real reported robustness issues (Python version, executable bit, startup sync blocking with a new `--no-initial-sync` flag, `job-status`/`launch-report-workdir` timeouts no longer 502ing the whole page).

## 6. Recommended next task (per this document's own audit)

**`docs/protocol-gap-analysis.md`**: for each normative rule in `protocol-formats-v0.md`, record its documented section, the implementing file/function, existing test coverage, whether the implementation matches the document, whether a conformance vector exists yet, whether the rule is consensus-visible, and -- for any mismatch -- whether it's a documentation bug, an implementation bug, unfinished prototype behavior, or something needing a maintainer decision. Analysis and documentation only; fix nothing discovered during it. This directly de-risks Phase D (test vectors) by establishing, before anything gets frozen into a fixed vector, whether the behavior being frozen is actually the intended protocol rule or just an accidental current implementation detail.
