# primescan (draft)

Status: **early draft, no code yet**. This is a placeholder design note, opened now so the direction is visible before implementation starts. Everything below is subject to change, including the name.

## What it is

A block/transaction explorer for primechain, plus a link from primewallet's Activity view straight to a transaction's or record's explorer page (`activity row -> primescan detail view`).

## Why it doesn't exist yet

Explorers are read-heavy: every page view is effectively a lookup over the whole chain's history (an address's transactions, a record's neighbors, running totals). Building that against `loadAll()`-style full replay would hit the same wall primewallet already hit -- see [`client-derived-indexes.md`](client-derived-indexes.md) and PR #10. So this was deliberately held until that indexing groundwork was in place upstream, rather than duplicating the same full-replay cost in a second tool.

## Planned shape (subject to change)

- **Backend**: a long-running process that keeps a workdir synced (`sync-peer` on an interval) and keeps the derived indexes from PR #10 current (`update-address-index`, `update-reward-index`, `update-participation-index`), then serves an HTTP API by shelling out to (or eventually linking against) the `-workdir`/`-fast` primechain-client commands -- `record`, `tx`, `address-report-workdir`, `wallet-history-workdir`, `board-report-workdir`, `launch-report-workdir`, etc. No new chain-reading logic; it's a thin serving layer over what the client already exposes.
- **Frontend**: dashboard (frontier, latest records), record detail page, address/transaction detail page, validator/board summary pages.
- **primewallet integration**: an Activity-row link to the matching primescan detail page. This will *not* be wired up or shipped until primescan itself is usable -- no dead links in the meantime.

## Open questions

- Language/runtime for the backend (leaning toward reusing `primechain-client`'s output directly rather than a separate reimplementation, to avoid a second place where record parsing can drift).
- Hosting (dev-only vs. something reachable by others).
- How much of this should live in this repo vs. a separate `primescan` repo.

## Dependencies

- PR #10 (client-derived indexes) landing on `main` -- primescan's backend is meant to be built straight on top of the commands it adds, not a parallel implementation.
