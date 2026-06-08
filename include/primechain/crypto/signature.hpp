#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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
Bytes commitPhaseVoteSigningPayload(
    PrimeValue integer,
    const Hash256& snapshot_hash,
    const Address& validator_address);
Bytes validatorEpochVoteSigningPayload(
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::uint64_t epoch,
    PrimeValue activation_integer,
    const std::vector<Address>& next_validator_set,
    const Address& validator_address);
Bytes recordFinalizationVoteSigningPayload(
    const Hash256& candidate_hash,
    std::uint64_t round,
    const Address& validator_address);
Bytes transactionSigningPayload(const Bytes& unsigned_transaction);
Bytes primeProofSigningPayload(
    const Hash256& previous_record_hash,
    PrimeValue prime,
    PrimeValue witness,
    const std::vector<std::pair<PrimeValue, std::uint64_t>>& factors,
    const Address& provider_address);
Bytes packPrimeProofAuthentication(
    const Bytes& public_key,
    const Bytes& signature);
bool verifyPackedPrimeProofAuthentication(
    const Hash256& previous_record_hash,
    PrimeValue prime,
    PrimeValue witness,
    const std::vector<std::pair<PrimeValue, std::uint64_t>>& factors,
    const Address& provider_address,
    const Bytes& packed_proof,
    std::string& error);
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
bool packedCompositeRevealMatchesCommitment(
    PrimeValue integer,
    PrimeValue d,
    PrimeValue e,
    const Address& provider_address,
    const Bytes& packed_proof,
    const Hash256& expected_commitment,
    std::string& error);

} // namespace primechain::crypto
