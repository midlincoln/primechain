# Primechain Validator Economy v0

This document records the emerging validator-economy design. It is a design
target, not current consensus behavior.

Primechain separates arithmetic discovery from settlement finality:

- miners and clients discover arithmetic facts,
- validators provide finality, coordination, availability, and client access.

That split means discovery rewards and validator rewards should be priced
separately. The design goal is a reserve-backed validator system where
validators are financial institutions with capital at risk, while public miners
still have a meaningful reason to discover prime and composite records.

## Roles

### Miners And Clients

Miners produce arithmetic evidence:

- Pratt certificates for primes,
- divisor/factor evidence for composites,
- commit-reveal participation for composite rewards.

Miners earn discovery rewards. Mining history is an address-bound reputation
input; it is not a transferable asset.

### Validators

Validators are settlement institutions. They:

- run public node endpoints,
- accept client commitments, reveals, transactions, and proof submissions,
- coordinate with other validators,
- sign commit-phase, timeout, round-change, finalization, epoch, and policy
  events,
- serve chain sync and status queries,
- maintain reserve capital locked by protocol rules.

Validators earn finality rewards for active participation.

## Validator Wallet Model

A validator operator should not use one key for every purpose.

Recommended wallet roles:

```text
validator signing wallet  = hot consensus key for finalization and votes
validator reserve wallet  = locked capital/stake, cold or semi-cold
validator income wallet   = spendable validator rewards
optional admin wallet     = endpoint, metadata, and governance administration
```

Validator rewards should be paid to the income wallet. Locked reserves should
not receive routine spendable income unless the operator explicitly locks more
funds.

Validator registration should eventually include:

```text
validator_signing_address
reserve_address
income_address
operator_metadata_hash
endpoint
```

The signing key may live on a server. The reserve wallet should not.

## Reserves

Validator reserves are locked balances that establish economic commitment.

Rules:

- reserve funds cannot be transferred while locked,
- reserve funds count toward validator eligibility,
- reserve unlock requires resignation plus an unbonding delay,
- reserve funds may be slashed for provable misbehavior once slashing exists.

Primechain currently tracks balances by `(address, prime_asset)`. A first
reserve implementation can define reserve value as the sum of locked
micro-units across eligible prime assets. A later implementation may define a
more precise valuation rule.

## Validator Reputation

Reputation should be replay-derived chain state, not an informal off-chain
database.

Useful reputation inputs:

- prime records mined,
- composite records mined,
- discovery rewards earned,
- locked reserve amount and duration,
- finalization votes signed,
- missed signing opportunities,
- timeout and round-change participation,
- policy and admission votes cast,
- slashing, equivocation, or removal events.

Initial implementation should report these metrics explicitly instead of
collapsing them into one opaque score.

Example future command:

```text
primechain-client validator-reputation <record-store> <address>
```

Mining history is non-transferable:

```text
work_score(address) cannot be transferred
```

The protocol should not allow a rich buyer to simply purchase an old qualified
miner wallet and inherit validator reputation as if they had earned it. A
qualified miner may later endorse or sponsor a validator candidate, but that
must be explicit, public, delayed, and revocable by rule.

## Admission And Removal

A validator candidate should eventually satisfy:

```text
own_work_score >= minimum
locked_reserve >= minimum
observer_uptime >= minimum
admission_vote >= threshold
```

Admission should use a higher threshold than ordinary finalization. Admission
changes affect future settlement power and should activate only after a delay.

Suggested event family:

```text
VALIDATOR_APPLY
VALIDATOR_RESERVE_LOCK
VALIDATOR_OBSERVER_ATTESTATION
VALIDATOR_ADMISSION_VOTE
VALIDATOR_ACTIVATE
VALIDATOR_RESIGN
VALIDATOR_REMOVE
```

Voluntary resignation may be submitted any time, but reserve withdrawal should
wait through an unbonding delay. Emergency exclusion should require a
supermajority and an evidence hash.

Early governance rule:

```text
one disclosed operator, one validator
```

This is a governance rule, not a cryptographic proof of independence. It should
be stated honestly.

## Board Meetings And Policy Epochs

Validator governance should happen on a schedule, not continuously.

The replayable version of a board meeting is a policy epoch review. Use
frontier-integer ranges rather than trusted wall-clock time:

```text
report covers:         integer A through integer B
meeting integer:       B
vote window:           B through C
activation integer:    D
```

For testnet, policy epochs can be short, for example every 500 or 1000
integers. A later production network can map epoch length to an approximate
monthly cadence.

Board meetings should review:

- total records mined,
- prime/composite counts,
- unique mining addresses,
- reward distribution,
- average reward per address,
- validator signing counts,
- missed signing duties,
- locked reserves,
- observed client counts,
- endpoint and uptime reports,
- estimated mining cost observations.

Some values are consensus facts replayed from records. Others, such as observed
client counts or electricity-cost estimates, are validator-reported metrics and
must be labeled as such.

## Economic Policy

Validators may vote on economic parameters, but only within hard protocol
bounds.

Policy parameters may include:

```text
miner_reward_bps
validator_reward_bps
treasury_reward_bps
min_validator_reserve
min_validator_work_score
unbonding_delay
admission_threshold
slashing_penalty
```

Basis points should sum exactly:

```text
miner_reward_bps + validator_reward_bps + treasury_reward_bps = 10000
```

Initial target parameters for testing:

```text
miner_reward_bps     = 4000
validator_reward_bps = 5000
treasury_reward_bps  = 1000
```

Hard bounds should prevent validator self-dealing. Example bounds:

```text
miner_reward_bps     >= 2500
validator_reward_bps <= 6500
treasury_reward_bps  <= 1500
reserve changes per policy epoch are capped
```

Policy changes should use delayed activation:

```text
approved at policy epoch N
active at policy epoch N+1 or later
```

## Reward Model

Primechain should pay two different goods:

```text
arithmetic discovery -> miners/providers
settlement finality  -> validator signers
```

A first implementation can split each newly minted prime asset:

```text
discovery pool = miner_reward_bps
validator pool = validator_reward_bps
treasury pool = treasury_reward_bps
```

Discovery pool can reuse the existing prime/composite reward allocation logic.
Validator pool should initially split equally among validators who signed the
finalized record. Later versions may add reserve-weighted components, but equal
payment to active signers is simpler and less cartel-prone.

Validators who do not sign should not receive that record's validator reward.

## Transfer And Wallet Prerequisite

Reserve governance should not be built on plaintext operational wallets.

Before reserve locking becomes consensus behavior, Primechain should add:

- encrypted/passphrase-protected protocol wallets,
- clean transfer commands in `primechain-client`,
- explicit mining, signing, reserve, income, and treasury wallet roles,
- a public transfer demo with before/after balances.

Because the current public testnet is experimental, a future reserve/governance
testnet should start from a fresh genesis after wallet and transfer behavior is
stable.

## Implementation Order

Recommended order:

1. Document validator economy and governance.
2. Add encrypted wallets and clean transfer UX.
3. Add policy/reward state with fixed default parameters.
4. Split rewards into discovery, validator, and treasury pools.
5. Add economy and validator reputation report commands.
6. Add policy epoch event records and validator votes.
7. Add reserve locking and unbonding records.
8. Add validator admission/removal records.
9. Add validator-driven finalization so miners no longer assemble quorum.

The design should remain honest: this is a reserve-backed federation model
until validator admission and Sybil resistance are formalized and tested.
