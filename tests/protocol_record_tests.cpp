#include <iostream>
#include <string>

#include "primechain/crypto/hash.hpp"
#include "primechain/protocol/records.hpp"

namespace {

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

primechain::protocol::CompositeRecordV0 makeCompositeRecord() {
    primechain::protocol::CompositeRecordV0 record;
    record.version = 0;
    record.height = 2;
    record.integer = 4;
    record.proof.g = 4;
    record.proof.d = 2;
    record.proof.e = 2;
    record.proof.provider_address = "pcdev1_composite_miner";
    record.tx_batch.transaction_count = 10;
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

primechain::protocol::PrimeRecordV0 makePrimeRecord() {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = 3;
    record.integer = 5;
    record.proof.p = 5;
    record.proof.witness = 2;
    record.proof.factors_of_p_minus_1.push_back({2, 2});
    record.proof.provider_address = "pcdev1_prime_miner";
    record.tx_batch.transaction_count = 3;
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

} // namespace

int main() {
    using primechain::crypto::toHex;
    using namespace primechain::protocol;

    if (!expect(isDevelopmentAddress("pcdev1_alice"), "valid dev address")) {
        return 1;
    }
    if (!expect(!isDevelopmentAddress("alice"), "reject missing dev prefix")) {
        return 1;
    }
    if (!expect(!isDevelopmentAddress("pcdev1_bad address"), "reject space in dev address")) {
        return 1;
    }

    TransactionV0 tx;
    tx.version = 0;
    tx.inputs.push_back({5, {1, 5}});
    tx.outputs.push_back({5, {1, 5}, "pcdev1_bob"});
    tx.fee = {5, {0, 1}};
    tx.nonce = 1;
    tx.sender_public_key = {9, 8, 7, 6};
    tx.sender_address = developmentAddressFromPublicKey(tx.sender_public_key);
    tx.signature = developmentTransactionSignature(tx);

    const auto tx_hash_a = transactionHash(tx);
    const auto tx_hash_b = transactionHash(tx);
    if (!expect(tx_hash_a == tx_hash_b, "transaction hash is deterministic")) {
        return 1;
    }

    tx.nonce = 2;
    if (!expect(tx_hash_a != transactionHash(tx), "transaction hash changes when nonce changes")) {
        return 1;
    }
    tx.nonce = 1;
    tx.signature = developmentTransactionSignature(tx);
    std::string tx_decode_error;
    const auto decoded_tx = deserializeTransaction(serializeTransaction(tx, true), tx_decode_error);
    if (!expect(decoded_tx.has_value(), "deserialize transaction")) {
        std::cerr << tx_decode_error << "\n";
        return 1;
    }
    if (!expect(verifyDevelopmentTransactionSignature(*decoded_tx), "verify development transaction signature")) {
        return 1;
    }

    const auto composite = makeCompositeRecord();
    const auto composite_hash_a = candidateRecordHash(composite);
    const auto composite_hash_b = candidateRecordHash(composite);
    if (!expect(composite_hash_a == composite_hash_b, "composite candidate hash is deterministic")) {
        return 1;
    }
    std::string decode_error;
    const auto decoded_composite = deserializeCompositeRecord(serializeCompositeRecord(composite), decode_error);
    if (!expect(decoded_composite.has_value(), "deserialize composite record")) {
        std::cerr << decode_error << "\n";
        return 1;
    }
    if (!expect(decoded_composite->integer == composite.integer &&
                    decoded_composite->proof.d == composite.proof.d &&
                    decoded_composite->proof.e == composite.proof.e,
                "composite record round trip")) {
        return 1;
    }

    auto composite_changed = composite;
    composite_changed.tx_batch.transaction_count = 11;
    if (!expect(composite_hash_a != candidateRecordHash(composite_changed), "composite hash changes when tx count changes")) {
        return 1;
    }

    auto composite_voted = composite;
    const auto vote = makeDevelopmentVote("pcdev1_validator_c", composite_hash_a, 1);
    composite_voted.finalized_by.votes.push_back(vote);
    if (!expect(candidateRecordHash(composite) == candidateRecordHash(composite_voted), "candidate hash ignores votes")) {
        return 1;
    }
    if (!expect(finalizedRecordHash(composite) != finalizedRecordHash(composite_voted), "finalized hash includes votes")) {
        return 1;
    }
    std::string vote_error;
    if (!expect(verifyDevelopmentFinalization(composite.finalized_by, composite_hash_a, vote_error), "valid development finalization")) {
        std::cerr << vote_error << "\n";
        return 1;
    }
    auto duplicate_vote = composite;
    duplicate_vote.finalized_by.votes[1].validator_address = duplicate_vote.finalized_by.votes[0].validator_address;
    duplicate_vote.finalized_by.votes[1].signature = developmentVoteSignature(
        duplicate_vote.finalized_by.votes[1].validator_address,
        duplicate_vote.finalized_by.votes[1].record_hash,
        duplicate_vote.finalized_by.votes[1].round);
    vote_error.clear();
    if (!expect(!verifyDevelopmentFinalization(duplicate_vote.finalized_by, composite_hash_a, vote_error), "reject duplicate development validator")) {
        return 1;
    }

    const auto prime = makePrimeRecord();
    if (!expect(candidateRecordHash(prime) == candidateRecordHash(prime), "prime candidate hash is deterministic")) {
        return 1;
    }
    decode_error.clear();
    const auto decoded_prime = deserializePrimeRecord(serializePrimeRecord(prime), decode_error);
    if (!expect(decoded_prime.has_value(), "deserialize prime record")) {
        std::cerr << decode_error << "\n";
        return 1;
    }
    if (!expect(decoded_prime->integer == prime.integer &&
                    decoded_prime->proof.witness == prime.proof.witness &&
                    decoded_prime->proof.factors_of_p_minus_1.size() == prime.proof.factors_of_p_minus_1.size(),
                "prime record round trip")) {
        return 1;
    }

    auto prime_changed = prime;
    prime_changed.proof.witness = 3;
    if (!expect(candidateRecordHash(prime) != candidateRecordHash(prime_changed), "prime hash changes when witness changes")) {
        return 1;
    }

    std::cout << "protocol record tests passed\n";
    std::cout << "sample_composite_candidate_hash=" << toHex(composite_hash_a).substr(0, 16) << "\n";
    return 0;
}
