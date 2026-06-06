#include "primechain/storage/record_store.hpp"

#include <fstream>
#include <limits>
#include <utility>

#include "primechain/crypto/hash.hpp"

namespace primechain::storage {

namespace {

constexpr std::uint64_t kRecordStoreMagic = 0x3056445243435055ull; // "UPCCRDV0" little-endian marker

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

bool readHash(std::istream& in, Hash256& hash) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(hash.data()), hash.size()));
}

void writeHash(std::ostream& out, const Hash256& hash) {
    out.write(reinterpret_cast<const char*>(hash.data()), hash.size());
}

bool validKind(std::uint64_t raw) {
    return raw == static_cast<std::uint64_t>(StoredRecordKind::Composite) ||
           raw == static_cast<std::uint64_t>(StoredRecordKind::Prime);
}

} // namespace

RecordStore::RecordStore(std::string path)
    : path_(std::move(path)) {}

bool RecordStore::append(const StoredRecord& record, std::string& error) const {
    if (record.payload.empty()) {
        error = "record payload is empty";
        return false;
    }
    const Hash256 payload_hash = crypto::devHash256(record.payload);
    if (payload_hash != record.record_hash) {
        error = "record hash does not match payload";
        return false;
    }

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        error = "could not open record store for append";
        return false;
    }

    writeUint64(out, kRecordStoreMagic);
    writeUint64(out, static_cast<std::uint64_t>(record.kind));
    writeUint64(out, record.height);
    writeUint64(out, record.integer);
    writeHash(out, record.record_hash);
    writeUint64(out, record.payload.size());
    out.write(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());

    if (!out) {
        error = "failed while writing record store";
        return false;
    }
    return true;
}

bool RecordStore::replaceTip(
    const Hash256& expected_old_tip_hash,
    const StoredRecord& replacement,
    std::string& error) const {
    auto records = loadAll(error);
    if (!error.empty()) {
        return false;
    }
    if (records.empty()) {
        error = "cannot replace tip in empty store";
        return false;
    }
    if (records.back().record_hash != expected_old_tip_hash) {
        error = "tip hash changed before replacement";
        return false;
    }
    if (replacement.payload.empty()) {
        error = "replacement payload is empty";
        return false;
    }
    if (crypto::devHash256(replacement.payload) != replacement.record_hash) {
        error = "replacement hash does not match payload";
        return false;
    }
    if (replacement.height != records.back().height ||
        replacement.integer != records.back().integer) {
        error = "replacement is not for current tip";
        return false;
    }

    records.back() = replacement;

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open record store for tip replacement";
        return false;
    }
    for (const auto& record : records) {
        writeUint64(out, kRecordStoreMagic);
        writeUint64(out, static_cast<std::uint64_t>(record.kind));
        writeUint64(out, record.height);
        writeUint64(out, record.integer);
        writeHash(out, record.record_hash);
        writeUint64(out, record.payload.size());
        out.write(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
        if (!out) {
            error = "failed while rewriting record store";
            return false;
        }
    }
    return true;
}

std::vector<StoredRecord> RecordStore::loadAll(std::string& error) const {
    std::vector<StoredRecord> records;
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return records;
    }

    while (true) {
        std::uint64_t magic = 0;
        if (!readUint64(in, magic)) {
            if (in.eof()) {
                break;
            }
            error = "truncated record magic";
            return {};
        }
        if (magic != kRecordStoreMagic) {
            error = "invalid record store magic";
            return {};
        }

        std::uint64_t raw_kind = 0;
        StoredRecord record;
        if (!readUint64(in, raw_kind) ||
            !readUint64(in, record.height) ||
            !readUint64(in, record.integer) ||
            !readHash(in, record.record_hash)) {
            error = "truncated record header";
            return {};
        }
        if (!validKind(raw_kind)) {
            error = "invalid record kind";
            return {};
        }
        record.kind = static_cast<StoredRecordKind>(raw_kind);

        std::uint64_t payload_size = 0;
        if (!readUint64(in, payload_size)) {
            error = "truncated payload size";
            return {};
        }
        if (payload_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            error = "payload too large for platform";
            return {};
        }

        record.payload.resize(static_cast<std::size_t>(payload_size));
        if (!in.read(reinterpret_cast<char*>(record.payload.data()), record.payload.size())) {
            error = "truncated record payload";
            return {};
        }

        const Hash256 payload_hash = crypto::devHash256(record.payload);
        if (payload_hash != record.record_hash) {
            error = "record payload hash mismatch at height " + std::to_string(record.height);
            return {};
        }

        records.push_back(std::move(record));
    }

    return records;
}

std::optional<StoredRecord> RecordStore::latest(std::string& error) const {
    auto records = loadAll(error);
    if (!error.empty() || records.empty()) {
        return std::nullopt;
    }
    return records.back();
}

std::optional<StoredRecord> RecordStore::findByInteger(PrimeValue integer, std::string& error) const {
    auto records = loadAll(error);
    if (!error.empty()) {
        return std::nullopt;
    }
    for (const auto& record : records) {
        if (record.integer == integer) {
            return record;
        }
    }
    return std::nullopt;
}

std::vector<StoredRecord> RecordStore::findRange(PrimeValue start, PrimeValue end, std::string& error) const {
    std::vector<StoredRecord> out;
    if (start > end) {
        error = "range start is greater than range end";
        return out;
    }

    auto records = loadAll(error);
    if (!error.empty()) {
        return {};
    }

    for (const auto& record : records) {
        if (record.integer >= start && record.integer <= end) {
            out.push_back(record);
        }
    }
    return out;
}

StoredRecord makeStoredRecord(const protocol::CompositeRecordV0& record) {
    StoredRecord out;
    out.kind = StoredRecordKind::Composite;
    out.height = record.height;
    out.integer = record.integer;
    out.payload = protocol::serializeCompositeRecord(record);
    out.record_hash = protocol::finalizedRecordHash(record);
    return out;
}

StoredRecord makeStoredRecord(const protocol::PrimeRecordV0& record) {
    StoredRecord out;
    out.kind = StoredRecordKind::Prime;
    out.height = record.height;
    out.integer = record.integer;
    out.payload = protocol::serializePrimeRecord(record);
    out.record_hash = protocol::finalizedRecordHash(record);
    return out;
}

} // namespace primechain::storage
