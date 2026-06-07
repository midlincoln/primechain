#include "primechain/crypto/signature.hpp"

#include <algorithm>
#include <memory>
#include <string_view>

#include <openssl/evp.h>

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
    const Hash256 hash = devHash256(public_key);
    return "pc1_" + toHex(hash).substr(0, 40);
}

bool isEd25519Address(const Address& address) {
    constexpr std::string_view prefix = "pc1_";
    if (address.size() != prefix.size() + 40 ||
        address.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    return std::all_of(address.begin() + static_cast<std::ptrdiff_t>(prefix.size()), address.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

Bytes compositeCommitSigningPayload(
    PrimeValue integer,
    const Hash256& commitment_hash,
    const Address& provider_address) {
    Bytes payload;
    appendString(payload, "primechain-composite-commit-signature-v1");
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
    appendString(payload, "primechain-composite-reveal-signature-v1");
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
    appendString(payload, "primechain-commit-phase-vote-v1");
    appendUint64(payload, integer);
    appendHash(payload, snapshot_hash);
    appendString(payload, validator_address);
    return payload;
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
    if (packed_proof.size() != 104) {
        error = "invalid packed composite reveal proof size";
        return false;
    }
    const Bytes public_key(packed_proof.begin(), packed_proof.begin() + 32);
    std::uint64_t nonce = 0;
    for (int i = 0; i < 8; ++i) {
        nonce |= static_cast<std::uint64_t>(packed_proof[32 + i]) << (i * 8);
    }
    const Bytes signature(packed_proof.begin() + 40, packed_proof.end());
    if (provider_address != addressFromEd25519PublicKey(public_key)) {
        error = "composite reveal proof address mismatch";
        return false;
    }
    return ed25519Verify(
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
    std::uint64_t nonce = 0;
    for (int i = 0; i < 8; ++i) {
        nonce |= static_cast<std::uint64_t>(packed_proof[32 + i]) << (i * 8);
    }
    if (developmentCompositeCommitment(integer, d, e, nonce, provider_address) !=
        expected_commitment) {
        error = "composite reveal does not match selected commitment";
        return false;
    }
    return true;
}

} // namespace primechain::crypto
