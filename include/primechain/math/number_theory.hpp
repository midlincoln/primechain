#pragma once

#include <optional>

#include "primechain/types.hpp"

namespace primechain::math {

bool isPrime(PrimeValue n);
PrimeValue nextPrimeAfter(PrimeValue n);
std::optional<CompositeProof> makeCompositeProof(PrimeValue n, const Address& provider);
bool verifyCompositeProof(const CompositeProof& proof);
bool verifyPrimeCertificate(PrimeValue p, const PrimeCertificate& certificate);

} // namespace primechain::math
