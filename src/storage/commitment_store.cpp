#include "primechain/storage/commitment_store.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

#include "primechain/storage/atomic_file.hpp"

namespace primechain::storage {
namespace {

constexpr std::uint64_t kCommitmentStoreMagicV0 = 0x3056544d43435055ull;
constexpr std::uint64_t kCommitmentStoreMagicV1 = 0x3156544d43435055ull;
constexpr std::uint64_t kCommitmentStoreMagicV2 = 0x3256544d43435055ull;
constexpr std::uint64_t kMaxAddressBytes = 1024;
constexpr std::uint64_t kMaxAuthBytes = 8192;

bool readUint64(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        char ch = 0;
        if (!in.get(ch)) {
            return false;
        }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) << (i * 8);
    }
    return true;
}

void writeUint64(std::ostream& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        const char ch = static_cast<char>((value >> (i * 8)) & 0xffu);
        out.write(&ch, 1);
    }
}

} // namespace

CommitmentStore::CommitmentStore(std::string path)
    : path_(std::move(path)) {}

std::vector<StoredCommitment> CommitmentStore::loadAll(std::string& error) const {
    error.clear();
    if (!detail::prepareAtomicLoad(path_, [](const std::string& candidate, std::string& candidate_error) {
            CommitmentStore candidate_store(candidate);
            candidate_store.loadAll(candidate_error);
            return candidate_error.empty();
        }, error)) return {};
    std::vector<StoredCommitment> commitments;
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return commitments;
    }

    while (true) {
        std::uint64_t magic = 0;
        if (!readUint64(in, magic)) {
            if (in.eof()) {
                break;
            }
            error = "truncated commitment store magic";
            return {};
        }
        if (magic != kCommitmentStoreMagicV0 && magic != kCommitmentStoreMagicV1 &&
            magic != kCommitmentStoreMagicV2) {
            error = "invalid commitment store magic";
            return {};
        }
        const bool authenticated_format = magic == kCommitmentStoreMagicV1 || magic == kCommitmentStoreMagicV2;
        const bool round_format = magic == kCommitmentStoreMagicV2;

        StoredCommitment commitment;
        std::uint64_t address_size = 0;
        if (!readUint64(in, commitment.integer)) {
            error = "truncated commitment header";
            return {};
        }
        if (round_format && !readUint64(in, commitment.commit_round)) {
            error = "truncated commitment round";
            return {};
        }
        if (!readUint64(in, address_size)) {
            error = "truncated commitment header";
            return {};
        }
        if (address_size == 0 || address_size > kMaxAddressBytes ||
            address_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            error = "invalid commitment address size";
            return {};
        }
        commitment.provider_address.resize(static_cast<std::size_t>(address_size));
        if (!in.read(commitment.provider_address.data(), commitment.provider_address.size()) ||
            !in.read(
                reinterpret_cast<char*>(commitment.commitment_hash.data()),
                commitment.commitment_hash.size())) {
            error = "truncated commitment record";
            return {};
        }
        if (authenticated_format) {
            std::uint64_t public_key_size = 0;
            std::uint64_t signature_size = 0;
            if (!readUint64(in, public_key_size) || public_key_size > kMaxAuthBytes) {
                error = "invalid commitment public key size";
                return {};
            }
            commitment.public_key.resize(static_cast<std::size_t>(public_key_size));
            if (!in.read(reinterpret_cast<char*>(commitment.public_key.data()), commitment.public_key.size()) ||
                !readUint64(in, signature_size) || signature_size > kMaxAuthBytes) {
                error = "truncated commitment authentication";
                return {};
            }
            commitment.signature.resize(static_cast<std::size_t>(signature_size));
            if (!in.read(reinterpret_cast<char*>(commitment.signature.data()), commitment.signature.size())) {
                error = "truncated commitment signature";
                return {};
            }
        }
        commitments.push_back(std::move(commitment));
    }
    return commitments;
}

bool CommitmentStore::replaceAll(
    const std::vector<StoredCommitment>& commitments,
    std::string& error) const {
    error.clear();
    const std::string temp_path = detail::uniqueAtomicTempPath(path_);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open temporary commitment store";
        return false;
    }

    for (const auto& commitment : commitments) {
        if (commitment.provider_address.empty() ||
            commitment.provider_address.size() > kMaxAddressBytes) {
            error = "invalid commitment provider address";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
        writeUint64(out, kCommitmentStoreMagicV2);
        writeUint64(out, commitment.integer);
        writeUint64(out, commitment.commit_round);
        writeUint64(out, commitment.provider_address.size());
        out.write(commitment.provider_address.data(), commitment.provider_address.size());
        out.write(
            reinterpret_cast<const char*>(commitment.commitment_hash.data()),
            commitment.commitment_hash.size());
        if (commitment.public_key.size() > kMaxAuthBytes ||
            commitment.signature.size() > kMaxAuthBytes) {
            error = "commitment authentication data too large";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
        writeUint64(out, commitment.public_key.size());
        out.write(
            reinterpret_cast<const char*>(commitment.public_key.data()),
            commitment.public_key.size());
        writeUint64(out, commitment.signature.size());
        out.write(
            reinterpret_cast<const char*>(commitment.signature.data()),
            commitment.signature.size());
        if (!out) {
            error = "failed while writing commitment store";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
    }
    out.close();
    if (!out) {
        error = "failed while closing commitment store";
        std::remove(temp_path.c_str());
        return false;
    }
    if (!detail::commitAtomicTemp(temp_path, path_, "commitment store", error)) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace primechain::storage
