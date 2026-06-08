#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/node/sequential_node.hpp"
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

std::optional<primechain::protocol::CompositeRecordV0> makeCertifiedCompositeRecord(
    std::string& error) {
    using namespace primechain;

    const auto miner = crypto::generateProtocolSignatureKeyPair(error);
    if (!miner.has_value()) {
        return std::nullopt;
    }
    const Address miner_address = crypto::addressFromProtocolPublicKey(miner->public_key);
    constexpr PrimeValue integer = 4;
    constexpr PrimeValue divisor = 2;
    constexpr std::uint64_t nonce = 77;
    const Hash256 commitment_hash = crypto::compositeCommitment(
        integer, divisor, divisor, nonce, miner_address);
    const auto commitment_signature = crypto::signProtocolMessage(
        miner->private_key,
        crypto::compositeCommitSigningPayload(integer, commitment_hash, miner_address),
        error);
    if (!commitment_signature.has_value()) {
        return std::nullopt;
    }
    const auto reveal_signature = crypto::signProtocolMessage(
        miner->private_key,
        crypto::compositeRevealSigningPayload(
            integer, divisor, divisor, nonce, miner_address),
        error);
    if (!reveal_signature.has_value()) {
        return std::nullopt;
    }

    struct ValidatorIdentity {
        Address address;
        crypto::SignatureKeyPair keys;
    };
    std::vector<ValidatorIdentity> validators;
    for (int i = 0; i < 3; ++i) {
        const auto keys = crypto::generateProtocolSignatureKeyPair(error);
        if (!keys.has_value()) {
            return std::nullopt;
        }
        validators.push_back({
            crypto::addressFromProtocolPublicKey(keys->public_key), *keys});
    }
    std::sort(validators.begin(), validators.end(),
        [](const ValidatorIdentity& left, const ValidatorIdentity& right) {
            return left.address < right.address;
        });

    protocol::CompositeRecordV0 record;
    record.version = 1;
    record.height = 2;
    record.integer = integer;
    record.proof.g = integer;
    record.proof.d = divisor;
    record.proof.e = divisor;
    record.proof.provider_address = miner_address;
    record.proof.signature = crypto::packCompositeRevealProof(
        miner->public_key, nonce, *reveal_signature);
    record.commit_phase.integer = integer;
    for (const auto& validator : validators) {
        record.commit_phase.validator_set.push_back(validator.address);
    }
    record.commit_phase.commitments.push_back({
        commitment_hash, miner_address, miner->public_key, *commitment_signature});
    record.commit_phase.snapshot_hash = protocol::commitPhaseSnapshotHash(
        integer, record.commit_phase.commitments);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto signature = crypto::signProtocolMessage(
            validators[i].keys.private_key,
            crypto::commitPhaseVoteSigningPayload(
                integer, record.commit_phase.snapshot_hash, validators[i].address),
            error);
        if (!signature.has_value()) {
            return std::nullopt;
        }
        record.commit_phase.votes.push_back({
            validators[i].address, validators[i].keys.public_key, *signature});
    }
    protocol::applyDevelopmentFinalization(record);
    return record;
}


} // namespace

int main() {
    using primechain::crypto::toHex;
    using namespace primechain::protocol;

    if (!expect(
            toHex(primechain::crypto::sha3_256({})) ==
                "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
            "SHA3-256 empty-string test vector")) {
        return 1;
    }
    const std::vector<std::uint8_t> abc{'a', 'b', 'c'};
    if (!expect(
            toHex(primechain::crypto::sha3_256(abc)) ==
                "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
            "SHA3-256 abc test vector")) {
        return 1;
    }

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

    std::string auth_error;
    const auto account = primechain::crypto::generateProtocolSignatureKeyPair(auth_error);
    if (!expect(account.has_value(), "generate authenticated transaction key")) {
        std::cerr << auth_error << "\n";
        return 1;
    }
    TransactionV0 authenticated_tx;
    authenticated_tx.version = 2;
    authenticated_tx.inputs.push_back({5, {100, 1}});
    authenticated_tx.outputs.push_back({5, {100, 1}, "pcdev1_receiver"});
    authenticated_tx.fee = {5, {0, 1}};
    authenticated_tx.nonce = 9;
    authenticated_tx.sender_public_key = account->public_key;
    authenticated_tx.sender_address =
        primechain::crypto::addressFromProtocolPublicKey(account->public_key);
    const auto authenticated_signature = primechain::crypto::signProtocolMessage(
        account->private_key,
        primechain::crypto::transactionSigningPayload(
            serializeTransaction(authenticated_tx, false)),
        auth_error);
    if (!expect(authenticated_signature.has_value(), "sign authenticated transaction")) return 1;
    authenticated_tx.signature = *authenticated_signature;
    if (!expect(verifyAuthenticatedTransactionSignature(authenticated_tx, auth_error),
                "verify authenticated transaction")) return 1;
    auto tampered_tx = authenticated_tx;
    tampered_tx.outputs[0].amount.numerator += 1;
    auth_error.clear();
    if (!expect(!verifyAuthenticatedTransactionSignature(tampered_tx, auth_error),
                "reject tampered authenticated transaction")) return 1;

    const primechain::Hash256 previous_hash = primechain::crypto::sha3_256({1, 2, 3});
    const std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors{{2, 2}};
    const auto prime_signature = primechain::crypto::signProtocolMessage(
        account->private_key,
        primechain::crypto::primeProofSigningPayload(
            previous_hash, 5, 2, factors, authenticated_tx.sender_address),
        auth_error);
    if (!expect(prime_signature.has_value(), "sign prime proof provider")) return 1;
    const auto packed_prime_auth = primechain::crypto::packPrimeProofAuthentication(
        account->public_key, *prime_signature);
    if (!expect(primechain::crypto::verifyPackedPrimeProofAuthentication(
                    previous_hash, 5, 2, factors, authenticated_tx.sender_address,
                    packed_prime_auth, auth_error),
                "verify prime proof provider")) return 1;
    auto wrong_previous = previous_hash;
    wrong_previous[0] ^= 1;
    auth_error.clear();
    if (!expect(!primechain::crypto::verifyPackedPrimeProofAuthentication(
                    wrong_previous, 5, 2, factors, authenticated_tx.sender_address,
                    packed_prime_auth, auth_error),
                "reject prime proof replay on another frontier")) return 1;

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
    std::string certificate_error;
    const auto certified = makeCertifiedCompositeRecord(certificate_error);
    if (!expect(certified.has_value(), "construct embedded commit-phase certificate")) {
        std::cerr << certificate_error << "\n";
        return 1;
    }
    if (!expect(verifyCommitPhaseCertificate(*certified, certificate_error),
                "verify embedded commit-phase certificate")) {
        std::cerr << certificate_error << "\n";
        return 1;
    }
    decode_error.clear();
    const auto decoded_certified = deserializeCompositeRecord(
        serializeCompositeRecord(*certified), decode_error);
    if (!expect(decoded_certified.has_value(), "embedded certificate round trip")) {
        std::cerr << decode_error << "\n";
        return 1;
    }
    certificate_error.clear();
    if (!expect(verifyCommitPhaseCertificate(*decoded_certified, certificate_error),
                "verify round-tripped embedded certificate")) {
        std::cerr << certificate_error << "\n";
        return 1;
    }
    auto bad_snapshot = *certified;
    bad_snapshot.commit_phase.snapshot_hash[0] ^= 0x01;
    certificate_error.clear();
    if (!expect(!verifyCommitPhaseCertificate(bad_snapshot, certificate_error),
                "reject altered commit-phase snapshot")) {
        return 1;
    }
    auto bad_vote = *certified;
    bad_vote.commit_phase.votes[0].signature[0] ^= 0x01;
    certificate_error.clear();
    if (!expect(!verifyCommitPhaseCertificate(bad_vote, certificate_error),
                "reject altered validator signature")) {
        return 1;
    }
    auto bad_reveal = *certified;
    bad_reveal.proof.signature[40] ^= 0x01;
    certificate_error.clear();
    if (!expect(!verifyCommitPhaseCertificate(bad_reveal, certificate_error),
                "reject reveal not authorized by winning commitment")) {
        return 1;
    }

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
    auto signed_record = composite;
    signed_record.finalized_by.rule = "fixed-2-of-3-mldsa65-v2";
    signed_record.finalized_by.votes.clear();
    std::vector<primechain::crypto::SignatureKeyPair> validator_keys;
    std::vector<primechain::Address> validator_set;
    for (int i = 0; i < 3; ++i) {
        std::string key_error;
        const auto key = primechain::crypto::generateProtocolSignatureKeyPair(key_error);
        if (!expect(key.has_value(), "generate finalization validator key")) return 1;
        validator_set.push_back(primechain::crypto::addressFromProtocolPublicKey(key->public_key));
        validator_keys.push_back(*key);
    }
    std::vector<std::size_t> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return validator_set[left] < validator_set[right];
    });
    std::vector<primechain::Address> sorted_validators;
    for (const auto index : order) sorted_validators.push_back(validator_set[index]);
    const auto signed_hash = candidateRecordHash(signed_record);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto index = order[i];
        auto signed_vote = makeSignedValidatorVote(
            validator_set[index], validator_keys[index].public_key,
            validator_keys[index].private_key, signed_hash, 1, vote_error);
        signed_record.finalized_by.votes.push_back(std::move(signed_vote));
    }
    if (!expect(
            verifyRecordFinalization(
                signed_record.finalized_by, signed_hash, signed_record.previous_record_hash,
                signed_record.integer, sorted_validators, vote_error),
            "valid signed two-of-three finalization")) {
        std::cerr << vote_error << "\n";
        return 1;
    }
    auto tampered_finalization = signed_record.finalized_by;
    tampered_finalization.votes[0].signature[0] ^= 0x01;
    vote_error.clear();
    if (!expect(
            !verifyRecordFinalization(
                tampered_finalization, signed_hash, signed_record.previous_record_hash,
                signed_record.integer, sorted_validators, vote_error),
            "reject tampered finalization signature")) return 1;

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

    std::vector<primechain::Address> genesis_validators;
    for (int i = 0; i < 3; ++i) {
        certificate_error.clear();
        const auto keys = primechain::crypto::generateProtocolSignatureKeyPair(certificate_error);
        if (!expect(keys.has_value(), "generate genesis validator identity")) {
            std::cerr << certificate_error << "\n";
            return 1;
        }
        genesis_validators.push_back(
            primechain::crypto::addressFromProtocolPublicKey(keys->public_key));
    }
    const auto anchored_genesis = primechain::node::makeGenesisPrimeRecordV0(
        genesis_validators);
    std::string genesis_error;
    if (!expect(verifyGenesisConfig(anchored_genesis, genesis_error),
                "verify anchored genesis validator set")) {
        std::cerr << genesis_error << "\n";
        return 1;
    }
    const auto decoded_genesis = deserializePrimeRecord(
        serializePrimeRecord(anchored_genesis), genesis_error);
    if (!expect(decoded_genesis.has_value() &&
                    decoded_genesis->genesis_config.validator_set ==
                        anchored_genesis.genesis_config.validator_set,
                "anchored genesis round trip")) {
        std::cerr << genesis_error << "\n";
        return 1;
    }
    auto duplicate_genesis = anchored_genesis;
    duplicate_genesis.genesis_config.validator_set[1] =
        duplicate_genesis.genesis_config.validator_set[0];
    std::sort(duplicate_genesis.genesis_config.validator_set.begin(),
              duplicate_genesis.genesis_config.validator_set.end());
    genesis_error.clear();
    if (!expect(!verifyGenesisConfig(duplicate_genesis, genesis_error),
                "reject duplicate genesis validator")) {
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
