# Client-Derived Indexes

Primechain clients may maintain derived indexes to answer wallet, address, transaction, reward, and explorer-style queries without replaying the full chain on every command. These indexes are read-model caches only.

Safety rules:

- `chain.dat` is the canonical source of truth.
- `chain.dat.idx` is a rebuildable low-level file-offset index for records inside `chain.dat`.
- Higher-level files under `indexes/` are disposable derived caches.
- Consensus validation, record decoding, signature verification, balances used by replay, validator finality, and reward correctness must not depend on higher-level derived indexes.
- Each derived index checkpoint must include at least the checkpoint integer and the canonical record hash at that integer.
- Before extending an index incrementally, the client must re-read that checkpoint record from `chain.dat` and compare the hash.
- If the checkpoint is missing, malformed, stale, or mismatched, the index must be discarded and rebuilt from `chain.dat`, or the fast command must refuse and ask for an update.
- Fast indexed commands should be testable against the original full-replay command output.

The first derived index is the address index:

- `indexes/address-index.dat` stores one event line per address touched by a transaction output or fee.
- `indexes/address-index.meta` stores the index version, checkpoint integer, checkpoint hash, and trusted event count.
- `update-address-index <workdir>` builds or extends it from the local workdir chain.
- `wallet-history-workdir <workdir> <wallet-file> [--last count]` reads it only when it is caught up to the workdir frontier.

The address index is intentionally not required on validators. It is mainly for wallets, explorers, and reporting clients.
