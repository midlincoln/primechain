#pragma once

#include <optional>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::math {

struct PrimePowerFactor {
    PrimeValue prime{0};
    std::uint64_t exponent{0};
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
std::optional<std::vector<PrimePowerFactor>> factorizeFromProofIndex(
    PrimeValue n,
    const CompositeProofIndex& proofs);

} // namespace primechain::math
