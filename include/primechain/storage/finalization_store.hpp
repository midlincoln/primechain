#pragma once

#include <string>
#include <vector>

#include "primechain/protocol/records.hpp"

namespace primechain::storage {

struct SignedCandidateRecord {
    PrimeValue integer{0};
    std::string candidate_kind;
    std::vector<std::uint8_t> candidate_payload;
    protocol::ValidatorVoteV0 vote;
};

class FinalizationStore {
public:
    explicit FinalizationStore(std::string path);

    std::vector<SignedCandidateRecord> loadAll(std::string& error) const;
    bool replaceAll(const std::vector<SignedCandidateRecord>& records, std::string& error) const;

private:
    std::string path_;
};

} // namespace primechain::storage
