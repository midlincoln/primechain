#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "primechain/crypto/signature.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"

namespace {

struct ValidatorKey {
    primechain::Address address;
    primechain::crypto::Ed25519KeyPair keys;
};

bool expect(bool condition, const std::string& name) {
    if (!condition) std::cerr << "failed: " << name << "\n";
    return condition;
}

std::vector<ValidatorKey> makeValidators(std::size_t count, std::string& error) {
    std::vector<ValidatorKey> out;
    for (std::size_t i = 0; i < count; ++i) {
        const auto keys = primechain::crypto::generateEd25519KeyPair(error);
        if (!keys.has_value()) return {};
        out.push_back({primechain::crypto::addressFromEd25519PublicKey(keys->public_key), *keys});
    }
    std::sort(out.begin(), out.end(), [](const ValidatorKey& lhs, const ValidatorKey& rhs) {
        return lhs.address < rhs.address;
    });
    return out;
}

primechain::protocol::PrimeRecordV0 makeRotationRecord(
    const primechain::node::SequentialNode& node,
    const std::vector<ValidatorKey>& current,
    std::vector<primechain::Address> next_set,
    std::size_t vote_count,
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
    const std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> prime_factors{{2, 1}};
    const auto prime_signature = primechain::crypto::ed25519Sign(
        current[0].keys.private_key,
        primechain::crypto::primeProofSigningPayload(
            record.previous_record_hash, record.proof.p, record.proof.witness,
            prime_factors, record.proof.provider_address),
        error);
    if (!prime_signature.has_value()) return {};
    record.proof.signature = primechain::crypto::packPrimeProofAuthentication(
        current[0].keys.public_key, *prime_signature);
    record.validator_epoch.epoch = node.validatorEpoch() + 1;
    record.validator_epoch.activation_integer = record.integer + 1;
    std::sort(next_set.begin(), next_set.end());
    record.validator_epoch.next_validator_set = next_set;

    for (std::size_t i = 0; i < vote_count && i < current.size(); ++i) {
        primechain::protocol::ValidatorEpochVoteV1 vote;
        vote.validator_address = current[i].address;
        vote.public_key = current[i].keys.public_key;
        const auto signature = primechain::crypto::ed25519Sign(
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
    record.finalized_by.rule = "fixed-2-of-3-ed25519-v1";
    record.finalized_by.votes.clear();
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

std::vector<primechain::Address> addresses(const std::vector<ValidatorKey>& validators) {
    std::vector<primechain::Address> out;
    for (const auto& validator : validators) out.push_back(validator.address);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: validator-epoch-tests <store-path>\n";
        return 1;
    }

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

    auto rotation = makeRotationRecord(node, current, next_set, 2, error);
    if (!expect(node.appendPrime(rotation, error), "accept signed 2-of-3 epoch rotation")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(node.validatorEpoch() == 1, "activate epoch one") ||
        !expect(node.validatorSet() == next_set, "activate next validator set")) {
        return 1;
    }

    primechain::node::SequentialNode reloaded(argv[1]);
    error.clear();
    if (!expect(reloaded.load(error), "replay signed epoch rotation") ||
        !expect(reloaded.validatorEpoch() == 1, "replay epoch number") ||
        !expect(reloaded.validatorSet() == next_set, "replay validator set")) {
        std::cerr << error << "\n";
        return 1;
    }

    const std::string insufficient_path = std::string(argv[1]) + ".insufficient";
    std::remove(insufficient_path.c_str());
    primechain::node::SequentialNode insufficient(insufficient_path);
    error.clear();
    if (!expect(insufficient.load(error), "load insufficient-vote node") ||
        !expect(insufficient.initializeGenesis(addresses(current), error), "initialize insufficient-vote genesis")) {
        std::cerr << error << "\n";
        return 1;
    }
    auto one_vote = makeRotationRecord(insufficient, current, next_set, 1, error);
    error.clear();
    if (!expect(!insufficient.appendPrime(one_vote, error), "reject one-vote epoch rotation")) return 1;

    const std::string tampered_path = std::string(argv[1]) + ".tampered";
    std::remove(tampered_path.c_str());
    primechain::node::SequentialNode tampered(tampered_path);
    error.clear();
    if (!expect(tampered.load(error), "load tampered-vote node") ||
        !expect(tampered.initializeGenesis(addresses(current), error), "initialize tampered-vote genesis")) {
        std::cerr << error << "\n";
        return 1;
    }
    auto bad_signature = makeRotationRecord(tampered, current, next_set, 2, error);
    bad_signature.validator_epoch.votes[0].signature[0] ^= 0x01;
    primechain::protocol::applyDevelopmentFinalization(bad_signature);
    error.clear();
    if (!expect(!tampered.appendPrime(bad_signature, error), "reject tampered epoch signature")) return 1;

    std::cout << "validator epoch tests passed\n";
    return 0;
}
