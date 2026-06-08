#include "primechain/crypto/signature.hpp"

#include <algorithm>
#include <memory>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/rand.h>

extern "C" {
#include "mldsa_native.h"
}

#include "primechain/crypto/hash.hpp"

namespace primechain::crypto {
namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void appendUint64(Bytes& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void appendString(Bytes& out, std::string_view value) {
    appendUint64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void appendHash(Bytes& out, const Hash256& hash) {
    out.insert(out.end(), hash.begin(), hash.end());
}

std::string opensslError(const std::string& operation) {
    return operation + " failed";
}

} // namespace

extern "C" int randombytes(std::uint8_t* out, std::size_t outlen) {
    return RAND_bytes(out, static_cast<int>(outlen)) == 1 ? 0 : -1;
}

std::string signatureAlgorithmName(SignatureAlgorithm algorithm) {
    switch (algorithm) {
        case SignatureAlgorithm::Ed25519: return "ed25519";
        case SignatureAlgorithm::MlDsa65: return "ml-dsa-65";
    }
    return "unknown";
}

std::optional<SignatureAlgorithm> parseSignatureAlgorithm(const std::string& name) {
    if (name == "ed25519") return SignatureAlgorithm::Ed25519;
    if (name == "ml-dsa-65") return SignatureAlgorithm::MlDsa65;
    return std::nullopt;
}

std::size_t signaturePublicKeySize(SignatureAlgorithm algorithm) {
    return algorithm == SignatureAlgorithm::Ed25519 ? 32 : MLDSA65_PUBLICKEYBYTES;
}

std::size_t signaturePrivateKeySize(SignatureAlgorithm algorithm) {
    return algorithm == SignatureAlgorithm::Ed25519 ? 32 : MLDSA65_SECRETKEYBYTES;
}

std::size_t signatureSize(SignatureAlgorithm algorithm) {
    return algorithm == SignatureAlgorithm::Ed25519 ? 64 : MLDSA65_BYTES;
}

std::optional<SignatureKeyPair> generateSignatureKeyPair(
    SignatureAlgorithm algorithm,
    std::string& error) {
    if (algorithm == SignatureAlgorithm::Ed25519) {
        const auto pair = generateEd25519KeyPair(error);
        if (!pair.has_value()) return std::nullopt;
        return SignatureKeyPair{algorithm, pair->private_key, pair->public_key};
    }
    SignatureKeyPair pair;
    pair.algorithm = algorithm;
    pair.private_key.resize(MLDSA65_SECRETKEYBYTES);
    pair.public_key.resize(MLDSA65_PUBLICKEYBYTES);
    if (MLD_API_NAMESPACE(keypair)(pair.public_key.data(), pair.private_key.data()) != 0) {
        error = "ML-DSA-65 key generation failed";
        return std::nullopt;
    }
    return pair;
}

std::optional<Bytes> signMessage(
    SignatureAlgorithm algorithm,
    const Bytes& private_key,
    const Bytes& message,
    std::string& error) {
    if (algorithm == SignatureAlgorithm::Ed25519) {
        return ed25519Sign(private_key, message, error);
    }
    if (private_key.size() != MLDSA65_SECRETKEYBYTES) {
        error = "invalid ML-DSA-65 private key size";
        return std::nullopt;
    }
    Bytes signature(MLDSA65_BYTES);
    std::size_t signature_size = 0;
    if (MLD_API_NAMESPACE(signature)(signature.data(), &signature_size,
            message.data(), message.size(), nullptr, 0, private_key.data()) != 0) {
        error = "ML-DSA-65 signing failed";
        return std::nullopt;
    }
    signature.resize(signature_size);
    return signature;
}

bool verifyMessageSignature(
    SignatureAlgorithm algorithm,
    const Bytes& public_key,
    const Bytes& message,
    const Bytes& signature,
    std::string& error) {
    if (algorithm == SignatureAlgorithm::Ed25519) {
        return ed25519Verify(public_key, message, signature, error);
    }
    if (public_key.size() != MLDSA65_PUBLICKEYBYTES || signature.size() != MLDSA65_BYTES) {
        error = "invalid ML-DSA-65 public key or signature size";
        return false;
    }
    if (MLD_API_NAMESPACE(verify)(signature.data(), signature.size(),
            message.data(), message.size(), nullptr, 0, public_key.data()) != 0) {
        error = "invalid ML-DSA-65 signature";
        return false;
    }
    return true;
}

Address addressFromPublicKey(SignatureAlgorithm algorithm, const Bytes& public_key) {
    const Hash256 hash = sha3_256(public_key);
    const std::string prefix = algorithm == SignatureAlgorithm::Ed25519 ? "pc1_" : "pcpq1_";
    return prefix + toHex(hash).substr(0, 40);
}

bool isAddressForAlgorithm(SignatureAlgorithm algorithm, const Address& address) {
    const std::string_view prefix = algorithm == SignatureAlgorithm::Ed25519 ? "pc1_" : "pcpq1_";
    if (address.size() != prefix.size() + 40 ||
        address.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    return std::all_of(address.begin() + static_cast<std::ptrdiff_t>(prefix.size()), address.end(),
        [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

std::optional<SignatureKeyPair> generateProtocolSignatureKeyPair(std::string& error) {
    return generateSignatureKeyPair(kProtocolSignatureAlgorithm, error);
}

std::optional<Bytes> signProtocolMessage(
    const Bytes& private_key,
    const Bytes& message,
    std::string& error) {
    return signMessage(kProtocolSignatureAlgorithm, private_key, message, error);
}

bool verifyProtocolMessageSignature(
    const Bytes& public_key,
    const Bytes& message,
    const Bytes& signature,
    std::string& error) {
    return verifyMessageSignature(
        kProtocolSignatureAlgorithm, public_key, message, signature, error);
}

Address addressFromProtocolPublicKey(const Bytes& public_key) {
    return addressFromPublicKey(kProtocolSignatureAlgorithm, public_key);
}

bool isProtocolSignatureAddress(const Address& address) {
    return isAddressForAlgorithm(kProtocolSignatureAlgorithm, address);
}

std::optional<Ed25519KeyPair> generateEd25519KeyPair(std::string& error) {
    PkeyCtxPtr context(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0) {
        error = opensslError("Ed25519 key generation initialization");
        return std::nullopt;
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw_key) <= 0) {
        error = opensslError("Ed25519 key generation");
        return std::nullopt;
    }
    PkeyPtr key(raw_key, EVP_PKEY_free);

    Ed25519KeyPair pair;
    pair.private_key.resize(32);
    pair.public_key.resize(32);
    std::size_t private_size = pair.private_key.size();
    std::size_t public_size = pair.public_key.size();
    if (EVP_PKEY_get_raw_private_key(key.get(), pair.private_key.data(), &private_size) <= 0 ||
        EVP_PKEY_get_raw_public_key(key.get(), pair.public_key.data(), &public_size) <= 0) {
        error = opensslError("Ed25519 raw key export");
        return std::nullopt;
    }
    pair.private_key.resize(private_size);
    pair.public_key.resize(public_size);
    return pair;
}

std::optional<Bytes> ed25519Sign(
    const Bytes& private_key,
    const Bytes& message,
    std::string& error) {
    PkeyPtr key(
        EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size()),
        EVP_PKEY_free);
    if (!key) {
        error = opensslError("Ed25519 private-key import");
        return std::nullopt;
    }
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) <= 0) {
        error = opensslError("Ed25519 signing initialization");
        return std::nullopt;
    }
    Bytes signature(64);
    std::size_t signature_size = signature.size();
    if (EVP_DigestSign(
            context.get(),
            signature.data(),
            &signature_size,
            message.data(),
            message.size()) <= 0) {
        error = opensslError("Ed25519 signing");
        return std::nullopt;
    }
    signature.resize(signature_size);
    return signature;
}

bool ed25519Verify(
    const Bytes& public_key,
    const Bytes& message,
    const Bytes& signature,
    std::string& error) {
    PkeyPtr key(
        EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()),
        EVP_PKEY_free);
    if (!key) {
        error = opensslError("Ed25519 public-key import");
        return false;
    }
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) <= 0) {
        error = opensslError("Ed25519 verification initialization");
        return false;
    }
    const int result = EVP_DigestVerify(
        context.get(), signature.data(), signature.size(), message.data(), message.size());
    if (result != 1) {
        error = "invalid Ed25519 signature";
        return false;
    }
    return true;
}

Address addressFromEd25519PublicKey(const Bytes& public_key) {
    return addressFromPublicKey(SignatureAlgorithm::Ed25519, public_key);
}

bool isEd25519Address(const Address& address) {
    return isAddressForAlgorithm(SignatureAlgorithm::Ed25519, address);
}

Bytes compositeCommitSigningPayload(
    PrimeValue integer,
    const Hash256& commitment_hash,
    const Address& provider_address) {
    Bytes payload;
    appendString(payload, "primechain-composite-commit-signature-mldsa65-v2");
    appendUint64(payload, integer);
    appendHash(payload, commitment_hash);
    appendString(payload, provider_address);
    return payload;
}

Bytes compositeRevealSigningPayload(
    PrimeValue integer,
    PrimeValue d,
    PrimeValue e,
    std::uint64_t nonce,
    const Address& provider_address) {
    Bytes payload;
    appendString(payload, "primechain-composite-reveal-signature-mldsa65-v2");
    appendUint64(payload, integer);
    appendUint64(payload, d);
    appendUint64(payload, e);
    appendUint64(payload, nonce);
    appendString(payload, provider_address);
    return payload;
}

Bytes commitPhaseVoteSigningPayload(
    PrimeValue integer,
    const Hash256& snapshot_hash,
    const Address& validator_address) {
    Bytes payload;
    appendString(payload, "primechain-commit-phase-vote-mldsa65-v2");
    appendUint64(payload, integer);
    appendHash(payload, snapshot_hash);
    appendString(payload, validator_address);
    return payload;
}

Bytes validatorEpochVoteSigningPayload(
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::uint64_t epoch,
    PrimeValue activation_integer,
    const std::vector<Address>& next_validator_set,
    const Address& validator_address) {
    Bytes payload;
    appendString(payload, "primechain-validator-epoch-mldsa65-v2");
    appendHash(payload, previous_record_hash);
    appendUint64(payload, record_integer);
    appendUint64(payload, epoch);
    appendUint64(payload, activation_integer);
    appendUint64(payload, next_validator_set.size());
    for (const auto& validator : next_validator_set) appendString(payload, validator);
    appendString(payload, validator_address);
    return payload;
}

Bytes recordFinalizationVoteSigningPayload(
    const Hash256& candidate_hash,
    std::uint64_t round,
    const Address& validator_address) {
    Bytes payload;
    appendString(payload, "primechain-record-finalization-mldsa65-v2");
    appendHash(payload, candidate_hash);
    appendUint64(payload, round);
    appendString(payload, validator_address);
    return payload;
}

Bytes roundChangeVoteSigningPayload(
    const Hash256& previous_record_hash,
    PrimeValue integer,
    std::uint64_t new_round,
    const Address& validator_address) {
    Bytes payload;
    appendString(payload, "primechain-finalization-round-change-mldsa65-v2");
    appendHash(payload, previous_record_hash);
    appendUint64(payload, integer);
    appendUint64(payload, new_round);
    appendString(payload, validator_address);
    return payload;
}

Bytes transactionSigningPayload(const Bytes& unsigned_transaction) {
    Bytes payload;
    appendString(payload, "primechain-transaction-signature-mldsa65-v2");
    appendUint64(payload, unsigned_transaction.size());
    payload.insert(payload.end(), unsigned_transaction.begin(), unsigned_transaction.end());
    return payload;
}

Bytes primeProofSigningPayload(
    const Hash256& previous_record_hash,
    PrimeValue prime,
    PrimeValue witness,
    const std::vector<std::pair<PrimeValue, std::uint64_t>>& factors,
    const Address& provider_address) {
    Bytes payload;
    appendString(payload, "primechain-prime-proof-signature-mldsa65-v2");
    appendHash(payload, previous_record_hash);
    appendUint64(payload, prime);
    appendUint64(payload, witness);
    appendUint64(payload, factors.size());
    for (const auto& factor : factors) {
        appendUint64(payload, factor.first);
        appendUint64(payload, factor.second);
    }
    appendString(payload, provider_address);
    return payload;
}

Bytes packPrimeProofAuthentication(
    const Bytes& public_key,
    const Bytes& signature) {
    Bytes packed;
    packed.reserve(public_key.size() + signature.size());
    packed.insert(packed.end(), public_key.begin(), public_key.end());
    packed.insert(packed.end(), signature.begin(), signature.end());
    return packed;
}

bool verifyPackedPrimeProofAuthentication(
    const Hash256& previous_record_hash,
    PrimeValue prime,
    PrimeValue witness,
    const std::vector<std::pair<PrimeValue, std::uint64_t>>& factors,
    const Address& provider_address,
    const Bytes& packed_proof,
    std::string& error) {
    const std::size_t public_key_size = signaturePublicKeySize(kProtocolSignatureAlgorithm);
    const std::size_t signature_size = signatureSize(kProtocolSignatureAlgorithm);
    if (packed_proof.size() != public_key_size + signature_size) {
        error = "invalid packed prime proof authentication size";
        return false;
    }
    const Bytes public_key(packed_proof.begin(), packed_proof.begin() + public_key_size);
    const Bytes signature(packed_proof.begin() + public_key_size, packed_proof.end());
    if (provider_address != addressFromProtocolPublicKey(public_key)) {
        error = "prime proof provider address mismatch";
        return false;
    }
    return verifyProtocolMessageSignature(
        public_key,
        primeProofSigningPayload(
            previous_record_hash, prime, witness, factors, provider_address),
        signature,
        error);
}

Bytes packCompositeRevealProof(
    const Bytes& public_key,
    std::uint64_t nonce,
    const Bytes& signature) {
    Bytes packed;
    packed.reserve(public_key.size() + 8 + signature.size());
    packed.insert(packed.end(), public_key.begin(), public_key.end());
    appendUint64(packed, nonce);
    packed.insert(packed.end(), signature.begin(), signature.end());
    return packed;
}

bool verifyPackedCompositeRevealProof(
    PrimeValue integer,
    PrimeValue d,
    PrimeValue e,
    const Address& provider_address,
    const Bytes& packed_proof,
    std::string& error) {
    const std::size_t public_key_size = signaturePublicKeySize(kProtocolSignatureAlgorithm);
    const std::size_t signature_size = signatureSize(kProtocolSignatureAlgorithm);
    if (packed_proof.size() != public_key_size + 8 + signature_size) {
        error = "invalid packed composite reveal proof size";
        return false;
    }
    const Bytes public_key(packed_proof.begin(), packed_proof.begin() + public_key_size);
    std::uint64_t nonce = 0;
    for (int i = 0; i < 8; ++i) {
        nonce |= static_cast<std::uint64_t>(packed_proof[public_key_size + i]) << (i * 8);
    }
    const Bytes signature(packed_proof.begin() + public_key_size + 8, packed_proof.end());
    if (provider_address != addressFromProtocolPublicKey(public_key)) {
        error = "composite reveal proof address mismatch";
        return false;
    }
    return verifyProtocolMessageSignature(
        public_key,
        compositeRevealSigningPayload(integer, d, e, nonce, provider_address),
        signature,
        error);
}

bool packedCompositeRevealMatchesCommitment(
    PrimeValue integer,
    PrimeValue d,
    PrimeValue e,
    const Address& provider_address,
    const Bytes& packed_proof,
    const Hash256& expected_commitment,
    std::string& error) {
    if (!verifyPackedCompositeRevealProof(
            integer, d, e, provider_address, packed_proof, error)) {
        return false;
    }
    const std::size_t public_key_size = signaturePublicKeySize(kProtocolSignatureAlgorithm);
    std::uint64_t nonce = 0;
    for (int i = 0; i < 8; ++i) {
        nonce |= static_cast<std::uint64_t>(packed_proof[public_key_size + i]) << (i * 8);
    }
    if (compositeCommitment(integer, d, e, nonce, provider_address) !=
        expected_commitment) {
        error = "composite reveal does not match selected commitment";
        return false;
    }
    return true;
}

} // namespace primechain::crypto
