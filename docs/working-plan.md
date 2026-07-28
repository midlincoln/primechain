# Primechain Working Plan

This is the active working plan for moving from the current controlled launch-testnet toward a credible mainnet-candidate launch. It combines product/community work with the remaining Bitcoin-readiness engineering gaps.

## Current Position

Primechain has a working controlled launch-testnet with:

- three active validators,
- on-chain validator endpoint records,
- reserve-backed validator admission workflows,
- ML-DSA-65 signed miner, wallet, and validator identities,
- signed prime submissions and signed composite commit/reveal,
- transaction submission, mempool sync, wallet history, and address/tx lookup,
- validator fee pools, validator reward pools, and participation-aware deterministic distribution,
- operator checks including `release-check`, `version-network`, `doctor-network`, `mempool-network`, `chain-doctor`, and `launch-summary`,
- public GitHub source and launch evidence tooling.

The next goal is not to rush an irreversible mainnet label. The goal is to prepare a public, credible mainnet-candidate launch while continuing development in an agile but explicit way.

## Principles

- Do not call the network final mainnet until the protocol, docs, launch procedure, and risks are clear.
- Keep on-chain validator endpoints authoritative after first contact.
- Keep mathematical mining central; do not add artificial hashcash difficulty unless the design direction changes.
- Make the external TCP/miner protocol stable enough for independent clients.
- Preserve balances when possible, but do not promise irreversible value during candidate/testnet stages.
- Be explicit with investors, miners, and reviewers about what is implemented and what is experimental.

## Phase 1: Public Readiness

Priority: immediate.

Deliverables:

- Add a README section: `Relation To Primecoin`.
- Update the whitepaper/review response with a clean Primecoin comparison.
- Add `docs/miner-quickstart.md`.
- Add `docs/mainnet-candidate-plan.md`.
- Add `docs/risk-disclaimer.md`.
- Update the website/evidence page with:
  - GitHub link,
  - launch-testnet status,
  - evidence downloads,
  - miner quickstart link,
  - explicit experimental-status disclaimer.

Purpose:

- Reduce confusion with Primecoin and older `PrimeChain` references.
- Let new miners build, sync, mine, and check rewards without hand-holding.
- Make public communication honest and defensible.

## Phase 2: Community Presence

Priority: immediate, parallel with docs.

Deliverables:

- Increase technical presence on Metzdowd gradually and respectfully.
- Comment on relevant protocol/cryptography/mining discussions without over-promoting.
- Prepare a short Metzdowd update:
  - what changed after feedback,
  - current 3-validator testnet status,
  - GitHub link,
  - evidence link,
  - Primecoin comparison,
  - request for technical review.
- Prepare a BitcoinTalk announcement draft.
- Create a Discord with channels:
  - `announcements`,
  - `getting-started`,
  - `mining`,
  - `validators`,
  - `protocol-research`,
  - `support`,
  - `bugs`.
- Pin build/mining commands and current bootstrap nodes.

Purpose:

- Build credibility before launch.
- Attract miners and reviewers before money/value claims dominate the discussion.

## Phase 3: Miner Onboarding

Priority: immediate after quickstart doc.

Deliverables:

- One documented command path:
  - clone repo,
  - build,
  - initialize workdir,
  - create wallet,
  - sync,
  - mine,
  - check wallet summary,
  - inspect rewards/history.
- Troubleshooting guide for:
  - passphrase prompts,
  - peer connection failures,
  - stale workdir,
  - validator disagreement,
  - build failures,
  - missing rewards,
  - mempool pending vs confirmed transactions.
- Public explanation that miners submit actual mathematical proofs, not hashes.
- Later: external miner protocol vectors so independent clients can mine without linking the C++ node code.

Purpose:

- Let outside miners join and experiment.
- Encourage better math algorithms for factorization and primality proof construction.

## Phase 4: Mainnet-Candidate Preparation

Priority: before any public launch announcement using mainnet language.

Deliverables:

- Decide final public name/branding.
- Avoid ticker/name conflict with Primecoin/XPM and old PrimeChain/PRIME references.
- Decide candidate network name, for example:
  - `launch-testnet`,
  - `mainnet-candidate-1`,
  - `genesis-candidate-1`.
- Decide high validator reserve/admission requirement.
- Document the reserve-backed incentive model and be explicit that reserve
  forfeiture/slashing is future work, not current consensus behavior.
- Decide genesis ceremony and exact genesis validator set.
- Decide bootstrap IPs or DNS seed names.
- Freeze protocol/network version for the candidate.
- Tag a GitHub release, for example `v0.1.0-mainnet-candidate`.
- Publish checksums and release notes.
- Run a clean genesis from scratch.
- Publish launch evidence and repeatable release-check output.

Purpose:

- Launch with enough structure that people can reproduce, critique, and join without assuming final production guarantees.

## Phase 5: Bitcoin-Readiness Catch-Up

These are the remaining gaps compared with Bitcoin-like public network maturity. They do not all block a mainnet-candidate, but they should be tracked explicitly.

### P2P Robustness

- First-contact seed mechanism for new nodes.
- Better peer discovery beyond manually configured peers.
- Persistent peer database for non-validator nodes.
- Peer scoring, quarantine, and temporary bans.
- Eclipse-attack mitigation.
- Clear rule: on-chain validator endpoints are authoritative after first contact.

### Mempool Policy

Implemented basics:

- mempool size limits,
- per-sender limit,
- mempool sync,
- stale-entry expiry,
- conflicting nonce rejection.

Remaining:

- stronger fee/priority policy,
- better conflict replacement rules,
- anti-spam behavior under many small transactions,
- clearer pending-vs-confirmed UX.

### Block/Record Production Policy

Design direction:

- mathematical difficulty should come from prime/composite proof work,
- avoid artificial hashcash-style mining if possible.

Remaining:

- define target chain-progress expectations,
- handle slow hard integers without stalling transaction UX,
- balance prime and composite incentives,
- document why Primechain does not copy Bitcoin's hash difficulty retargeting.

### Fork/Reorg Handling

Implemented basics:

- same-tip conflict classification,
- controlled quorum finalization,
- immutable finalized quorum records.

Remaining:

- explicit fork-choice policy for wider networks,
- branch storage and rollback/replay tests,
- partition and reconnect tests,
- validator disagreement scenarios.

### Protocol Versioning And Independent Clients

Implemented basics:

- `version`, `GET_VERSION`, `version-network`, protocol/network fields.

Remaining:

- `docs/protocol-v1.md`,
- conformance vectors for signatures, transaction hex, prime submissions, composite commit/reveal, and record hashes,
- compatibility/rejection rules for incompatible peers,
- external miner/client examples.

### Storage And Recovery

Implemented basics:

- append-only record store,
- incomplete-tail recovery,
- indexes,
- replay snapshots,
- sidecar recovery,
- `chain-doctor`,
- `chain-recover`.

Remaining:

- more crash tests,
- backup/restore docs,
- deterministic state roots,
- archival pruning design after state roots and checkpoints.

### Security Hardening

Implemented basics:

- dangerous admin commands restricted to loopback unless explicitly allowed,
- framed-message limits,
- range limits,
- command/write limits.

Remaining:

- fuzzing malformed TCP commands,
- DoS tests,
- per-source rate limits,
- peer abuse handling,
- security review of all public TCP commands.

### Wallet And User Experience

Implemented basics:

- wallet summary,
- wallet send,
- prompt-based passphrase handling,
- wallet history,
- wallet pending,
- wallet dashboard,
- tx lookup,
- address report.

Remaining:

- clearer interactive CLI wallet,
- backup/export/import flows,
- key rotation and recovery docs,
- GUI/web wallet later,
- safer defaults for passphrase environment variables.

### Release Packaging

Remaining:

- GitHub release tags,
- Linux binaries or tarballs,
- checksums,
- reproducible build notes,
- install guide for miners,
- install guide for validators,
- service files packaged through `primechain-ops`.

### Legal / Risk / Communication

Remaining:

- risk disclaimer,
- experimental network disclaimer,
- no guaranteed value statement,
- not investment advice statement,
- policy for whether candidate balances may carry forward.

## Phase 6: After Candidate Launch

Deliverables:

- Weekly network evidence reports.
- Weekly release notes.
- Public issue tracking for protocol bugs.
- Miner leaderboard or explorer views.
- Discord support rhythm.
- Ongoing Metzdowd/BitcoinTalk updates only when there is substantive progress.
- Decide after a stability window whether candidate balances carry into final mainnet.

## Immediate Next Items

1. Add `Relation To Primecoin` to README.
2. Add `docs/miner-quickstart.md`.
3. Add `docs/mainnet-candidate-plan.md`.
4. Add `docs/risk-disclaimer.md`.
5. Update website/evidence page with GitHub, quickstart, and disclaimer links.
6. Prepare Metzdowd update draft.
7. Prepare BitcoinTalk announcement draft.
8. Create Discord and pin getting-started commands.
