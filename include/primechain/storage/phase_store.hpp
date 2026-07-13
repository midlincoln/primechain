#pragma once

#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::storage {

struct CommitPhaseVote {
    PrimeValue integer{0};
    std::uint64_t commit_round{1};
    Hash256 snapshot_hash{};
    Address validator_address;
    std::vector<std::uint8_t> public_key;
    std::vector<std::uint8_t> signature;
};

class PhaseStore {
public:
    explicit PhaseStore(std::string path);

    const std::string& path() const { return path_; }
    std::vector<CommitPhaseVote> loadAll(std::string& error) const;
    bool replaceAll(const std::vector<CommitPhaseVote>& votes, std::string& error) const;

private:
    std::string path_;
};

} // namespace primechain::storage
