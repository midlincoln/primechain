#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "primechain/protocol/records.hpp"
#include "primechain/types.hpp"

namespace primechain::storage {

enum class StoredRecordKind : std::uint64_t {
    Composite = 1,
    Prime = 2,
};

struct StoredRecord {
    StoredRecordKind kind{StoredRecordKind::Composite};
    std::uint64_t height{0};
    PrimeValue integer{0};
    Hash256 record_hash{};
    std::vector<std::uint8_t> payload;
};

class RecordStore {
public:
    explicit RecordStore(std::string path);

    const std::string& path() const { return path_; }

    bool append(const StoredRecord& record, std::string& error) const;
    std::vector<StoredRecord> loadAll(std::string& error) const;
    std::optional<StoredRecord> latest(std::string& error) const;
    std::optional<StoredRecord> findByInteger(PrimeValue integer, std::string& error) const;
    std::vector<StoredRecord> findRange(PrimeValue start, PrimeValue end, std::string& error) const;

private:
    std::string path_;
};

StoredRecord makeStoredRecord(const protocol::CompositeRecordV0& record);
StoredRecord makeStoredRecord(const protocol::PrimeRecordV0& record);

} // namespace primechain::storage
