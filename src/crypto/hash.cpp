#include "primechain/crypto/hash.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace primechain::crypto {
namespace {

void appendUint64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void appendString(std::vector<std::uint8_t>& out, std::string_view value) {
    appendUint64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

} // namespace

Hash256 devHash256(const std::vector<std::uint8_t>& bytes) {
    // Development-only deterministic hash. Replace with SHA3-256 before testnet.
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    std::array<std::uint64_t, 4> lanes{
        kOffset,
        kOffset ^ 0x9e3779b97f4a7c15ull,
        kOffset ^ 0xbf58476d1ce4e5b9ull,
        kOffset ^ 0x94d049bb133111ebull,
    };

    for (std::uint8_t byte : bytes) {
        for (std::uint64_t& lane : lanes) {
            lane ^= byte;
            lane *= kPrime;
            lane ^= lane >> 32;
        }
    }

    Hash256 out{};
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        std::uint64_t lane = lanes[i];
        for (std::size_t j = 0; j < 8; ++j) {
            out[(i * 8) + j] = static_cast<std::uint8_t>((lane >> (j * 8)) & 0xffu);
        }
    }
    return out;
}

Hash256 developmentCompositeCommitment(
    PrimeValue g,
    PrimeValue d,
    PrimeValue e,
    std::uint64_t nonce,
    const Address& provider_address) {
    std::vector<std::uint8_t> bytes;
    appendString(bytes, "primechain-composite-commit-v0");
    appendUint64(bytes, g);
    appendUint64(bytes, d);
    appendUint64(bytes, e);
    appendUint64(bytes, nonce);
    appendString(bytes, provider_address);
    return devHash256(bytes);
}

std::string toHex(const Hash256& hash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint8_t byte : hash) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

} // namespace primechain::crypto
