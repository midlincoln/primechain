#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::crypto {

Hash256 devHash256(const std::vector<std::uint8_t>& bytes);
std::string toHex(const Hash256& hash);

} // namespace primechain::crypto
