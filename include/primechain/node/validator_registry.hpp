#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::node {

enum class ValidatorRegistryEventType {
    Genesis,
    EpochTransition,
};

struct ValidatorRegistryEvent {
    ValidatorRegistryEventType type{ValidatorRegistryEventType::Genesis};
    std::uint64_t height{0};
    PrimeValue record_integer{0};
    Hash256 record_hash{};
    std::uint64_t epoch{0};
    PrimeValue activation_integer{0};
    std::vector<Address> validator_set;
};

struct ValidatorRegistryState {
    bool has_genesis{false};
    std::uint64_t current_epoch{0};
    PrimeValue current_activation_integer{0};
    std::vector<Address> active_validators;
    std::vector<ValidatorRegistryEvent> events;
};

bool loadValidatorRegistry(
    const std::string& record_store_path,
    ValidatorRegistryState& state,
    std::string& error);

const char* validatorRegistryEventTypeName(ValidatorRegistryEventType type);

} // namespace primechain::node
