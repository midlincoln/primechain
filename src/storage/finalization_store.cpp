#include "primechain/storage/finalization_store.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

namespace primechain::storage {
namespace {
constexpr std::uint64_t kMagic = 0x31544f5643464350ull;
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
}

FinalizationStore::FinalizationStore(std::string path) : path_(std::move(path)) {}

std::vector<SignedCandidateRecord> FinalizationStore::loadAll(std::string& error) const {
    error.clear();
    std::vector<SignedCandidateRecord> records;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return records;
    while (true) {
        std::uint64_t magic = 0;
        if (!readUint64(in, magic)) {
            if (in.eof()) break;
            error = "truncated finalization store";
            return {};
        }
        SignedCandidateRecord record;
        if (magic != kMagic || !readUint64(in, record.integer) ||
            !readAddress(in, record.vote.validator_address) ||
            !readBytes(in, record.vote.public_key) ||
            !in.read(reinterpret_cast<char*>(record.vote.record_hash.data()), record.vote.record_hash.size()) ||
            !readUint64(in, record.vote.round) || !readBytes(in, record.vote.signature)) {
            error = "invalid finalization store record";
            return {};
        }
        records.push_back(std::move(record));
    }
    return records;
}

bool FinalizationStore::replaceAll(
    const std::vector<SignedCandidateRecord>& records,
    std::string& error) const {
    error.clear();
    const std::string temp_path = path_ + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "could not open temporary finalization store"; return false; }
    for (const auto& record : records) {
        if (record.vote.validator_address.empty() ||
            record.vote.validator_address.size() > kMaxFieldBytes ||
            record.vote.public_key.size() > kMaxFieldBytes ||
            record.vote.signature.size() > kMaxFieldBytes) {
            error = "invalid finalization store field";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
        writeUint64(out, kMagic);
        writeUint64(out, record.integer);
        writeAddress(out, record.vote.validator_address);
        writeBytes(out, record.vote.public_key);
        out.write(reinterpret_cast<const char*>(record.vote.record_hash.data()), record.vote.record_hash.size());
        writeUint64(out, record.vote.round);
        writeBytes(out, record.vote.signature);
        if (!out) { error = "failed while writing finalization store"; break; }
    }
    out.close();
    if (!error.empty() || !out) {
        if (error.empty()) error = "failed while closing finalization store";
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
        error = "could not atomically replace finalization store";
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace primechain::storage
