#include "primechain/storage/commitment_store.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

namespace primechain::storage {
namespace {

constexpr std::uint64_t kCommitmentStoreMagic = 0x3056544d43435055ull;
constexpr std::uint64_t kMaxAddressBytes = 1024;

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
        if (magic != kCommitmentStoreMagic) {
            error = "invalid commitment store magic";
            return {};
        }

        StoredCommitment commitment;
        std::uint64_t address_size = 0;
        if (!readUint64(in, commitment.integer) || !readUint64(in, address_size)) {
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
        commitments.push_back(std::move(commitment));
    }
    return commitments;
}

bool CommitmentStore::replaceAll(
    const std::vector<StoredCommitment>& commitments,
    std::string& error) const {
    const std::string temp_path = path_ + ".tmp";
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
        writeUint64(out, kCommitmentStoreMagic);
        writeUint64(out, commitment.integer);
        writeUint64(out, commitment.provider_address.size());
        out.write(commitment.provider_address.data(), commitment.provider_address.size());
        out.write(
            reinterpret_cast<const char*>(commitment.commitment_hash.data()),
            commitment.commitment_hash.size());
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
    if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
        error = "could not atomically replace commitment store";
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace primechain::storage
