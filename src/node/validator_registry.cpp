#include "primechain/node/validator_registry.hpp"

#include <optional>

#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"

namespace primechain::node {
namespace {

bool hasTransition(const protocol::ValidatorEpochTransitionV1& transition) {
    return transition.epoch != 0 ||
           transition.activation_integer != 0 ||
           !transition.next_validator_set.empty() ||
           !transition.votes.empty();
}

void applyGenesis(
    ValidatorRegistryState& state,
    const storage::StoredRecord& stored,
    const protocol::PrimeRecordV0& record) {
    if (record.height != 0 || record.genesis_config.validator_set.empty()) return;
    state.has_genesis = true;
    state.current_epoch = 0;
    state.current_activation_integer = record.integer;
    state.active_validators = record.genesis_config.validator_set;
    state.events.push_back({
        ValidatorRegistryEventType::Genesis,
        stored.height,
        stored.integer,
        stored.record_hash,
        0,
        record.integer,
        record.genesis_config.validator_set});
}

void applyTransition(
    ValidatorRegistryState& state,
    const storage::StoredRecord& stored,
    const protocol::ValidatorEpochTransitionV1& transition) {
    if (!hasTransition(transition)) return;
    state.current_epoch = transition.epoch;
    state.current_activation_integer = transition.activation_integer;
    state.active_validators = transition.next_validator_set;
    state.events.push_back({
        ValidatorRegistryEventType::EpochTransition,
        stored.height,
        stored.integer,
        stored.record_hash,
        transition.epoch,
        transition.activation_integer,
        transition.next_validator_set});
}

} // namespace

const char* validatorRegistryEventTypeName(ValidatorRegistryEventType type) {
    switch (type) {
        case ValidatorRegistryEventType::Genesis: return "GENESIS";
        case ValidatorRegistryEventType::EpochTransition: return "EPOCH_TRANSITION";
    }
    return "UNKNOWN";
}

bool loadValidatorRegistry(
    const std::string& record_store_path,
    ValidatorRegistryState& state,
    std::string& error) {
    state = ValidatorRegistryState{};
    storage::RecordStore store(record_store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) return false;

    for (const auto& stored : records) {
        if (stored.kind == storage::StoredRecordKind::Prime) {
            const auto record = protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) return false;
            applyGenesis(state, stored, *record);
            applyTransition(state, stored, record->validator_epoch);
            continue;
        }
        const auto record = protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) return false;
        applyTransition(state, stored, record->validator_epoch);
    }
    return true;
}

} // namespace primechain::node
