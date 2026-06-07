#pragma once

#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::storage {

struct StoredCommitment {
    PrimeValue integer{0};
    Address provider_address;
    Hash256 commitment_hash{};
};

class CommitmentStore {
public:
    explicit CommitmentStore(std::string path);

    const std::string& path() const { return path_; }

    std::vector<StoredCommitment> loadAll(std::string& error) const;
    bool replaceAll(const std::vector<StoredCommitment>& commitments, std::string& error) const;

private:
    std::string path_;
};

} // namespace primechain::storage
