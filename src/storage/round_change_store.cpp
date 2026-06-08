#include "primechain/storage/round_change_store.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

#include "primechain/storage/atomic_file.hpp"

namespace primechain::storage {
namespace {
constexpr std::uint64_t kMagic = 0x3152474e48434350ull;
constexpr std::uint64_t kMaxFieldBytes = 8192;

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
bool readBytes(std::istream& in, std::vector<std::uint8_t>& bytes) {
    std::uint64_t size = 0;
    if (!readUint64(in, size) || size > kMaxFieldBytes ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
    bytes.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
}
void writeBytes(std::ostream& out, const std::vector<std::uint8_t>& bytes) {
    writeUint64(out, bytes.size());
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
bool readAddress(std::istream& in, Address& address) {
    std::vector<std::uint8_t> bytes;
    if (!readBytes(in, bytes)) return false;
    address.assign(bytes.begin(), bytes.end());
    return !address.empty();
}
void writeAddress(std::ostream& out, const Address& address) {
    writeBytes(out, std::vector<std::uint8_t>(address.begin(), address.end()));
}
} // namespace

RoundChangeStore::RoundChangeStore(std::string path) : path_(std::move(path)) {}

std::vector<protocol::RoundChangeVoteV1> RoundChangeStore::loadAll(std::string& error) const {
    error.clear();
    if (!detail::prepareAtomicLoad(path_, [](const std::string& candidate, std::string& candidate_error) {
            RoundChangeStore candidate_store(candidate);
            candidate_store.loadAll(candidate_error);
            return candidate_error.empty();
        }, error)) return {};
    std::vector<protocol::RoundChangeVoteV1> votes;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return votes;
    while (true) {
        std::uint64_t magic = 0;
        if (!readUint64(in, magic)) {
            if (in.eof()) break;
            error = "truncated round-change store";
            return {};
        }
        protocol::RoundChangeVoteV1 vote;
        if (magic != kMagic || !readAddress(in, vote.validator_address) ||
            !readBytes(in, vote.public_key) ||
            !in.read(reinterpret_cast<char*>(vote.previous_record_hash.data()), vote.previous_record_hash.size()) ||
            !readUint64(in, vote.integer) || !readUint64(in, vote.new_round) ||
            !readBytes(in, vote.signature)) {
            error = "invalid round-change store record";
            return {};
        }
        votes.push_back(std::move(vote));
    }
    return votes;
}

bool RoundChangeStore::replaceAll(
    const std::vector<protocol::RoundChangeVoteV1>& votes,
    std::string& error) const {
    error.clear();
    const std::string temp_path = path_ + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "could not open temporary round-change store"; return false; }
    for (const auto& vote : votes) {
        if (vote.validator_address.empty() || vote.validator_address.size() > kMaxFieldBytes ||
            vote.public_key.size() > kMaxFieldBytes || vote.signature.size() > kMaxFieldBytes ||
            vote.new_round < 2) {
            error = "invalid round-change store field";
            break;
        }
        writeUint64(out, kMagic);
        writeAddress(out, vote.validator_address);
        writeBytes(out, vote.public_key);
        out.write(reinterpret_cast<const char*>(vote.previous_record_hash.data()), vote.previous_record_hash.size());
        writeUint64(out, vote.integer);
        writeUint64(out, vote.new_round);
        writeBytes(out, vote.signature);
        if (!out) { error = "failed while writing round-change store"; break; }
    }
    out.close();
    if (!error.empty() || !out) {
        if (error.empty()) error = "failed while closing round-change store";
        std::remove(temp_path.c_str());
        return false;
    }
    if (!detail::commitAtomicTemp(temp_path, path_, "round-change store", error)) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace primechain::storage
