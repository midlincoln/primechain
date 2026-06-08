#include "primechain/crypto/hash.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <openssl/evp.h>

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

Hash256 sha3_256(const std::vector<std::uint8_t>& bytes) {
    Hash256 out{};
    unsigned int size = 0;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr ||
        EVP_DigestInit_ex(context, EVP_sha3_256(), nullptr) != 1 ||
        EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
        EVP_DigestFinal_ex(context, out.data(), &size) != 1 ||
        size != out.size()) {
        if (context != nullptr) EVP_MD_CTX_free(context);
        throw std::runtime_error("SHA3-256 digest failure");
    }
    EVP_MD_CTX_free(context);
    return out;
}

Hash256 compositeCommitment(
    PrimeValue g,
    PrimeValue d,
    PrimeValue e,
    std::uint64_t nonce,
    const Address& provider_address) {
    std::vector<std::uint8_t> bytes;
    appendString(bytes, "primechain-composite-commit-v1");
    appendUint64(bytes, g);
    appendUint64(bytes, d);
    appendUint64(bytes, e);
    appendUint64(bytes, nonce);
    appendString(bytes, provider_address);
    return sha3_256(bytes);
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
