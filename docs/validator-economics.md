# Validator And Economic Rules

This document summarizes the launch-testnet validator and economic rules that are intended to carry into the mainnet-candidate plan unless explicitly changed before genesis.

Primechain is currently a controlled reserve-backed validator network. It is not yet a permissionless proof-of-stake network. Validator membership, endpoint updates, economic policy, and reward distribution are replay-derived chain state, but reserve forfeiture and automated slashing are planned policy work rather than active consensus behavior.

## Validator Roles

Active validators sign and relay consensus evidence for arithmetic records. Their current duties are:

- sign commit-phase votes for composite records,
- sign finalization votes for prime and composite records,
- sign validator epoch transitions that change the active validator set,
- sign validator endpoint updates,
- sign economic policy updates,
- keep the public endpoint online and synchronized,
- maintain the canonical append-only record store.

A prime record has no composite commit phase, so `commit_phase_votes=0` is expected for prime records. Composite records should include a commit-phase certificate and a finalization certificate.

## Quorum Formula

Validator quorum is formula-based, not hardcoded to two of three:

```text
required_quorum = ceil(2n / 3)
```

The implementation computes this as integer arithmetic equivalent to `(2n + 2) / 3`. For the current three-validator launch network, this requires two votes. For larger validator sets, the same formula applies. The network should never rely on string literals such as `2-of-3` as the authority for current quorum policy; replay and consensus code decide quorum from the active validator set size.

## Validator Admission

A validator candidate becomes active through an on-chain validator epoch transition. The candidate must first satisfy admission checks and then receive quorum approval from the active validator set.

The current admission inputs are:

- a candidate `pcpq1_` validator address,
- a signed validator application containing host, port, sequence, and endpoint observation counts,
- a reserve lock to the candidate reserve address,
- endpoint observation meeting the uptime threshold,
- candidate work score, either direct or sponsored by a work binding,
- an epoch vote quorum from current active validators.

The deterministic reserve address is:

```text
pcreserve_validator_<validator_address>
```

The active validator set change is embedded into the next accepted arithmetic record once the epoch proposal has quorum. Activation happens according to the transition's activation integer.

## Validator Reserve

The launch-testnet minimum reserve is currently:

```text
5,000,000 micro-units
```

The reserve is an economic bond and future slashing basis. It gives validators something at risk and makes validator admission more expensive than ordinary mining. The current chain enforces reserve lock state for admission, but it does not yet automatically slash or forfeit reserve funds for misbehavior.

Reserve forfeiture should not be advertised as implemented until the chain supports explicit evidence records, disable/removal records, and deterministic forfeiture rules.

## Work Score

Validator admission also requires work history. A candidate can satisfy this through its own mining work or through miner sponsorship using validator work bindings.

The work score is derived from replayed chain evidence, including prime/composite mining history and discovery micro-units. The purpose is to make admission depend on visible contribution to the chain, not only on holding reserve.

## Endpoint Updates

Validator network endpoints are on-chain after first contact. A validator signs endpoint updates with:

- previous record hash,
- target record integer,
- host,
- port,
- sequence,
- optional effective integer.

Once embedded in a record, endpoint state is replay-derived. Operators may still configure bootstrap peers for first contact, but chain endpoint records are authoritative for validator peer discovery after startup.

## Economic Policy Voting

Economic policy voting is separate from validator admission. Active validators vote on policy changes. Current policy fields include:

- transfer fee in micro-units,
- validator minimum reserve in micro-units.

A policy update becomes active only when a quorum of active validators signs the same proposal and the proposal is embedded in an accepted arithmetic record. Miners do not vote on economic policy in the current design.

## Mining Reward Split

For validator-set chains, every non-genesis prime asset uses the fixed launch/mainnet split:

```text
45% prime prover
45% composite proof providers
10% validator reward pool
```

If no composite providers contributed between primes, the miner allocation follows the implemented chain rule, but the validator-set chain still reserves the validator share for the epoch reward pool.

The deterministic validator reward pool address is:

```text
pcpool_validator_rewards_epoch_<epoch>
```

Old launch-testnet chains that used earlier reward rules should be treated as disposable evidence chains. Do not upgrade old economics chains in place for mainnet economics; start a clean genesis chain.

## Transaction Fees

The current transfer fee is:

```text
1 micro-unit
```

Transaction fees are credited to the deterministic validator fee-pool address for the active epoch:

```text
pcpool_validator_fees_epoch_<epoch>
```

Fee-pool distribution is separate from validator reward-pool distribution.

## Validator Reward Distribution

Validator reward-pool distribution is deterministic and should not depend on manual discretion. The intended interval is:

```text
1000 primes
```

Distribution should pay eligible active validators according to replay-derived participation evidence. The exact production eligibility formula should be frozen before mainnet-candidate genesis. The conservative policy is:

- only active validators for the relevant epoch are eligible,
- validators with no participation evidence receive no share,
- shares are deterministic from chain data,
- recipient order is canonical by validator address.

## Slashing And Removal Status

Slashing is planned but not yet complete consensus behavior.

Future slashing/removal rules should cover cryptographically provable faults such as:

- double-signing conflicting finalization candidates,
- signing invalid records,
- equivocation across validator epoch or economic policy votes,
- repeated malicious endpoint or peer behavior,
- possibly sustained downtime after an explicit warning/removal process.

Before activating slashing, the protocol needs:

- explicit evidence record formats,
- deterministic evidence verification,
- validator disable/removal records,
- unbonding delay rules,
- deterministic reserve forfeiture rules,
- tests for false-positive resistance.

Until then, downtime and missing signatures are reputation and removal evidence, not automatic reserve loss.

## Launch Position

For launch communication, describe Primechain as:

```text
A controlled reserve-backed validator network with deterministic arithmetic mining, replay-derived validator state, validator fee/reward pools, and planned slashing.
```

Do not describe it yet as:

```text
A permissionless proof-of-stake network with fully implemented slashing.
```
