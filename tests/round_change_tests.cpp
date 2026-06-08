#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "primechain/crypto/signature.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/storage/round_change_store.hpp"

namespace {

struct ValidatorKey {
    primechain::Address address;
    primechain::crypto::Ed25519KeyPair keys;
};

bool expect(bool condition, const std::string& name) {
    if (!condition) std::cerr << "failed: " << name << "\n";
    return condition;
}

std::vector<ValidatorKey> makeValidators(std::string& error) {
    std::vector<ValidatorKey> validators;
    for (int i = 0; i < 3; ++i) {
        const auto keys = primechain::crypto::generateEd25519KeyPair(error);
        if (!keys.has_value()) return {};
        validators.push_back({
            primechain::crypto::addressFromEd25519PublicKey(keys->public_key), *keys});
    }
    std::sort(validators.begin(), validators.end(),
        [](const auto& left, const auto& right) {
            return left.address < right.address;
        });
    return validators;
}

std::vector<primechain::Address> addresses(
    const std::vector<ValidatorKey>& validators) {
    std::vector<primechain::Address> out;
    for (const auto& validator : validators) out.push_back(validator.address);
    return out;
}

primechain::protocol::RoundChangeVoteV1 makeRoundChangeVote(
    const ValidatorKey& validator,
    const primechain::Hash256& previous_hash,
    primechain::PrimeValue integer,
    std::uint64_t new_round,
    std::string& error) {
    primechain::protocol::RoundChangeVoteV1 vote;
    vote.validator_address = validator.address;
    vote.public_key = validator.keys.public_key;
    vote.previous_record_hash = previous_hash;
    vote.integer = integer;
    vote.new_round = new_round;
    const auto signature = primechain::crypto::ed25519Sign(
        validator.keys.private_key,
        primechain::crypto::roundChangeVoteSigningPayload(
            previous_hash, integer, new_round, validator.address),
        error);
    if (signature.has_value()) vote.signature = *signature;
    return vote;
}

primechain::protocol::PrimeRecordV0 makeRoundTwoPrime(
    const primechain::node::SequentialNode& node,
    const std::vector<ValidatorKey>& validators,
    std::string& error) {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 1;
    record.height = node.status().height + 1;
    record.previous_record_hash = node.status().latest_record_hash;
    record.integer = 3;
    record.proof.p = 3;
    record.proof.witness = 2;
    record.proof.factors_of_p_minus_1.push_back({2, 1});
    record.proof.provider_address = validators[0].address;

    const std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors{{2, 1}};
    const auto prime_signature = primechain::crypto::ed25519Sign(
        validators[0].keys.private_key,
        primechain::crypto::primeProofSigningPayload(
            record.previous_record_hash, record.integer, record.proof.witness,
            factors, record.proof.provider_address),
        error);
    if (!prime_signature.has_value()) return {};
    record.proof.signature = primechain::crypto::packPrimeProofAuthentication(
        validators[0].keys.public_key, *prime_signature);

    record.finalized_by.rule = "fixed-2-of-3-ed25519-rounds-v2";
    for (std::size_t i = 0; i < 2; ++i) {
        record.finalized_by.round_changes.push_back(makeRoundChangeVote(
            validators[i], record.previous_record_hash, record.integer, 2, error));
    }
    const auto candidate_hash = primechain::protocol::candidateRecordHash(record);
    for (std::size_t i = 0; i < 2; ++i) {
        record.finalized_by.votes.push_back(
            primechain::protocol::makeSignedValidatorVote(
                validators[i].address, validators[i].keys.public_key,
                validators[i].keys.private_key, candidate_hash, 2, error));
    }
    return record;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: round-change-tests <store-path>\n";
        return 2;
    }

    const std::string chain_path = argv[1];
    const std::string round_path = chain_path + ".rounds";
    std::remove(chain_path.c_str());
    std::remove(round_path.c_str());

    std::string error;
    const auto validators = makeValidators(error);
    if (!expect(validators.size() == 3, "generate validator identities")) {
        std::cerr << error << "\n";
        return 1;
    }

    primechain::node::SequentialNode node(chain_path);
    if (!expect(node.load(error), "load empty node") ||
        !expect(node.initializeGenesis(addresses(validators), error),
                "initialize anchored genesis")) {
        std::cerr << error << "\n";
        return 1;
    }

    auto record = makeRoundTwoPrime(node, validators, error);
    if (!expect(record.finalized_by.round_changes.size() == 2,
                "construct two round-change votes") ||
        !expect(record.finalized_by.votes.size() == 2,
                "construct two round-two finalization votes")) {
        std::cerr << error << "\n";
        return 1;
    }

    std::uint64_t round = 0;
    if (!expect(primechain::protocol::verifyRoundChangeCertificate(
                    record.finalized_by, record.previous_record_hash,
                    record.integer, addresses(validators), round, error),
                "verify two-of-three round-change certificate") ||
        !expect(round == 2, "activate round two")) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto bytes = primechain::protocol::serializePrimeRecord(record);
    const auto decoded = primechain::protocol::deserializePrimeRecord(bytes, error);
    if (!expect(decoded.has_value(), "round-change record round trip") ||
        !expect(decoded->finalized_by.round_changes.size() == 2,
                "round-change votes survive serialization")) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto candidate_hash = primechain::protocol::candidateRecordHash(record);
    if (!expect(primechain::protocol::verifyRecordFinalization(
                    record.finalized_by, candidate_hash,
                    record.previous_record_hash, record.integer,
                    addresses(validators), error),
                "verify round-two record finalization")) {
        std::cerr << error << "\n";
        return 1;
    }

    auto one_change = record.finalized_by;
    one_change.round_changes.pop_back();
    error.clear();
    if (!expect(!primechain::protocol::verifyRecordFinalization(
                    one_change, candidate_hash, record.previous_record_hash,
                    record.integer, addresses(validators), error),
                "reject one-vote round change")) {
        return 1;
    }

    auto tampered = record.finalized_by;
    tampered.round_changes[0].signature[0] ^= 0x01;
    error.clear();
    if (!expect(!primechain::protocol::verifyRecordFinalization(
                    tampered, candidate_hash, record.previous_record_hash,
                    record.integer, addresses(validators), error),
                "reject tampered round-change signature")) {
        return 1;
    }

    auto wrong_frontier = record.finalized_by;
    wrong_frontier.round_changes[0].integer += 1;
    error.clear();
    if (!expect(!primechain::protocol::verifyRecordFinalization(
                    wrong_frontier, candidate_hash, record.previous_record_hash,
                    record.integer, addresses(validators), error),
                "reject round change for another integer")) {
        return 1;
    }

    primechain::storage::RoundChangeStore round_store(round_path);
    if (!expect(round_store.replaceAll(record.finalized_by.round_changes, error),
                "persist round-change votes")) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto loaded_rounds = round_store.loadAll(error);
    if (!expect(error.empty() && loaded_rounds.size() == 2,
                "reload round-change votes") ||
        !expect(loaded_rounds[0].signature ==
                    record.finalized_by.round_changes[0].signature,
                "preserve round-change signature")) {
        return 1;
    }

    error.clear();
    if (!expect(node.appendPrime(record, error),
                "append round-two finalized record")) {
        std::cerr << error << "\n";
        return 1;
    }
    std::remove(round_path.c_str());
    primechain::node::SequentialNode replayed(chain_path);
    error.clear();
    if (!expect(replayed.load(error),
                "replay round-change record without sidecar") ||
        !expect(replayed.status().frontier_integer == 3,
                "round-change replay advances frontier")) {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << "round-change tests passed\n";
    return 0;
}
