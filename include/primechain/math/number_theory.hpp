#pragma once

#include <optional>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::math {

struct PrimePowerFactor {
    PrimeValue prime{0};
    std::uint64_t exponent{0};
};

struct Factorization {
    // Canonical order: strictly increasing prime values.
    std::vector<PrimePowerFactor> factors;
};

class CompositeProofIndex {
public:
    virtual ~CompositeProofIndex() = default;
    virtual std::optional<CompositeProof> findCompositeProof(PrimeValue n) const = 0;
};

bool isPrime(PrimeValue n);
PrimeValue nextPrimeAfter(PrimeValue n);
std::optional<CompositeProof> makeCompositeProof(PrimeValue n, const Address& provider);
bool verifyCompositeProof(const CompositeProof& proof);
bool verifyPrimeCertificate(PrimeValue p, const PrimeCertificate& certificate);
bool isCanonicalFactorization(const Factorization& factorization);
std::optional<PrimeValue> multiplyFactorization(const Factorization& factorization);
std::vector<std::uint8_t> serializeFactorization(const Factorization& factorization);
std::optional<Factorization> factorizeFromProofIndex(
    PrimeValue n,
    const CompositeProofIndex& proofs);

} // namespace primechain::math
