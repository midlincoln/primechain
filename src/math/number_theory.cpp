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
    if (n == 2 || n == 3) {
        return true;
    }
    if (n % 2 == 0 || n % 3 == 0) {
        return false;
    }

    for (PrimeValue d = 5; d <= n / d; d += 6) {
        if (n % d == 0 || n % (d + 2) == 0) {
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
