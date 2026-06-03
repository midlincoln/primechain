# Testnet Plan

This note records the current thinking about how Primechain should move from local testing to a public cooperative mining testnet.

## Current State

The prototype can already run a local TCP node and a terminal miner:

```text
terminal miner -> TCP -> node -> consensus validation -> in-memory chain state
```

This proves the first local loop:

- node starts from genesis frontier prime `2`,
- miner asks for the current tip,
- miner builds the next-prime block,
- node validates the block,
- chain advances in memory.

This is useful, but it is not yet a real public testnet.

## How Many Nodes Are Needed?

A serious public test does not need hundreds of nodes at first. It needs enough independent machines to prove that the protocol works outside one process or one computer.

## Stage 1: Local Test

Recommended setup:

```text
1 node
1-3 miners
same PC or same server
```

Goal:

- verify mining,
- verify block submission,
- verify chain evolution,
- verify local TCP behavior.

This stage is already partially working.

## Stage 2: Private Multi-Machine Test

Recommended setup:

```text
3 nodes
3-10 miners
```

Possible machines:

- development server,
- local PC,
- one VPS or trusted collaborator machine.

Goal:

- test networking outside localhost,
- test propagation behavior,
- test rejected blocks,
- test latency,
- test node restarts,
- test basic operational reliability.

## Stage 3: Small Public Testnet

Recommended setup:

```text
5-10 stable nodes
10-50 miners
```

This is enough for an early public testnet. The first public test does not need large decentralization. It needs reproducible setup, clear instructions, and stable behavior.

## Minimum Before Inviting Outside Miners

Before asking outside miners to join, the project should have:

- at least 3 always-on nodes,
- at least 1 public bootstrap node,
- persistent chain storage,
- cooperative proof pool,
- node restart recovery,
- basic sync or bootstrap behavior,
- clear miner instructions,
- readable logs,
- clear testnet reset policy,
- clear warning that the system is experimental and has no real monetary value.

## Current Blockers For Public Miners

The current prototype is not ready for outside miners because:

- chain state is not persistent,
- node only stores state in memory,
- cooperative proof pool is not implemented,
- multi-node sync is not implemented,
- malformed input handling is minimal,
- no commit-reveal exists yet,
- no wallet or reward accounting exists yet.

## Practical Sequence

Recommended build order:

1. Keep testing `1 node + 1 miner` locally.
2. Add cooperative composite proof pool.
3. Add separate composite miner executable.
4. Add persistent chain log.
5. Add node restart recovery.
6. Run `1 server node + local PC miner`.
7. Run 3 controlled nodes.
8. Add basic sync/bootstrap behavior.
9. Invite a small group of outside miners.

## Design Principle

The first public testnet should be small and honest about its limits. The goal is not immediate scale. The goal is to prove that independent participants can run nodes and miners, submit useful composite work, and observe the deterministic prime chain evolve.
