#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"
#include "primechain/types.hpp"

namespace primechain::node {

struct SequentialNodeStatus {
    bool has_genesis{false};
    std::uint64_t height{0};
    PrimeValue frontier_integer{0};
    Hash256 latest_record_hash{};
};

class SequentialNode {
public:
    explicit SequentialNode(std::string record_store_path);

    bool load(std::string& error);
    bool initializeGenesis(std::string& error);

    bool appendComposite(const protocol::CompositeRecordV0& record, std::string& error);
    bool appendPrime(const protocol::PrimeRecordV0& record, std::string& error);

    const SequentialNodeStatus& status() const { return status_; }

private:
    bool validateCommon(
        std::uint64_t height,
        PrimeValue integer,
        const Hash256& previous_record_hash,
        std::string& error) const;

    storage::RecordStore store_;
    SequentialNodeStatus status_;
};

protocol::PrimeRecordV0 makeGenesisPrimeRecordV0();

} // namespace primechain::node
