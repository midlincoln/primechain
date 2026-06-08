#pragma once

#include <string>
#include <vector>

#include "primechain/protocol/records.hpp"

namespace primechain::storage {

class RoundChangeStore {
public:
    explicit RoundChangeStore(std::string path);

    std::vector<protocol::RoundChangeVoteV1> loadAll(std::string& error) const;
    bool replaceAll(
        const std::vector<protocol::RoundChangeVoteV1>& votes,
        std::string& error) const;

private:
    std::string path_;
};

} // namespace primechain::storage
