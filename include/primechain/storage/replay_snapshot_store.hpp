#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::storage {

struct ReplaySnapshot {
    std::uint64_t height{0};
    PrimeValue frontier_integer{0};
    Hash256 record_hash{};
    std::map<std::pair<Address, PrimeValue>, std::uint64_t> balances;
    std::map<PrimeValue, std::uint64_t> total_supply;
    std::map<Address, std::uint64_t> account_nonces;
    std::vector<Address> pending_composite_providers;
    std::vector<Address> validator_set;
    std::uint64_t validator_epoch{0};
    std::uint64_t transfer_fee_micro_units{1};
    std::uint64_t validator_min_reserve_micro_units{5'000'000};
    std::vector<Address> fee_distribution_participants;
};

class ReplaySnapshotStore {
public:
    explicit ReplaySnapshotStore(std::string path);
    bool load(ReplaySnapshot& snapshot, bool& found, std::string& error) const;
    bool replace(const ReplaySnapshot& snapshot, std::string& error) const;
    void discard() const;

private:
    std::string path_;
};

} // namespace primechain::storage
