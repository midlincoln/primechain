#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::crypto {

using Bytes = std::vector<std::uint8_t>;

struct Ed25519KeyPair {
    Bytes private_key;
    Bytes public_key;
};

std::optional<Ed25519KeyPair> generateEd25519KeyPair(std::string& error);
std::optional<Bytes> ed25519Sign(
    const Bytes& private_key,
    const Bytes& message,
    std::string& error);
bool ed25519Verify(
    const Bytes& public_key,
    const Bytes& message,
    const Bytes& signature,
    std::string& error);
Address addressFromEd25519PublicKey(const Bytes& public_key);
bool isEd25519Address(const Address& address);

Bytes compositeCommitSigningPayload(
    PrimeValue integer,
    const Hash256& commitment_hash,
    const Address& provider_address);
Bytes compositeRevealSigningPayload(
    PrimeValue integer,
    PrimeValue d,
    PrimeValue e,
    std::uint64_t nonce,
    const Address& provider_address);
Bytes packCompositeRevealProof(
    const Bytes& public_key,
    std::uint64_t nonce,
    const Bytes& signature);
bool verifyPackedCompositeRevealProof(
    PrimeValue integer,
    PrimeValue d,
    PrimeValue e,
    const Address& provider_address,
    const Bytes& packed_proof,
    std::string& error);

} // namespace primechain::crypto
