#include <cstdint>
#include <iostream>
#include <string>

#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/wallet/miner_identity.hpp"

namespace {

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " g d e nonce provider_address\n"
              << "  " << argv0 << " sign-commit <miner-identity> g d e nonce\n"
              << "  " << argv0 << " sign-reveal <miner-identity> g d e nonce\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) != "--help") {
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

    if (argc != 7 ||
        (std::string(argv[1]) != "sign-commit" && std::string(argv[1]) != "sign-reveal")) {
        printUsage(argv[0]);
        return 1;
    }

    primechain::wallet::MinerIdentity identity;
    std::string error;
    if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto g = static_cast<primechain::PrimeValue>(std::stoull(argv[3]));
    const auto d = static_cast<primechain::PrimeValue>(std::stoull(argv[4]));
    const auto e = static_cast<primechain::PrimeValue>(std::stoull(argv[5]));
    const auto nonce = static_cast<std::uint64_t>(std::stoull(argv[6]));

    primechain::crypto::Bytes payload;
    primechain::Hash256 commitment{};
    if (std::string(argv[1]) == "sign-commit") {
        commitment = primechain::crypto::developmentCompositeCommitment(
            g, d, e, nonce, identity.address);
        payload = primechain::crypto::compositeCommitSigningPayload(
            g, commitment, identity.address);
    } else {
        payload = primechain::crypto::compositeRevealSigningPayload(
            g, d, e, nonce, identity.address);
    }
    const auto signature = primechain::crypto::ed25519Sign(identity.private_key, payload, error);
    if (!signature.has_value()) {
        std::cerr << error << "\n";
        return 1;
    }

    if (std::string(argv[1]) == "sign-commit") {
        std::cout << "SUBMIT_SIGNED_COMMIT " << g << " "
                  << primechain::crypto::toHex(commitment) << " "
                  << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
    } else {
        std::cout << "SUBMIT_SIGNED_REVEAL " << g << " " << d << " " << e << " "
                  << nonce << " " << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
    }
    return 0;
}
