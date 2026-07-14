# Primechain Validator And Gossip Architecture v0

This document defines the target architecture for validator membership,
validator-owned coordination, and gossip. It is a design target, not current
consensus behavior.

The key boundary is:

```text
Blockchain stores authority.
Gossip stores liveness.
Validators execute consensus.
Miners submit work.
Clients observe state.
```

## Roles

### Miner

A miner discovers arithmetic evidence and submits work:

- prime proof candidates,
- composite commitments,
- composite reveals,
- transfer transactions once transfer UX is complete.

A miner must not be responsible for assembling validator quorum. The miner
may submit to one or more public nodes, but validator-to-validator coordination
belongs to validators.

### Client

A client is a user-facing program. It may mine, query balances, submit
transactions, or inspect records. It should read `GET_MINING_VIEW` and other
status views, but it should not decide validator membership or finality.

### Public Node

A public node accepts client traffic and serves sync/status queries. A public
node may also be a validator, but the roles are separate. A non-validator
public node relays and observes; it does not sign validator votes.

### Validator

A validator is an admitted settlement institution. It:

- signs commit-phase votes,
- signs timeout and round-change votes,
- signs finalization votes,
- participates in validator admission and removal votes,
- runs a reachable endpoint,
- maintains a locked reserve,
- earns validator rewards for signed finality work.

## On-Chain Authority

Validator authority comes from replayed chain records, not from a local config
file. A node may have local peers for bootstrapping, but the chain decides whose
validator signatures count.

The chain should eventually contain these validator event families:

```text
VALIDATOR_CANDIDATE
VALIDATOR_RESERVE_LOCK
VALIDATOR_ENDPOINT_OBSERVATION
VALIDATOR_ADMISSION_VOTE
VALIDATOR_ADMITTED
VALIDATOR_ENDPOINT_UPDATE
VALIDATOR_REPUTATION_REVIEW
VALIDATOR_EXIT_REQUEST
VALIDATOR_REMOVED
```

The current hardcoded `--validator-set` model is only a bootstrap/demo
mechanism.

## Epochs

An epoch is a fixed frontier-integer range during which validator membership
and policy parameters are constant.

Example:

```text
epoch_length = 1000
epoch 0 = integers 2-999
epoch 1 = integers 1000-1999
epoch 2 = integers 2000-2999
```

Validator changes are decided during one epoch and activate in a future epoch.
No validator may be admitted or removed in the middle of an integer's
commit/reveal/finalization lifecycle.

Initial testnet constants:

```text
epoch_length = 1000
validator_activation_delay = 1 epoch
validator_exit_delay = 1 epoch
validator_reserve_unlock_delay = 2 epochs
```

## Validator Eligibility v0

A candidate is eligible only if it satisfies objective chain rules and then
receives validator approval.

Initial testnet constants:

```text
validator_min_work_score = 100
validator_min_reserve_micro_units = 5000000
validator_endpoint_observation_window = 100 integers
validator_endpoint_required_uptime = 80%
validator_admission_quorum = 2/3 active validators
```

The v0 work score is intentionally simple and governance-adjustable:

```text
work_score =
    10 * prime_records_mined
  +  2 * composite_records_mined
  + discovery_micro_units / 100000
```

Eligibility rule:

```text
eligible if:
  work_score >= validator_min_work_score
  reserve_locked >= validator_min_reserve_micro_units
  endpoint_uptime >= validator_endpoint_required_uptime
  no active disqualification record exists
  admission_votes >= validator_admission_quorum
```

This formula is a placeholder. The record types must be designed so board
policy can later change weights and thresholds without changing the meaning of
historical records.

## Work History And Anti-Transferability

Primechain validator reputation should be earned by addresses that performed
real chain work. Work history must not be treated as a freely transferable
asset.

Rules:

- work score is replay-derived from records credited to an address,
- reserve coins may be transferred before lock, but work score may not,
- a wallet sale must not silently transfer validator reputation,
- sponsorship or delegation, if later allowed, must be explicit, delayed, and
  revocable by chain rule.

This protects validator admission from pure capital purchase of old mining
wallets.

## Reserve Lock

Validator reserves are locked balances that prove economic commitment.

The first reserve implementation can use a simple record:

```text
VALIDATOR_RESERVE_LOCK
  candidate_address
  reserve_address
  income_address
  amount_micro_units
  lock_epoch_start
  unlock_delay_epochs
  signature
```

Locked reserves cannot be spent while active. If a validator's reserve drops
below the active minimum, that validator becomes ineligible at the next epoch.

## Admission Flow

Admission should be replayable:

1. Candidate submits `VALIDATOR_CANDIDATE`.
2. Candidate locks reserve with `VALIDATOR_RESERVE_LOCK`.
3. Existing validators publish endpoint observations.
4. Existing validators vote with `VALIDATOR_ADMISSION_VOTE`.
5. If objective eligibility and quorum both pass, the chain records
   `VALIDATOR_ADMITTED`.
6. Membership activates at a deterministic future epoch.

Validator votes are cryptographic records signed by validator wallets. The
operator or institution behind the wallet makes the governance decision, but
the chain verifies the vote and quorum.

Validators should not be able to admit an objectively ineligible candidate.
They may reject an eligible candidate, but rejection should be recorded with a
reason code.

## Endpoint Updates

Validator identity is the signing address. IP address or DNS name is routing
metadata.

Endpoint changes should be signed chain records:

```text
VALIDATOR_ENDPOINT_UPDATE
  validator_address
  endpoint_host
  endpoint_port
  effective_epoch
  metadata_hash
  signature
```

Endpoint updates should activate at a future epoch or after a short delay, not
inside an in-progress finalization.

## Removal And Exit

Removal and exit must also be replayable.

Voluntary exit:

```text
VALIDATOR_EXIT_REQUEST
  validator_address
  requested_exit_epoch
  reserve_unlock_epoch
  signature
```

Forced removal may be triggered by:

- reserve below minimum,
- repeated missed finalization duties,
- provable equivocation,
- endpoint uptime below threshold,
- validator removal vote meeting quorum.

Initial testnet removal can be conservative and use only explicit validator
vote plus delayed activation. Slashing should not be enabled until evidence
formats are precise.

## Gossip Pool

The gossip pool is live networking state. It is not consensus authority.

Runtime memory should track:

```text
connected peers
known validator endpoints
pending commitments
pending phase votes
pending finalization votes
pending timeout and round-change votes
recent message hashes for deduplication
per-peer reachability, latency, and failure counters
```

A node may persist a small local cache:

```text
endpoint history
last seen time
last successful sync
average latency
failure count
protocol version
```

But replayed chain state remains the source of authority for active validators.

## Validator-Owned Mining Coordination

The target mining flow:

1. Miner asks any public node for `GET_MINING_VIEW`.
2. Miner submits commitment or prime proof to one or more public nodes.
3. Validators gossip the submission among themselves.
4. Validators close the commit phase when the deterministic rule is satisfied.
5. Validators gossip close votes and finalization votes.
6. Miner observes the closed phase.
7. Miner reveals only if its commitment won.
8. Validators finalize the record and gossip the result.

The client should not collect validator votes directly. It should not have to
know which validators are currently reachable beyond selecting public
submission endpoints.

## Messages Validators Should Gossip

Initial gossip message families:

```text
NEW_RECORD
COMMITMENT
COMMIT_PHASE_VOTE
COMMIT_PHASE_TIMEOUT
FINALIZATION_VOTE
ROUND_CHANGE
VALIDATOR_ENDPOINT_UPDATE
VALIDATOR_CANDIDATE
VALIDATOR_ADMISSION_VOTE
BOARD_POLICY_RECORD
```

Each message must carry enough data for deduplication, signature verification,
and replay-safe persistence.

## What Is Not Trusted

The architecture must not trust:

- a miner's claimed validator list,
- a client's local peer list,
- a single validator endpoint's current view,
- off-chain operator reputation,
- local gossip cache entries,
- mutable IP addresses as validator identity.

The chain decides authority. Gossip helps the authorized parties communicate.

## Implementation Order

Recommended order:

1. Define validator registry and epoch record formats.
2. Add replay-derived active validator set by epoch.
3. Add endpoint update records.
4. Add validator candidate, reserve lock, and admission vote records.
5. Add validator-owned commit-phase coordination.
6. Move quorum assembly out of client mining.
7. Add local peer cache and runtime gossip pool.
8. Add endpoint observation/reputation reports.
9. Add delayed removal and exit records.

This should replace the current manual `--peer` and client-quorum model
incrementally, without pretending the current demo configuration is the final
network architecture.
