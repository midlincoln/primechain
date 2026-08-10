#include <iostream>
#include <string>

#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/storage/finalization_store.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::string error;
    const auto keys = primechain::crypto::generateProtocolSignatureKeyPair(error);
    if (!keys.has_value()) return 1;
    const auto address = primechain::crypto::addressFromProtocolPublicKey(keys->public_key);
    const primechain::Hash256 candidate = primechain::crypto::sha3_256({1, 2, 3});
    auto vote = primechain::protocol::makeSignedValidatorVote(
        address, keys->public_key, keys->private_key, candidate, 1, error);
    if (vote.signature.empty()) return 1;

    primechain::storage::SignedCandidateRecord signed_record;
    signed_record.integer = 17;
    signed_record.candidate_kind = "PRIME";
    signed_record.candidate_payload = {9, 8, 7};
    signed_record.vote = vote;

    primechain::storage::FinalizationStore store(argv[1]);
    if (!store.replaceAll({signed_record}, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto loaded = store.loadAll(error);
    if (!error.empty() || loaded.size() != 1 || loaded[0].integer != 17 ||
        loaded[0].vote.validator_address != vote.validator_address ||
        loaded[0].vote.public_key != vote.public_key ||
        loaded[0].candidate_kind != signed_record.candidate_kind ||
        loaded[0].candidate_payload != signed_record.candidate_payload ||
        loaded[0].vote.record_hash != vote.record_hash ||
        loaded[0].vote.round != vote.round || loaded[0].vote.signature != vote.signature) {
        std::cerr << "finalization store round trip failed\n";
        return 1;
    }
    if (!store.replaceAll({}, error) || !store.loadAll(error).empty()) {
        std::cerr << "finalization store clear failed\n";
        return 1;
    }
    std::cout << "finalization store tests passed\n";
    return 0;
}
