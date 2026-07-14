#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "primechain/crypto/signature.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/node/validator_registry.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/protocol/validator_governance.hpp"

namespace {

struct ValidatorKey {
    primechain::Address address;
    primechain::crypto::SignatureKeyPair keys;
};

bool expect(bool condition, const std::string& name) {
    if (!condition) std::cerr << "failed: " << name << "\n";
    return condition;
}

std::vector<ValidatorKey> makeValidators(std::size_t count, std::string& error) {
    std::vector<ValidatorKey> out;
    for (std::size_t i = 0; i < count; ++i) {
        const auto keys = primechain::crypto::generateProtocolSignatureKeyPair(error);
        if (!keys.has_value()) return {};
        out.push_back({primechain::crypto::addressFromProtocolPublicKey(keys->public_key), *keys});
    }
    std::sort(out.begin(), out.end(), [](const ValidatorKey& lhs, const ValidatorKey& rhs) {
        return lhs.address < rhs.address;
    });
    return out;
}

std::vector<primechain::Address> addresses(const std::vector<ValidatorKey>& validators) {
    std::vector<primechain::Address> out;
    for (const auto& validator : validators) out.push_back(validator.address);
    return out;
}

primechain::protocol::PrimeRecordV0 makeRotationRecord(
    const primechain::node::SequentialNode& node,
    const std::vector<ValidatorKey>& current,
    std::vector<primechain::Address> next_set,
    std::string& error) {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 2;
    record.height = node.status().height + 1;
    record.previous_record_hash = node.status().latest_record_hash;
    record.integer = 3;
    record.proof.p = 3;
    record.proof.witness = 2;
    record.proof.factors_of_p_minus_1.push_back({2, 1});
    record.proof.provider_address = current[0].address;

    const std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors{{2, 1}};
    const auto prime_signature = primechain::crypto::signProtocolMessage(
        current[0].keys.private_key,
        primechain::crypto::primeProofSigningPayload(
            record.previous_record_hash,
            record.proof.p,
            record.proof.witness,
            factors,
            record.proof.provider_address),
        error);
    if (!prime_signature.has_value()) return {};
    record.proof.signature = primechain::crypto::packPrimeProofAuthentication(
        current[0].keys.public_key, *prime_signature);

    record.validator_epoch.epoch = node.validatorEpoch() + 1;
    record.validator_epoch.activation_integer = record.integer + 1;
    std::sort(next_set.begin(), next_set.end());
    record.validator_epoch.next_validator_set = next_set;

    for (std::size_t i = 0; i < 2; ++i) {
        primechain::protocol::ValidatorEpochVoteV1 vote;
        vote.validator_address = current[i].address;
        vote.public_key = current[i].keys.public_key;
        const auto signature = primechain::crypto::signProtocolMessage(
            current[i].keys.private_key,
            primechain::crypto::validatorEpochVoteSigningPayload(
                record.previous_record_hash,
                record.integer,
                record.validator_epoch.epoch,
                record.validator_epoch.activation_integer,
                record.validator_epoch.next_validator_set,
                vote.validator_address),
            error);
        if (!signature.has_value()) return {};
        vote.signature = *signature;
        record.validator_epoch.votes.push_back(vote);
    }
    std::sort(record.validator_epoch.votes.begin(), record.validator_epoch.votes.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.validator_address < rhs.validator_address; });

    primechain::protocol::updateTransactionBatch(record);
    record.finalized_by.rule = "fixed-2-of-3-mldsa65-v2";
    const auto candidate_hash = primechain::protocol::candidateRecordHash(record);
    for (std::size_t i = 0; i < 2; ++i) {
        auto vote = primechain::protocol::makeSignedValidatorVote(
            current[i].address,
            current[i].keys.public_key,
            current[i].keys.private_key,
            candidate_hash,
            1,
            error);
        if (vote.signature.empty()) return {};
        record.finalized_by.votes.push_back(std::move(vote));
    }
    std::sort(record.finalized_by.votes.begin(), record.finalized_by.votes.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.validator_address < rhs.validator_address; });
    return record;
}

bool testEligibilityPolicy() {
    primechain::protocol::ValidatorEligibilityPolicyV0 policy;
    if (!expect(policy.min_work_score == 100, "default min work score")) return false;
    if (!expect(policy.min_reserve_micro_units == 5000000, "default reserve minimum")) return false;
    if (!expect(policy.epoch_length == 1000, "default epoch length")) return false;

    primechain::protocol::ValidatorWorkStatsV0 weak{4, 10, 900000};
    if (!expect(primechain::protocol::validatorWorkScoreV0(weak) == 69, "weak work score formula")) {
        return false;
    }
    if (!expect(!primechain::protocol::validatorMeetsWorkMinimumV0(weak), "weak work fails")) {
        return false;
    }

    primechain::protocol::ValidatorWorkStatsV0 qualified{5, 15, 2000000};
    if (!expect(primechain::protocol::validatorWorkScoreV0(qualified) == 100, "qualified work score formula")) {
        return false;
    }
    if (!expect(primechain::protocol::validatorMeetsWorkMinimumV0(qualified), "qualified work passes")) {
        return false;
    }
    if (!expect(!primechain::protocol::validatorMeetsReserveMinimumV0(4999999), "reserve below minimum fails")) {
        return false;
    }
    if (!expect(primechain::protocol::validatorMeetsReserveMinimumV0(5000000), "reserve minimum passes")) {
        return false;
    }
    if (!expect(!primechain::protocol::validatorMeetsEndpointUptimeMinimumV0(79, 100), "79 percent uptime fails")) {
        return false;
    }
    if (!expect(primechain::protocol::validatorMeetsEndpointUptimeMinimumV0(80, 100), "80 percent uptime passes")) {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: validator-registry-tests <store-path>\n";
        return 1;
    }
    if (!testEligibilityPolicy()) return 1;

    std::string error;
    const auto validators = makeValidators(4, error);
    if (!expect(validators.size() == 4, "generate validator keys")) {
        std::cerr << error << "\n";
        return 1;
    }
    const std::vector<ValidatorKey> current{validators[0], validators[1], validators[2]};
    std::vector<primechain::Address> next_set{
        validators[0].address, validators[1].address, validators[3].address};
    std::sort(next_set.begin(), next_set.end());

    std::remove(argv[1]);
    primechain::node::SequentialNode node(argv[1]);
    if (!expect(node.load(error), "load empty node") ||
        !expect(node.initializeGenesis(addresses(current), error), "initialize anchored genesis")) {
        std::cerr << error << "\n";
        return 1;
    }

    primechain::node::ValidatorRegistryState genesis_registry;
    if (!expect(primechain::node::loadValidatorRegistry(argv[1], genesis_registry, error), "load genesis registry") ||
        !expect(genesis_registry.has_genesis, "registry has genesis") ||
        !expect(genesis_registry.current_epoch == 0, "registry epoch zero") ||
        !expect(genesis_registry.active_validators == addresses(current), "registry current validators from genesis") ||
        !expect(genesis_registry.events.size() == 1, "registry has one genesis event")) {
        std::cerr << error << "\n";
        return 1;
    }

    auto rotation = makeRotationRecord(node, current, next_set, error);
    if (!expect(node.appendPrime(rotation, error), "append signed validator rotation")) {
        std::cerr << error << "\n";
        return 1;
    }

    primechain::node::ValidatorRegistryState rotated_registry;
    error.clear();
    if (!expect(primechain::node::loadValidatorRegistry(argv[1], rotated_registry, error), "load rotated registry") ||
        !expect(rotated_registry.current_epoch == 1, "registry epoch one") ||
        !expect(rotated_registry.current_activation_integer == 4, "registry activation integer") ||
        !expect(rotated_registry.active_validators == next_set, "registry active validators after rotation") ||
        !expect(rotated_registry.events.size() == 2, "registry has genesis and rotation events") ||
        !expect(rotated_registry.events[1].type == primechain::node::ValidatorRegistryEventType::EpochTransition,
            "registry second event is transition")) {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << "validator registry tests passed\n";
    return 0;
}
