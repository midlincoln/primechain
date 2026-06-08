#include "primechain/storage/validator_epoch_store.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

#include "primechain/storage/atomic_file.hpp"

namespace primechain::storage {
namespace {

constexpr std::uint64_t kMagic = 0x3154504543564350ull;
constexpr std::uint64_t kMaxFieldBytes = 8192;
constexpr std::uint64_t kMaxValidators = 16;

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

ValidatorEpochStore::ValidatorEpochStore(std::string path) : path_(std::move(path)) {}

std::vector<ValidatorEpochVoteRecord> ValidatorEpochStore::loadAll(std::string& error) const {
    error.clear();
    if (!detail::prepareAtomicLoad(path_, [](const std::string& candidate, std::string& candidate_error) {
            ValidatorEpochStore candidate_store(candidate);
            candidate_store.loadAll(candidate_error);
            return candidate_error.empty();
        }, error)) return {};
    std::vector<ValidatorEpochVoteRecord> votes;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return votes;
    while (true) {
        std::uint64_t magic = 0;
        if (!readUint64(in, magic)) {
            if (in.eof()) break;
            error = "truncated validator epoch store";
            return {};
        }
        ValidatorEpochVoteRecord record;
        std::uint64_t validator_count = 0;
        if (magic != kMagic ||
            !in.read(reinterpret_cast<char*>(record.previous_record_hash.data()), record.previous_record_hash.size()) ||
            !readUint64(in, record.record_integer) || !readUint64(in, record.epoch) ||
            !readUint64(in, record.activation_integer) || !readUint64(in, validator_count) ||
            validator_count > kMaxValidators) {
            error = "invalid validator epoch store record";
            return {};
        }
        for (std::uint64_t i = 0; i < validator_count; ++i) {
            Address address;
            if (!readAddress(in, address)) {
                error = "truncated validator epoch set";
                return {};
            }
            record.next_validator_set.push_back(std::move(address));
        }
        if (!readAddress(in, record.vote.validator_address) ||
            !readBytes(in, record.vote.public_key) || !readBytes(in, record.vote.signature)) {
            error = "truncated validator epoch vote";
            return {};
        }
        votes.push_back(std::move(record));
    }
    return votes;
}

bool ValidatorEpochStore::replaceAll(
    const std::vector<ValidatorEpochVoteRecord>& votes,
    std::string& error) const {
    error.clear();
    const std::string temp_path = path_ + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open temporary validator epoch store";
        return false;
    }
    for (const auto& record : votes) {
        if (record.next_validator_set.size() > kMaxValidators ||
            record.vote.validator_address.empty() ||
            record.vote.validator_address.size() > kMaxFieldBytes ||
            record.vote.public_key.size() > kMaxFieldBytes || record.vote.signature.size() > kMaxFieldBytes) {
            error = "invalid validator epoch store field";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
        writeUint64(out, kMagic);
        out.write(reinterpret_cast<const char*>(record.previous_record_hash.data()), record.previous_record_hash.size());
        writeUint64(out, record.record_integer);
        writeUint64(out, record.epoch);
        writeUint64(out, record.activation_integer);
        writeUint64(out, record.next_validator_set.size());
        for (const auto& validator : record.next_validator_set) writeAddress(out, validator);
        writeAddress(out, record.vote.validator_address);
        writeBytes(out, record.vote.public_key);
        writeBytes(out, record.vote.signature);
        if (!out) {
            error = "failed while writing validator epoch store";
            out.close();
            std::remove(temp_path.c_str());
            return false;
        }
    }
    out.close();
    if (!out) {
        error = "failed while closing validator epoch store";
        std::remove(temp_path.c_str());
        return false;
    }
    if (!detail::commitAtomicTemp(temp_path, path_, "validator epoch store", error)) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace primechain::storage
