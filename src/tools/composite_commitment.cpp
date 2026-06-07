#include <cstdint>
#include <iostream>
#include <string>

#include "primechain/crypto/hash.hpp"

namespace {

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " g d e nonce provider_address\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6 || std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 1;
    }

    const auto g = static_cast<primechain::PrimeValue>(std::stoull(argv[1]));
    const auto d = static_cast<primechain::PrimeValue>(std::stoull(argv[2]));
    const auto e = static_cast<primechain::PrimeValue>(std::stoull(argv[3]));
    const auto nonce = static_cast<std::uint64_t>(std::stoull(argv[4]));
    const std::string provider = argv[5];

    std::cout << primechain::crypto::toHex(
        primechain::crypto::developmentCompositeCommitment(g, d, e, nonce, provider))
              << "\n";
    return 0;
}
