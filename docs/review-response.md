# Review Response And Design Clarifications

Dear Christoph, Peter, and members of the list,

Thank you for the detailed and thoughtful feedback.

The draft I shared is an early conceptual exploration, and several of your critiques correctly highlight areas where clarification or revision is needed. Posting here was intentional: to obtain exactly this kind of scrutiny from people with strong backgrounds in number theory and cryptographic protocol design.

Below I address the main technical points raised and outline changes for the next revision.

## 1. Scope Of The Paper

The current version is not intended as a complete cryptocurrency specification.

Its purpose is to explore:

- deterministic prime discovery as a proof-of-work,
- cooperative composite submissions,
- a mathematically indexed asset structure,
- incentive mechanisms inspired loosely by Nash's principles.

A fully cryptographic specification, including hash formats, signatures, block encodings, and adversary models, was not included because the architecture itself is what I hoped to evaluate first.

The next draft will clearly separate:

- conceptual architecture,
- protocol specification components with concrete cryptographic primitives.

## 2. Scaling Of Prime Sizes And Feasibility Of Primality Proofs

Christoph raised concerns about the feasibility of primality certificates for very large primes beyond `10^1000` digits. Some of the numbers cited in the review reflect older benchmarks. Modern ECPP implementations have certified primes significantly larger than 50,000 digits; recent records exceed 100,000 digits, and certificates above 200,000 digits have been constructed in practice. Verification remains extremely fast compared to certificate generation.

It is also important to emphasize that the blockchain will never reach primes anywhere near those sizes.

The index of the first 1024-bit prime is approximately:

```text
pi(2^1024) ~= 10^306
```

Even assuming an unrealistically fast rate of one prime per microsecond, the chain would still require approximately:

```text
10^291 years
```

to reach 1024-bit primes, vastly exceeding the age of the universe. At more realistic block rates, such as seconds or minutes, the gap becomes even more extreme.

Therefore the chain remains permanently in a range where modern primality certificates, such as ECPP or APR-CL, are fast and practical. Verification remains cheap relative to mining.

This will be spelled out more clearly in the next revision.

## 3. Wallet Representation: Sparse Model, Not Global Vectors

Both Christoph and Peter interpreted the wallet state as an ever-growing vector whose dimension equals the number of mined primes. That was an error in the way the draft presented the model.

Section 4.3 included a brief note, but I should have emphasized:

```text
Wallets store only coefficients for primes they actually own.
```

The headline integer `H` encodes which primes appear.

Example:

- If `H = 2 * 3 * 7`,
- the wallet stores coefficients only for `(2, 3, 7)`, for example `(0.4, 0.1, 1.0)`.

Wallets do not hold vectors with entries for all mined primes. They hold a sparse vector aligned to the factorization of their own `H`.

This makes storage compact and scalable. The next draft will make the sparse model the canonical one.

## 4. Composite Proof Front-Running And Gossip Timing

Christoph correctly notes that in an asynchronous gossip network, "first seen" does not reliably define ordering. The paper's treatment of this was incomplete.

The intended mechanism is a commit-reveal protocol:

1. Miner submits `commit = H(m || t || address)`.
2. Commitments are timestamped upon receipt.
3. Later, the miner reveals `m, t`.
4. Only reveals matching prior commitments are valid.
5. The earliest commitment wins.

This prevents front-running and does not require global synchronization. Commit-reveal is widely used in decentralized protocols and adapts cleanly here.

The next draft will specify this protocol in detail.

## 5. Number-Theoretic Domain Extensions

Christoph's criticism about non-UFD rings is valid. Rings like `Z[sqrt(-5)]` do not support unique factorization and therefore cannot sustain a divisibility-based ownership model.

Clarification:

- Mining irreducibles in any ring is possible as a PoW puzzle.
- Ownership accounting based on divisibility requires a UFD or Euclidean domain.
- Therefore, extension chains must be restricted to such rings.
- The main chain is always over `Z`, the canonical UFD.

The earlier version did not sufficiently separate mining puzzles from ledger semantics. This will be corrected.

## 6. Quantum Considerations

Several points are worth distinguishing.

## 6.1 Signatures

ECDSA and RSA fail under Shor's algorithm. The prototype now uses NIST ML-DSA-65 for authenticated protocol identities.

## 6.2 Composite Proofs

Quantum algorithms accelerate factoring, but the protocol does not rely on factoring hardness. Composite proofs only require miners to exhibit one divisor, not to hide factorization.

Quantum ability to find divisors faster simply alters mining difficulty, much like ASICs affect Bitcoin.

## 6.3 Prime Search

Shor's algorithm does not accelerate prime search.

Grover's algorithm provides at most a quadratic speedup, which can be offset through difficulty adjustment.

Conclusion:

```text
The mining layer is far more quantum-resilient than Bitcoin PoW.
Only the signature layer needs a PQ upgrade.
```

This distinction will be clearer in the next draft.

## 7. Incentive Model And Nash Terminology

The current text uses "Nashian" in an informal sense, meaning cooperative stability, not as a formal definition of Nash equilibrium.

Christoph is right that an actual equilibrium analysis requires:

- utility functions,
- strategy spaces,
- deviation analysis,
- explicit proofs.

I will replace "Nashian" with "cooperative incentive structure" unless a formal game-theoretic appendix is added.

On proof withholding: with commit-reveal, the incentive to withhold vanishes because:

- withholding commits delays reward collection,
- simultaneous reveals cannot displace earlier commitments.

I agree that the current incentive description needs a formal treatment, which I will work on.

## 8. Economic Framing

The current paper does not argue that fractional prime ownership is a superior currency.

Its intent is to explore:

- deterministic mathematical issuance,
- provable scarcity,
- cooperative computation,
- a novel ledger structure.

Value attribution, or non-value, is orthogonal to the construction.

The revised draft will make this clearer.

## 9. On Peter's Comments

Peter's concerns about wallet size, factorization storage, and ownership conversion arise from interpreting the system as requiring:

- storage of all factorizations up to `2N`,
- global dense vectors,
- automatic conversion of one prime into another.

None of these are part of the design.

Wallets store only what they own. Prime ownership is independent per asset. No global state explosion occurs.

I appreciate the time taken to comment, and I will revise the exposition to avoid similar misunderstandings in the future.

## Next Steps

I am preparing a significantly revised draft that will:

- clearly separate architecture from protocol specification,
- introduce commit-reveal for composite ordering,
- adopt PQ-safe signature schemes,
- restrict extension rings to UFDs,
- formalize sparse wallet semantics,
- add an explicit adversary model,
- include a more developed incentive theory or remove Nash terminology.

I have also appended a new Section 9 to the draft that compares this proposal with Primecoin and with factor-blockchain designs, clarifying both similarities and substantive differences in proof-of-work philosophy, scalability, and verification cost.

Draft link:

```text
https://midlincoln.com/pdfs/primenumberblockchain.pdf
```

Thank you again for the thoughtful critiques. They have been extremely valuable, and I welcome further comments as the next revision becomes available.

Best regards,

Ovanes Oganisian  
oovanes@midlincoln.com
