#include "primechain/storage/record_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "primechain/crypto/hash.hpp"
#include "primechain/storage/atomic_file.hpp"

namespace primechain::storage {

namespace {

constexpr std::uint64_t kRecordStoreMagic = 0x3056445243435055ull; // "UPCCRDV0"
constexpr std::uint64_t kRecordIndexMagic = 0x3158444943435055ull; // "UPCCIDX1"
constexpr std::uint64_t kRecordHeaderBytes = 72;
constexpr std::uint64_t kMaxRecordPayloadBytes = 64ull * 1024ull * 1024ull;

std::mutex& recordStoreMutex() {
    static std::mutex mutex;
    return mutex;
}

struct IndexEntry {
    PrimeValue integer{0};
    std::uint64_t offset{0};
};

bool readUint64(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        char ch = 0;
        if (!in.get(ch)) return false;
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) << (i * 8);
    }
    return true;
}

void appendUint64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
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

bool validKind(std::uint64_t raw) {
    return raw == static_cast<std::uint64_t>(StoredRecordKind::Composite) ||
           raw == static_cast<std::uint64_t>(StoredRecordKind::Prime);
}


std::optional<Hash256> canonicalHashFromPayload(
    StoredRecordKind kind,
    const std::vector<std::uint8_t>& payload,
    std::string& error) {
    if (kind == StoredRecordKind::Composite) {
        auto record = protocol::deserializeCompositeRecord(payload, error);
        if (!record.has_value()) return std::nullopt;
        return protocol::canonicalStoredRecordHash(*record);
    }
    auto record = protocol::deserializePrimeRecord(payload, error);
    if (!record.has_value()) return std::nullopt;
    return protocol::canonicalStoredRecordHash(*record);
}

bool recordPayloadMatchesEnvelope(const StoredRecord& record, std::string& error) {
    auto canonical_hash = canonicalHashFromPayload(record.kind, record.payload, error);
    if (!canonical_hash.has_value()) {
        if (error.empty()) error = "record payload did not decode";
        return false;
    }
    if (*canonical_hash != record.record_hash) {
        error = "record hash does not match canonical payload identity";
        return false;
    }
    return true;
}

bool validateRecord(const StoredRecord& record, std::string& error) {
    if (record.payload.empty()) {
        error = "record payload is empty";
        return false;
    }
    if (record.payload.size() > kMaxRecordPayloadBytes) {
        error = "record payload exceeds store limit";
        return false;
    }
    return recordPayloadMatchesEnvelope(record, error);
}

std::vector<std::uint8_t> encodeRecord(const StoredRecord& record) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kRecordHeaderBytes) + record.payload.size());
    appendUint64(bytes, kRecordStoreMagic);
    appendUint64(bytes, static_cast<std::uint64_t>(record.kind));
    appendUint64(bytes, record.height);
    appendUint64(bytes, record.integer);
    bytes.insert(bytes.end(), record.record_hash.begin(), record.record_hash.end());
    appendUint64(bytes, record.payload.size());
    bytes.insert(bytes.end(), record.payload.begin(), record.payload.end());
    return bytes;
}

bool fileSize(const std::string& path, std::uint64_t& size, std::string& error) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            size = 0;
            return true;
        }
        error = "could not stat record store: " + std::string(std::strerror(errno));
        return false;
    }
    if (info.st_size < 0) {
        error = "record store has invalid size";
        return false;
    }
    size = static_cast<std::uint64_t>(info.st_size);
    return true;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::string parentDirectory(const std::string& path) {
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) return ".";
    if (separator == 0) return "/";
    return path.substr(0, separator);
}

bool syncParentDirectory(const std::string& path, std::string& error) {
    const std::string directory = parentDirectory(path);
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        error = "could not open record store directory: " + std::string(std::strerror(errno));
        return false;
    }
    const bool ok = fsync(fd) == 0;
    const int saved_errno = errno;
    close(fd);
    if (!ok) {
        error = "could not sync record store directory: " + std::string(std::strerror(saved_errno));
        return false;
    }
    return true;
}

bool scanStore(
    const std::string& path,
    std::vector<StoredRecord>* records,
    std::vector<IndexEntry>& entries,
    std::uint64_t& store_size,
    std::uint64_t& valid_size,
    bool& incomplete_tail,
    std::string& error) {
    entries.clear();
    if (records != nullptr) records->clear();
    valid_size = 0;
    incomplete_tail = false;
    if (!fileSize(path, store_size, error)) return false;
    if (store_size == 0) return true;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not open record store";
        return false;
    }

    std::uint64_t offset = 0;
    while (offset < store_size) {
        if (store_size - offset < kRecordHeaderBytes) {
            incomplete_tail = true;
            break;
        }
        in.seekg(static_cast<std::streamoff>(offset));
        std::uint64_t magic = 0;
        std::uint64_t raw_kind = 0;
        StoredRecord record;
        std::uint64_t payload_size = 0;
        if (!readUint64(in, magic) || !readUint64(in, raw_kind) ||
            !readUint64(in, record.height) || !readUint64(in, record.integer) ||
            !readHash(in, record.record_hash) || !readUint64(in, payload_size)) {
            incomplete_tail = true;
            break;
        }
        if (magic != kRecordStoreMagic) {
            error = "invalid record store magic at offset " + std::to_string(offset);
            return false;
        }
        if (!validKind(raw_kind)) {
            error = "invalid record kind at offset " + std::to_string(offset);
            return false;
        }
        if (payload_size == 0 || payload_size > kMaxRecordPayloadBytes ||
            payload_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            error = "invalid record payload size at offset " + std::to_string(offset);
            return false;
        }
        if (payload_size > store_size - offset - kRecordHeaderBytes) {
            incomplete_tail = true;
            break;
        }

        record.kind = static_cast<StoredRecordKind>(raw_kind);
        record.payload.resize(static_cast<std::size_t>(payload_size));
        if (!in.read(reinterpret_cast<char*>(record.payload.data()), record.payload.size())) {
            incomplete_tail = true;
            break;
        }
        std::string payload_error;
        if (!recordPayloadMatchesEnvelope(record, payload_error)) {
            error = "record payload identity mismatch at height " + std::to_string(record.height) +
                (payload_error.empty() ? std::string{} : ": " + payload_error);
            return false;
        }

        entries.push_back({record.integer, offset});
        if (records != nullptr) records->push_back(std::move(record));
        offset += kRecordHeaderBytes + payload_size;
        valid_size = offset;
    }
    return true;
}

bool backupRecordStoreBeforeRecovery(
    const std::string& path,
    std::string& backup_path,
    std::string& error) {
    const std::string backup_prefix = path + ".recovery-backup." +
        std::to_string(static_cast<std::uint64_t>(std::time(nullptr)));

    const int source_fd = open(path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        error = "could not open record store for recovery backup: " + std::string(std::strerror(errno));
        return false;
    }

    int backup_fd = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        backup_path = backup_prefix + "." + std::to_string(attempt);
        backup_fd = open(backup_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (backup_fd >= 0) break;
        if (errno != EEXIST) break;
    }
    if (backup_fd < 0) {
        const int saved_errno = errno;
        close(source_fd);
        error = "could not create record store recovery backup: " + std::string(std::strerror(saved_errno));
        return false;
    }

    std::vector<std::uint8_t> buffer(64 * 1024);
    bool ok = true;
    while (true) {
        const ssize_t count = read(source_fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            error = "failed while reading record store for recovery backup: " +
                std::string(std::strerror(errno));
            ok = false;
            break;
        }
        if (count == 0) break;
        if (!writeAll(backup_fd, buffer.data(), static_cast<std::size_t>(count))) {
            error = "failed while writing record store recovery backup: " +
                std::string(std::strerror(errno));
            ok = false;
            break;
        }
    }
    close(source_fd);
    if (ok && fsync(backup_fd) != 0) {
        error = "could not sync record store recovery backup: " + std::string(std::strerror(errno));
        ok = false;
    }
    if (close(backup_fd) != 0 && ok) {
        error = "could not close record store recovery backup: " + std::string(std::strerror(errno));
        ok = false;
    }
    if (!ok) {
        std::remove(backup_path.c_str());
        return false;
    }
    return syncParentDirectory(backup_path, error);
}

bool truncateIncompleteTail(const std::string& path, std::uint64_t valid_size, std::string& error) {
    std::string backup_path;
    if (!backupRecordStoreBeforeRecovery(path, backup_path, error)) return false;
    if (truncate(path.c_str(), static_cast<off_t>(valid_size)) != 0) {
        error = "could not truncate incomplete record tail after backup " + backup_path + ": " +
            std::string(std::strerror(errno));
        return false;
    }
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = "could not open recovered record store: " + std::string(std::strerror(errno));
        return false;
    }
    const bool ok = fsync(fd) == 0;
    const int saved_errno = errno;
    close(fd);
    if (!ok) {
        error = "could not sync recovered record store: " + std::string(std::strerror(saved_errno));
        return false;
    }
    return syncParentDirectory(path, error);
}

bool writeIndex(
    const std::string& store_path,
    std::uint64_t store_size,
    const std::vector<IndexEntry>& entries,
    std::string& error) {
    const std::string index_path = store_path + ".idx";
    const std::string temp_path = detail::uniqueAtomicTempPath(index_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open temporary record index";
        return false;
    }
    writeUint64(out, kRecordIndexMagic);
    writeUint64(out, store_size);
    writeUint64(out, entries.size());
    for (const auto& entry : entries) {
        writeUint64(out, entry.integer);
        writeUint64(out, entry.offset);
    }
    out.close();
    if (!out) {
        error = "failed while writing record index";
        std::remove(temp_path.c_str());
        return false;
    }
    const int fd = open(temp_path.c_str(), O_RDONLY);
    if (fd < 0 || fsync(fd) != 0) {
        const int saved_errno = errno;
        if (fd >= 0) close(fd);
        std::remove(temp_path.c_str());
        error = "could not sync record index: " + std::string(std::strerror(saved_errno));
        return false;
    }
    close(fd);
    if (std::rename(temp_path.c_str(), index_path.c_str()) != 0) {
        error = "could not atomically replace record index: " + std::string(std::strerror(errno));
        std::remove(temp_path.c_str());
        return false;
    }
    return syncParentDirectory(index_path, error);
}

bool appendIndexEntry(
    const std::string& store_path,
    std::uint64_t old_store_size,
    std::uint64_t new_store_size,
    std::uint64_t old_count,
    const IndexEntry& entry) {
    const std::string index_path = store_path + ".idx";
    const int fd = open(index_path.c_str(), O_RDWR);
    if (fd < 0) return false;
    const off_t expected_size = static_cast<off_t>(24 + old_count * 16);
    bool ok = ftruncate(fd, expected_size) == 0 &&
        lseek(fd, expected_size, SEEK_SET) == expected_size;
    std::vector<std::uint8_t> encoded;
    encoded.reserve(16);
    appendUint64(encoded, entry.integer);
    appendUint64(encoded, entry.offset);
    ok = ok && writeAll(fd, encoded.data(), encoded.size());

    std::vector<std::uint8_t> header_update;
    header_update.reserve(16);
    appendUint64(header_update, new_store_size);
    appendUint64(header_update, old_count + 1);
    if (ok) {
        std::size_t written = 0;
        while (written < header_update.size()) {
            const ssize_t result = pwrite(
                fd, header_update.data() + written, header_update.size() - written,
                static_cast<off_t>(8 + written));
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                ok = false;
                break;
            }
            written += static_cast<std::size_t>(result);
        }
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
    close(fd);
    if (!ok || entry.offset != old_store_size) {
        std::remove(index_path.c_str());
        return false;
    }
    return true;
}

bool readHeaderAt(
    std::ifstream& in,
    std::uint64_t store_size,
    const IndexEntry& entry) {
    if (entry.offset > store_size || store_size - entry.offset < kRecordHeaderBytes) return false;
    in.seekg(static_cast<std::streamoff>(entry.offset));
    std::uint64_t magic = 0;
    std::uint64_t raw_kind = 0;
    std::uint64_t height = 0;
    std::uint64_t integer = 0;
    Hash256 hash{};
    std::uint64_t payload_size = 0;
    if (!readUint64(in, magic) || !readUint64(in, raw_kind) || !readUint64(in, height) ||
        !readUint64(in, integer) || !readHash(in, hash) || !readUint64(in, payload_size)) {
        in.clear();
        return false;
    }
    if (magic != kRecordStoreMagic || !validKind(raw_kind) || integer != entry.integer ||
        payload_size == 0 || payload_size > kMaxRecordPayloadBytes ||
        payload_size > store_size - entry.offset - kRecordHeaderBytes) {
        return false;
    }
    return true;
}

bool loadIndex(
    const std::string& store_path,
    std::uint64_t store_size,
    std::vector<IndexEntry>& entries) {
    entries.clear();
    std::ifstream index(store_path + ".idx", std::ios::binary);
    if (!index) return false;
    std::uint64_t magic = 0;
    std::uint64_t indexed_size = 0;
    std::uint64_t count = 0;
    if (!readUint64(index, magic) || !readUint64(index, indexed_size) ||
        !readUint64(index, count) || magic != kRecordIndexMagic || indexed_size != store_size ||
        count > store_size / kRecordHeaderBytes) {
        return false;
    }
    entries.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        IndexEntry entry;
        if (!readUint64(index, entry.integer) || !readUint64(index, entry.offset)) return false;
        if (!entries.empty() && (entry.integer <= entries.back().integer ||
            entry.offset <= entries.back().offset)) return false;
        entries.push_back(entry);
    }
    char trailing = 0;
    if (index.get(trailing)) return false;
    if (store_size == 0) return entries.empty();
    if (entries.empty() || entries.front().offset != 0) return false;

    std::ifstream store(store_path, std::ios::binary);
    if (!store || !readHeaderAt(store, store_size, entries.front()) ||
        !readHeaderAt(store, store_size, entries.back())) return false;
    return true;
}

bool prepareIndex(
    const std::string& path,
    std::vector<IndexEntry>& entries,
    std::uint64_t& store_size,
    std::string& error) {
    if (!fileSize(path, store_size, error)) return false;
    if (loadIndex(path, store_size, entries)) return true;

    std::uint64_t valid_size = 0;
    bool incomplete_tail = false;
    if (!scanStore(path, nullptr, entries, store_size, valid_size, incomplete_tail, error)) return false;
    if (incomplete_tail) {
        if (!truncateIncompleteTail(path, valid_size, error)) return false;
        store_size = valid_size;
    }
    return writeIndex(path, store_size, entries, error);
}

bool sameIndexEntries(
    const std::vector<IndexEntry>& left,
    const std::vector<IndexEntry>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].integer != right[i].integer || left[i].offset != right[i].offset) {
            return false;
        }
    }
    return true;
}

std::optional<StoredRecord> readRecordAt(
    const std::string& path,
    std::uint64_t offset,
    std::string& error) {
    std::uint64_t store_size = 0;
    if (!fileSize(path, store_size, error) || offset > store_size ||
        store_size - offset < kRecordHeaderBytes) {
        if (error.empty()) error = "record index offset is outside store";
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::uint64_t magic = 0;
    std::uint64_t raw_kind = 0;
    StoredRecord record;
    std::uint64_t payload_size = 0;
    if (!readUint64(in, magic) || !readUint64(in, raw_kind) ||
        !readUint64(in, record.height) || !readUint64(in, record.integer) ||
        !readHash(in, record.record_hash) || !readUint64(in, payload_size) ||
        magic != kRecordStoreMagic || !validKind(raw_kind) || payload_size == 0 ||
        payload_size > kMaxRecordPayloadBytes ||
        payload_size > store_size - offset - kRecordHeaderBytes) {
        error = "record index points to invalid record header";
        return std::nullopt;
    }
    record.kind = static_cast<StoredRecordKind>(raw_kind);
    record.payload.resize(static_cast<std::size_t>(payload_size));
    if (!in.read(reinterpret_cast<char*>(record.payload.data()), record.payload.size())) {
        error = "record index points to truncated payload";
        return std::nullopt;
    }
    std::string payload_error;
    if (!recordPayloadMatchesEnvelope(record, payload_error)) {
        error = "record payload identity mismatch at height " + std::to_string(record.height) +
            (payload_error.empty() ? std::string{} : ": " + payload_error);
        return std::nullopt;
    }
    return record;
}

bool writeStoreAtomically(
    const std::string& path,
    const std::vector<StoredRecord>& records,
    std::vector<IndexEntry>& entries,
    std::uint64_t& store_size,
    std::string& error) {
    const std::string temp_path = detail::uniqueAtomicTempPath(path, ".rewrite.tmp");
    const int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = "could not open temporary record store: " + std::string(std::strerror(errno));
        return false;
    }
    entries.clear();
    store_size = 0;
    for (const auto& record : records) {
        if (!validateRecord(record, error)) {
            close(fd);
            std::remove(temp_path.c_str());
            return false;
        }
        const auto bytes = encodeRecord(record);
        entries.push_back({record.integer, store_size});
        if (!writeAll(fd, bytes.data(), bytes.size())) {
            error = "failed while writing temporary record store: " +
                std::string(std::strerror(errno));
            close(fd);
            std::remove(temp_path.c_str());
            return false;
        }
        store_size += bytes.size();
    }
    if (fsync(fd) != 0) {
        error = "could not sync temporary record store: " + std::string(std::strerror(errno));
        close(fd);
        std::remove(temp_path.c_str());
        return false;
    }
    if (close(fd) != 0) {
        error = "could not close temporary record store: " + std::string(std::strerror(errno));
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        error = "could not atomically replace record store: " + std::string(std::strerror(errno));
        std::remove(temp_path.c_str());
        return false;
    }
    return syncParentDirectory(path, error);
}

} // namespace

RecordStore::RecordStore(std::string path)
    : path_(std::move(path)) {}

bool RecordStore::append(const StoredRecord& record, std::string& error) const {
    std::lock_guard<std::mutex> lock(recordStoreMutex());
    if (!validateRecord(record, error)) return false;

    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    if (!prepareIndex(path_, entries, store_size, error)) return false;
    if (!entries.empty()) {
        auto latest = readRecordAt(path_, entries.back().offset, error);
        if (!latest.has_value()) return false;
        if (latest->integer == record.integer && latest->record_hash == record.record_hash) {
            return true;
        }
        if (latest->integer >= record.integer) {
            error = "record already exists with a different hash";
            return false;
        }
    }

    const auto bytes = encodeRecord(record);
    const int fd = open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        error = "could not open record store for append: " + std::string(std::strerror(errno));
        return false;
    }
    if (!writeAll(fd, bytes.data(), bytes.size()) || fsync(fd) != 0) {
        const int saved_errno = errno;
        close(fd);
        error = "failed while durably appending record store: " +
            std::string(std::strerror(saved_errno));
        return false;
    }
    close(fd);

    const IndexEntry entry{record.integer, store_size};
    const std::uint64_t new_store_size = store_size + bytes.size();
    if (!appendIndexEntry(path_, store_size, new_store_size, entries.size(), entry)) {
        std::cerr << "record_store: index update failed for integer="
                  << record.integer << " at " << path_
                  << " -- record data is durable, but the index is stale"
                  << " and will be rebuilt from the record store when needed\n";
    }
    return true;
}

bool RecordStore::replaceTip(
    const Hash256& expected_old_tip_hash,
    const StoredRecord& replacement,
    std::string& error) const {
    std::lock_guard<std::mutex> lock(recordStoreMutex());
    auto records = loadAll(error);
    if (!error.empty()) return false;
    if (records.empty()) {
        error = "cannot replace tip in empty store";
        return false;
    }
    if (records.back().record_hash != expected_old_tip_hash) {
        error = "tip hash changed before replacement";
        return false;
    }
    if (!validateRecord(replacement, error)) return false;
    if (replacement.height != records.back().height ||
        replacement.integer != records.back().integer) {
        error = "replacement is not for current tip";
        return false;
    }
    records.back() = replacement;

    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    if (!writeStoreAtomically(path_, records, entries, store_size, error)) return false;
    if (!writeIndex(path_, store_size, entries, error)) {
        std::remove((path_ + ".idx").c_str());
        error.clear();
    }
    return true;
}

bool RecordStore::installValidatedStore(const std::string& source_path, std::string& error) const {
    std::lock_guard<std::mutex> lock(recordStoreMutex());
    std::vector<IndexEntry> source_entries;
    std::uint64_t source_size = 0;
    std::uint64_t valid_size = 0;
    bool incomplete_tail = false;
    if (!scanStore(source_path, nullptr, source_entries, source_size, valid_size,
            incomplete_tail, error)) return false;
    if (incomplete_tail || valid_size != source_size) {
        error = "cannot install store with incomplete record tail";
        return false;
    }

    const std::string temp_path = detail::uniqueAtomicTempPath(path_, ".install.tmp");
    const int source_fd = open(source_path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        error = "could not open validated store for install: " +
            std::string(std::strerror(errno));
        return false;
    }
    const int destination_fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (destination_fd < 0) {
        error = "could not open temporary installed store: " +
            std::string(std::strerror(errno));
        close(source_fd);
        return false;
    }

    std::vector<std::uint8_t> buffer(64 * 1024);
    bool ok = true;
    while (true) {
        const ssize_t count = read(source_fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            error = "failed while reading validated store: " +
                std::string(std::strerror(errno));
            ok = false;
            break;
        }
        if (count == 0) break;
        if (!writeAll(destination_fd, buffer.data(), static_cast<std::size_t>(count))) {
            error = "failed while writing installed store: " +
                std::string(std::strerror(errno));
            ok = false;
            break;
        }
    }
    close(source_fd);
    if (ok && fsync(destination_fd) != 0) {
        error = "could not sync installed store: " + std::string(std::strerror(errno));
        ok = false;
    }
    if (close(destination_fd) != 0 && ok) {
        error = "could not close installed store: " + std::string(std::strerror(errno));
        ok = false;
    }
    if (!ok) {
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
        error = "could not atomically install validated store: " +
            std::string(std::strerror(errno));
        std::remove(temp_path.c_str());
        return false;
    }
    if (!syncParentDirectory(path_, error)) return false;

    std::remove((path_ + ".idx").c_str());
    if (!writeIndex(path_, source_size, source_entries, error)) {
        std::remove((path_ + ".idx").c_str());
        error.clear();
    }
    return true;
}

std::vector<StoredRecord> RecordStore::loadAll(std::string& error) const {
    std::vector<StoredRecord> records;
    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    std::uint64_t valid_size = 0;
    bool incomplete_tail = false;
    if (!scanStore(path_, &records, entries, store_size, valid_size, incomplete_tail, error)) {
        return {};
    }
    if (incomplete_tail) {
        if (!truncateIncompleteTail(path_, valid_size, error)) return {};
        store_size = valid_size;
    }
    std::vector<IndexEntry> indexed_entries;
    if (!loadIndex(path_, store_size, indexed_entries) ||
        !sameIndexEntries(entries, indexed_entries)) {
        std::string index_error;
        if (!writeIndex(path_, store_size, entries, index_error)) {
            std::remove((path_ + ".idx").c_str());
        }
    }
    return records;
}

std::optional<StoredRecord> RecordStore::latest(std::string& error) const {
    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    if (!prepareIndex(path_, entries, store_size, error) || entries.empty()) {
        return std::nullopt;
    }
    return readRecordAt(path_, entries.back().offset, error);
}

std::optional<StoredRecord> RecordStore::findByInteger(PrimeValue integer, std::string& error) const {
    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    if (!prepareIndex(path_, entries, store_size, error)) return std::nullopt;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto found = std::lower_bound(entries.begin(), entries.end(), integer,
            [](const IndexEntry& entry, PrimeValue value) { return entry.integer < value; });
        if (found == entries.end() || found->integer != integer) return std::nullopt;
        auto record = readRecordAt(path_, found->offset, error);
        if (record.has_value() && record->integer == integer) return record;
        if (attempt != 0) {
            if (error.empty()) error = "record index integer mismatch";
            return std::nullopt;
        }
        std::remove((path_ + ".idx").c_str());
        error.clear();
        if (!prepareIndex(path_, entries, store_size, error)) return std::nullopt;
    }
    return std::nullopt;
}

std::vector<StoredRecord> RecordStore::findRange(
    PrimeValue start,
    PrimeValue end,
    std::string& error) const {
    std::vector<StoredRecord> out;
    if (start > end) {
        error = "range start is greater than range end";
        return out;
    }
    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    if (!prepareIndex(path_, entries, store_size, error)) return {};
    for (int attempt = 0; attempt < 2; ++attempt) {
        out.clear();
        auto current = std::lower_bound(entries.begin(), entries.end(), start,
            [](const IndexEntry& entry, PrimeValue value) { return entry.integer < value; });
        bool invalid_index = false;
        while (current != entries.end() && current->integer <= end) {
            auto record = readRecordAt(path_, current->offset, error);
            if (!record.has_value() || record->integer != current->integer) {
                invalid_index = true;
                break;
            }
            out.push_back(std::move(*record));
            ++current;
        }
        if (!invalid_index) return out;
        if (attempt != 0) {
            if (error.empty()) error = "record index integer mismatch";
            return {};
        }
        std::remove((path_ + ".idx").c_str());
        error.clear();
        if (!prepareIndex(path_, entries, store_size, error)) return {};
    }
    return {};
}

bool RecordStore::forEachRange(
    PrimeValue start,
    PrimeValue end,
    const std::function<bool(const StoredRecord&)>& visitor,
    std::string& error) const {
    if (start > end) {
        error = "range start is greater than range end";
        return false;
    }
    std::vector<IndexEntry> entries;
    std::uint64_t store_size = 0;
    if (!prepareIndex(path_, entries, store_size, error)) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto current = std::lower_bound(entries.begin(), entries.end(), start,
            [](const IndexEntry& entry, PrimeValue value) { return entry.integer < value; });
        PrimeValue expected = start;
        bool invalid_index = false;
        while (expected <= end) {
            if (current == entries.end() || current->integer != expected) {
                error = "requested record range is incomplete";
                return false;
            }
            auto record = readRecordAt(path_, current->offset, error);
            if (!record.has_value() || record->integer != current->integer) {
                invalid_index = true;
                break;
            }
            if (!visitor(*record)) return false;
            ++current;
            ++expected;
        }
        if (!invalid_index) return true;
        if (attempt != 0) {
            if (error.empty()) error = "record index integer mismatch";
            return false;
        }
        std::remove((path_ + ".idx").c_str());
        error.clear();
        if (!prepareIndex(path_, entries, store_size, error)) return false;
    }
    return true;
}

StoredRecord makeStoredRecord(const protocol::CompositeRecordV0& record) {
    StoredRecord out;
    out.kind = StoredRecordKind::Composite;
    out.height = record.height;
    out.integer = record.integer;
    out.payload = protocol::serializeCompositeRecord(record);
    out.record_hash = protocol::canonicalStoredRecordHash(record);
    return out;
}

StoredRecord makeStoredRecord(const protocol::PrimeRecordV0& record) {
    StoredRecord out;
    out.kind = StoredRecordKind::Prime;
    out.height = record.height;
    out.integer = record.integer;
    out.payload = protocol::serializePrimeRecord(record);
    out.record_hash = protocol::canonicalStoredRecordHash(record);
    return out;
}

} // namespace primechain::storage
