#pragma once

#include <cstddef>
#include <string>

#include "primechain/types.hpp"

namespace primechain::core {

class ConsensusEngine {
public:
    bool validateBlock(const Block& block, const ChainState& previous, std::string& error) const;
    ChainState applyBlock(const Block& block, const ChainState& previous) const;

private:
    bool validateHeader(const Block& block, const ChainState& previous, std::string& error) const;
    bool validateCompositeCoverage(const Block& block, const ChainState& previous, std::string& error) const;
};

Hash256 blockHash(const BlockHeader& header);
std::size_t requiredValidatorQuorum(std::size_t validator_count);
bool validValidatorSetSize(std::size_t validator_count);

} // namespace primechain::core
