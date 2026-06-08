#include <iostream>
#include <string>

#include "primechain/crypto/signature.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

} // namespace

int main() {
    using namespace primechain::crypto;
    const Bytes message{'p', 'r', 'i', 'm', 'e', 'c', 'h', 'a', 'i', 'n'};

    for (const auto algorithm : {SignatureAlgorithm::Ed25519, SignatureAlgorithm::MlDsa65}) {
        std::string error;
        const auto pair = generateSignatureKeyPair(algorithm, error);
        if (!expect(pair.has_value(), "generate signature key pair")) return 1;
        if (!expect(pair->public_key.size() == signaturePublicKeySize(algorithm), "public key size")) return 1;
        if (!expect(pair->private_key.size() == signaturePrivateKeySize(algorithm), "private key size")) return 1;

        const auto signature = signMessage(algorithm, pair->private_key, message, error);
        if (!expect(signature.has_value(), "sign message")) return 1;
        if (!expect(signature->size() == signatureSize(algorithm), "signature size")) return 1;
        if (!expect(verifyMessageSignature(
                algorithm, pair->public_key, message, *signature, error), "verify signature")) return 1;

        auto tampered = message;
        tampered[0] ^= 1;
        error.clear();
        if (!expect(!verifyMessageSignature(
                algorithm, pair->public_key, tampered, *signature, error), "reject tampered message")) return 1;

        const auto address = addressFromPublicKey(algorithm, pair->public_key);
        if (!expect(isAddressForAlgorithm(algorithm, address), "algorithm address")) return 1;
        if (!expect(parseSignatureAlgorithm(signatureAlgorithmName(algorithm)) == algorithm,
                "algorithm name round trip")) return 1;
    }

    std::cout << "signature tests passed\n";
    return 0;
}
