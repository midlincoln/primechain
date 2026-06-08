#pragma once

#include <string>
#include <vector>

#include "primechain/protocol/records.hpp"

namespace primechain::storage {

struct ValidatorEpochVoteRecord {
    Hash256 previous_record_hash{};
    PrimeValue record_integer{0};
    std::uint64_t epoch{0};
    PrimeValue activation_integer{0};
    std::vector<Address> next_validator_set;
    protocol::ValidatorEpochVoteV1 vote;
};

class ValidatorEpochStore {
public:
    explicit ValidatorEpochStore(std::string path);

    std::vector<ValidatorEpochVoteRecord> loadAll(std::string& error) const;
    bool replaceAll(const std::vector<ValidatorEpochVoteRecord>& votes, std::string& error) const;

private:
    std::string path_;
};

} // namespace primechain::storage
