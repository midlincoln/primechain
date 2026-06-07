#include "primechain/storage/phase_store.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

namespace primechain::storage {
namespace {

constexpr std::uint64_t kMagic = 0x3156455341485055ull;
constexpr std::uint64_t kMaxFieldBytes = 1024;

bool readUint64(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        char ch = 0;
        if (!in.get(ch)) return false;
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

bool readBytes(std::istream& in, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::uint64_t size = 0;
    if (!readUint64(in, size) || size > kMaxFieldBytes ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = "invalid phase vote field size";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        error = "truncated phase vote field";
        return false;
    }
    return true;
}

void writeBytes(std::ostream& out, const std::vector<std::uint8_t>& bytes) {
    writeUint64(out, bytes.size());
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

PhaseStore::PhaseStore(std::string path) : path_(std::move(path)) {}

std::vector<CommitPhaseVote> PhaseStore::loadAll(std::string& error) const {
    std::vector<CommitPhaseVote> votes;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return votes;

    while (true) {
        std::uint64_t magic = 0;
        if (!readUint64(in, magic)) {
            if (in.eof()) break;
            error = "truncated phase store magic";
            return {};
        }
        if (magic != kMagic) {
            error = "invalid phase store magic";
            return {};
        }
        CommitPhaseVote vote;
        std::vector<std::uint8_t> address;
        if (!readUint64(in, vote.integer) ||
            !in.read(reinterpret_cast<char*>(vote.snapshot_hash.data()), vote.snapshot_hash.size()) ||
            !readBytes(in, address, error) || !readBytes(in, vote.public_key, error) ||
            !readBytes(in, vote.signature, error)) {
            if (error.empty()) error = "truncated phase vote";
            return {};
        }
        vote.validator_address.assign(address.begin(), address.end());
        votes.push_back(std::move(vote));
    }
    return votes;
}

bool PhaseStore::replaceAll(const std::vector<CommitPhaseVote>& votes, std::string& error) const {
    const std::string temp_path = path_ + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open temporary phase store";
        return false;
    }
    for (const auto& vote : votes) {
        const std::vector<std::uint8_t> address(
            vote.validator_address.begin(), vote.validator_address.end());
        if (address.empty() || address.size() > kMaxFieldBytes ||
            vote.public_key.size() > kMaxFieldBytes || vote.signature.size() > kMaxFieldBytes) {
            error = "invalid phase vote field";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
        writeUint64(out, kMagic);
        writeUint64(out, vote.integer);
        out.write(reinterpret_cast<const char*>(vote.snapshot_hash.data()), vote.snapshot_hash.size());
        writeBytes(out, address);
        writeBytes(out, vote.public_key);
        writeBytes(out, vote.signature);
        if (!out) {
            error = "failed while writing phase store";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
    }
    out.close();
    if (!out) {
        error = "failed while closing phase store";
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
        error = "could not atomically replace phase store";
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace primechain::storage
