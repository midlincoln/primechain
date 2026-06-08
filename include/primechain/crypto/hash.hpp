#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::crypto {

Hash256 sha3_256(const std::vector<std::uint8_t>& bytes);
Hash256 compositeCommitment(
    PrimeValue g,
    PrimeValue d,
    PrimeValue e,
    std::uint64_t nonce,
    const Address& provider_address);
std::string toHex(const Hash256& hash);

} // namespace primechain::crypto
