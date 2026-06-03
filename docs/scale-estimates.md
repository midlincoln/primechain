# Chain Scale Estimates

This note records a basic sanity check for long-running Prime Mining tests.

The active chain height corresponds to a prime index:

```text
height h => frontier approximately p_(h + 1)
```

Using the prime number theorem, the `n`th prime is approximately:

```text
p_n ~= n(log n + log log n - 1)
```

This is not a consensus rule. It is only an estimator for understanding scale.

## Why This Matters

One feasibility objection is that primality certificates for extremely large primes may become expensive. The chain, however, advances by one prime per block. Even after many years of mining, the prime sizes remain small compared with cryptographic-size primes.

For example, after 10 years:

```text
1 block / minute       => about 5.26e6 blocks
1 block / second       => about 3.16e8 blocks
1 block / microsecond  => about 3.16e14 blocks
```

Approximate frontier sizes:

```text
1 block / minute       => around 9e7, about 27 bits
1 block / second       => around 7e9, about 33 bits
1 block / microsecond  => around 1e16, about 54 bits
```

Even the unrealistic microsecond schedule remains far below 1024-bit primes.

## Estimator Tool

Build the project, then run:

```bash
./build/primechain-estimator 10 1
```

Arguments:

- `10`: years
- `1`: blocks per second

Other examples:

```bash
./build/primechain-estimator 10 0.0166666667
./build/primechain-estimator 10 1000000
```

The estimator prints:

- estimated number of blocks,
- estimated prime index,
- estimated frontier prime,
- decimal digits,
- bit length,
- expected prime gap near the frontier.

## Design Implication

For the current testnet, `uint64_t` is enough for realistic and even extremely aggressive time-span simulations. Big integers are still needed later for protocol completeness, but they are not urgent for testing the cooperative architecture.
