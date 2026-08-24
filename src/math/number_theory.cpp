#include "primechain/math/number_theory.hpp"

#include <cmath>
#include <map>
#include <limits>
#include <vector>

namespace primechain::math {

namespace {

bool addFactor(
    PrimeValue n,
    const CompositeProofIndex& proofs,
    std::map<PrimeValue, std::uint64_t>& factors) {
    if (n < 2) {
        return false;
    }
    if (isPrime(n)) {
        ++factors[n];
        return true;
    }

    const auto proof = proofs.findCompositeProof(n);
    if (!proof.has_value() || !verifyCompositeProof(*proof) || proof->m != n) {
        return false;
    }

    return addFactor(proof->d, proofs, factors) && addFactor(proof->e, proofs, factors);
}

PrimeValue gcd(PrimeValue a, PrimeValue b) {
    while (b != 0) {
        const PrimeValue r = a % b;
        a = b;
        b = r;
    }
    return a;
}

PrimeValue mulMod(PrimeValue a, PrimeValue b, PrimeValue mod) {
    // The previous implementation computed this via repeated
    // double-and-add (result+a, a+a, each reduced mod `mod`) specifically
    // to avoid an overflowing a*b -- but that reasoning only holds when
    // mod <= 2^63: once mod exceeds that, a and result can each already be
    // up to mod-1, so `a + a` or `result + a` can reach up to ~2*mod,
    // which overflows uint64_t and silently wraps for any mod > 2^63.
    // Confirmed live: isPrime()'s Miller-Rabin now calls this for
    // candidates across the *entire* uint64_t range (unlike the old
    // Pratt-proof-only caller, which in practice never exercised primes
    // above 2^63), and an oracle cross-check caught exactly this --
    // correct results below 2^63, silent false-composite results above
    // it. __int128 sidesteps the whole problem: the true 128-bit product
    // never overflows, so a single multiply+mod is both correct for the
    // full range and considerably faster than the old bit-by-bit loop.
    return static_cast<PrimeValue>(
        (static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) % mod);
}

PrimeValue powMod(PrimeValue base, PrimeValue exponent, PrimeValue mod) {
    PrimeValue result = 1 % mod;
    base %= mod;
    while (exponent > 0) {
        if ((exponent & 1u) != 0) {
            result = mulMod(result, base, mod);
        }
        exponent >>= 1u;
        if (exponent > 0) {
            base = mulMod(base, base, mod);
        }
    }
    return result;
}

} // namespace

void appendUint64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

bool isPrime(PrimeValue n) {
    if (n < 2) {
        return false;
    }

    // Trial-divide against the small primes used as Miller-Rabin witnesses
    // below: cheaply resolves n as prime when it equals one of them, and
    // rejects most composites (including all even n) before paying for any
    // modular exponentiation.
    constexpr PrimeValue kWitnesses[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (const PrimeValue p : kWitnesses) {
        if (n == p) {
            return true;
        }
        if (n % p == 0) {
            return false;
        }
    }

    // Deterministic Miller-Rabin over witnesses {2,3,5,7,11,13,17,19,23,29,
    // 31,37}: a proven-correct (zero false positives, not merely
    // probabilistic) primality test for every n < 3,317,044,064,679,887,
    // 385,961,981 -- which safely covers PrimeValue's entire uint64_t
    // range. Replaces the O(sqrt(n)) trial division that used to run all
    // the way up here: primality is a mathematical fact independent of
    // which correct test decides it, so this cannot disagree with the old
    // algorithm on any input, it just gets there in O(log^3 n) instead of
    // O(sqrt(n)) -- the difference between a candidate near 2^63 costing
    // roughly 3 billion divisions versus a few dozen modular
    // multiplications. mulMod/powMod below are the same helpers already
    // used elsewhere in this file (e.g. verifyPrattProof).
    PrimeValue d = n - 1;
    int r = 0;
    while ((d & 1u) == 0) {
        d >>= 1u;
        ++r;
    }

    for (const PrimeValue a : kWitnesses) {
        PrimeValue x = powMod(a, d, n);
        if (x == 1 || x == n - 1) {
            continue;
        }
        bool possibly_composite = true;
        for (int i = 0; i < r - 1; ++i) {
            x = mulMod(x, x, n);
            if (x == n - 1) {
                possibly_composite = false;
                break;
            }
        }
        if (possibly_composite) {
            return false;
        }
    }
    return true;
}

PrimeValue nextPrimeAfter(PrimeValue n) {
    if (n < 2) {
        return 2;
    }

    PrimeValue candidate = n + 1;
    while (candidate < std::numeric_limits<PrimeValue>::max()) {
        if (isPrime(candidate)) {
            return candidate;
        }
        ++candidate;
    }
    return 0;
}

std::optional<CompositeProof> makeCompositeProof(PrimeValue n, const Address& provider) {
    if (n < 4 || isPrime(n)) {
        return std::nullopt;
    }

    for (PrimeValue d = 2; d <= n / d; ++d) {
        if (n % d == 0) {
            CompositeProof proof;
            proof.m = n;
            proof.d = d;
            proof.e = n / d;
            proof.provider_address = provider;
            return proof;
        }
    }
    return std::nullopt;
}

bool verifyCompositeProof(const CompositeProof& proof) {
    if (proof.m < 4 || proof.d <= 1 || proof.e <= 1) {
        return false;
    }
    if (proof.d >= proof.m || proof.e >= proof.m) {
        return false;
    }
    return proof.d <= proof.m / proof.e && proof.d * proof.e == proof.m;
}

bool verifyPrimeCertificate(PrimeValue p, const PrimeCertificate& certificate) {
    (void)certificate;
    return isPrime(p);
}

bool isCanonicalFactorization(const Factorization& factorization) {
    PrimeValue previous = 0;
    for (const auto& factor : factorization.factors) {
        if (factor.prime < 2 || factor.exponent == 0) {
            return false;
        }
        if (!isPrime(factor.prime)) {
            return false;
        }
        if (previous != 0 && factor.prime <= previous) {
            return false;
        }
        previous = factor.prime;
    }
    return true;
}

std::optional<PrimeValue> multiplyFactorization(const Factorization& factorization) {
    if (!isCanonicalFactorization(factorization)) {
        return std::nullopt;
    }

    PrimeValue product = 1;
    for (const auto& factor : factorization.factors) {
        for (std::uint64_t i = 0; i < factor.exponent; ++i) {
            if (product > std::numeric_limits<PrimeValue>::max() / factor.prime) {
                return std::nullopt;
            }
            product *= factor.prime;
        }
    }
    return product;
}

std::vector<std::uint8_t> serializeFactorization(const Factorization& factorization) {
    std::vector<std::uint8_t> out;
    appendUint64(out, factorization.factors.size());
    for (const auto& factor : factorization.factors) {
        appendUint64(out, factor.prime);
        appendUint64(out, factor.exponent);
    }
    return out;
}

bool verifyPrattProof(const PrattProof& proof) {
    if (proof.p == 2) {
        return proof.witness == 0 && proof.factors_of_p_minus_1.factors.empty();
    }
    if (proof.p < 2 || proof.witness <= 1 || proof.witness >= proof.p) {
        return false;
    }

    if (!isCanonicalFactorization(proof.factors_of_p_minus_1)) {
        return false;
    }

    const auto product = multiplyFactorization(proof.factors_of_p_minus_1);
    if (!product.has_value() || *product != proof.p - 1) {
        return false;
    }

    if (powMod(proof.witness, proof.p - 1, proof.p) != 1) {
        return false;
    }

    for (const auto& factor : proof.factors_of_p_minus_1.factors) {
        const PrimeValue exponent = (proof.p - 1) / factor.prime;
        const PrimeValue residue = powMod(proof.witness, exponent, proof.p);
        const PrimeValue diff = residue == 0 ? proof.p - 1 : residue - 1;
        if (gcd(diff, proof.p) != 1) {
            return false;
        }
    }

    return true;
}

std::optional<PrattProof> makePrattProof(
    PrimeValue p,
    const CompositeProofIndex& proofs) {
    if (p == 2) {
        PrattProof proof;
        proof.p = 2;
        proof.witness = 0;
        return proof;
    }
    if (p < 2 || !isPrime(p)) {
        return std::nullopt;
    }

    const auto factorization = factorizeFromProofIndex(p - 1, proofs);
    if (!factorization.has_value()) {
        return std::nullopt;
    }

    for (PrimeValue witness = 2; witness < p; ++witness) {
        PrattProof proof;
        proof.p = p;
        proof.witness = witness;
        proof.factors_of_p_minus_1 = *factorization;
        if (verifyPrattProof(proof)) {
            return proof;
        }
    }

    return std::nullopt;
}

std::optional<Factorization> factorizeFromProofIndex(
    PrimeValue n,
    const CompositeProofIndex& proofs) {
    if (n < 2) {
        return std::nullopt;
    }

    std::map<PrimeValue, std::uint64_t> factor_counts;
    if (!addFactor(n, proofs, factor_counts)) {
        return std::nullopt;
    }

    Factorization out;
    for (const auto& [prime, exponent] : factor_counts) {
        out.factors.push_back({prime, exponent});
    }
    if (!isCanonicalFactorization(out)) {
        return std::nullopt;
    }
    return out;
}

} // namespace primechain::math
