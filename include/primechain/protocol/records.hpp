#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::protocol {

using Bytes = std::vector<std::uint8_t>;

struct Amount {
    std::uint64_t numerator{0};
    std::uint64_t denominator{1};
};

struct TxInputV0 {
    PrimeValue prime{0};
    Amount amount;
};

struct TxOutputV0 {
    PrimeValue prime{0};
    Amount amount;
    Address receiver_address;
};

struct FeeSpecV0 {
    PrimeValue prime{0};
    Amount amount;
};

struct TransactionV0 {
    std::uint64_t version{0};
    std::vector<TxInputV0> inputs;
    std::vector<TxOutputV0> outputs;
    FeeSpecV0 fee;
    std::uint64_t nonce{0};
    Address sender_address;
    Bytes sender_public_key;
    Bytes signature;
};

struct TransactionBatchV0 {
    std::uint64_t transaction_count{0};
    Hash256 transaction_merkle_root{};
};

struct CompositeProofV0 {
    PrimeValue g{0};
    PrimeValue d{0};
    PrimeValue e{0};
    Address provider_address;
    Bytes signature;
};

struct PrimePowerV0 {
    PrimeValue prime{0};
    std::uint64_t exponent{0};
};

struct PrattPrimeProofV0 {
    PrimeValue p{0};
    PrimeValue witness{0};
    std::vector<PrimePowerV0> factors_of_p_minus_1;
    Address provider_address;
    Bytes signature;
};

struct ValidatorVoteV0 {
    Address validator_address;
    Bytes public_key;
    Hash256 record_hash{};
    std::uint64_t round{0};
    Bytes signature;
};

struct CommitCertificateEntryV1 {
    Hash256 commitment_hash{};
    Address provider_address;
    Bytes public_key;
    Bytes signature;
};

struct CommitCertificateVoteV1 {
    Address validator_address;
    Bytes public_key;
    Bytes signature;
};

struct CommitPhaseCertificateV1 {
    PrimeValue integer{0};
    Hash256 snapshot_hash{};
    std::vector<Address> validator_set;
    std::vector<CommitCertificateEntryV1> commitments;
    std::vector<CommitCertificateVoteV1> votes;
};

struct RoundChangeVoteV1 {
    Address validator_address;
    Bytes public_key;
    Hash256 previous_record_hash{};
    PrimeValue integer{0};
    std::uint64_t new_round{0};
    Bytes signature;
};

struct FinalizationProofV0 {
    std::string rule{"fixed-2-of-3-dev"};
    std::vector<RoundChangeVoteV1> round_changes;
    std::vector<ValidatorVoteV0> votes;
};

struct GenesisConfigV1 {
    std::vector<Address> validator_set;
};

struct ValidatorEpochVoteV1 {
    Address validator_address;
    Bytes public_key;
    Bytes signature;
};

struct ValidatorEpochTransitionV1 {
    std::uint64_t epoch{0};
    PrimeValue activation_integer{0};
    std::vector<Address> next_validator_set;
    std::vector<ValidatorEpochVoteV1> votes;
};

struct CompositeRecordV0 {
    std::uint64_t version{0};
    std::uint64_t height{0};
    Hash256 previous_record_hash{};
    PrimeValue integer{0};
    CompositeProofV0 proof;
    TransactionBatchV0 tx_batch;
    std::vector<TransactionV0> transactions;
    Hash256 state_root{};
    CommitPhaseCertificateV1 commit_phase;
    ValidatorEpochTransitionV1 validator_epoch;
    FinalizationProofV0 finalized_by;
};

struct PrimeRecordV0 {
    std::uint64_t version{0};
    std::uint64_t height{0};
    Hash256 previous_record_hash{};
    PrimeValue integer{0};
    PrattPrimeProofV0 proof;
    TransactionBatchV0 tx_batch;
    std::vector<TransactionV0> transactions;
    Hash256 state_root{};
    GenesisConfigV1 genesis_config;
    ValidatorEpochTransitionV1 validator_epoch;
    FinalizationProofV0 finalized_by;
};

bool isDevelopmentAddress(const Address& address);
bool isProtocolAddress(const Address& address);
Address developmentAddressFromPublicKey(const Bytes& public_key);

std::vector<std::uint8_t> serializeTransaction(const TransactionV0& tx, bool include_signature);
std::vector<std::uint8_t> serializeCompositeRecord(const CompositeRecordV0& record);
std::vector<std::uint8_t> serializePrimeRecord(const PrimeRecordV0& record);
std::optional<TransactionV0> deserializeTransaction(const std::vector<std::uint8_t>& bytes, std::string& error);
std::optional<CompositeRecordV0> deserializeCompositeRecord(const std::vector<std::uint8_t>& bytes, std::string& error);
std::optional<PrimeRecordV0> deserializePrimeRecord(const std::vector<std::uint8_t>& bytes, std::string& error);

Hash256 transactionHash(const TransactionV0& tx);
Hash256 transactionMerkleRoot(const std::vector<TransactionV0>& transactions);
void updateTransactionBatch(CompositeRecordV0& record);
void updateTransactionBatch(PrimeRecordV0& record);
Hash256 candidateRecordHash(const CompositeRecordV0& record);
Hash256 candidateRecordHash(const PrimeRecordV0& record);
Hash256 finalizedRecordHash(const CompositeRecordV0& record);
Hash256 finalizedRecordHash(const PrimeRecordV0& record);

Bytes developmentVoteSignature(const Address& validator_address, const Hash256& record_hash, std::uint64_t round);
ValidatorVoteV0 makeDevelopmentVote(const Address& validator_address, const Hash256& record_hash, std::uint64_t round);
Bytes developmentTransactionSignature(const TransactionV0& tx);
bool verifyDevelopmentTransactionSignature(const TransactionV0& tx);
bool verifyAuthenticatedTransactionSignature(const TransactionV0& tx, std::string& error);
void applyDevelopmentFinalization(CompositeRecordV0& record);
void applyDevelopmentFinalization(PrimeRecordV0& record);
bool verifyDevelopmentFinalization(const FinalizationProofV0& proof, const Hash256& candidate_hash, std::string& error);
ValidatorVoteV0 makeSignedValidatorVote(
    const Address& validator_address,
    const Bytes& public_key,
    const Bytes& private_key,
    const Hash256& record_hash,
    std::uint64_t round,
    std::string& error);
bool verifyRoundChangeCertificate(
    const FinalizationProofV0& proof,
    const Hash256& previous_record_hash,
    PrimeValue integer,
    const std::vector<Address>& validator_set,
    std::uint64_t& round,
    std::string& error);
bool verifyRecordFinalization(
    const FinalizationProofV0& proof,
    const Hash256& candidate_hash,
    const Hash256& previous_record_hash,
    PrimeValue integer,
    const std::vector<Address>& validator_set,
    std::string& error);
Hash256 commitPhaseSnapshotHash(
    PrimeValue integer,
    const std::vector<CommitCertificateEntryV1>& commitments);
bool verifyCommitPhaseCertificate(
    const CompositeRecordV0& record, std::string& error);
bool verifyGenesisConfig(const PrimeRecordV0& record, std::string& error);
bool verifyValidatorEpochTransition(
    const ValidatorEpochTransitionV1& transition,
    const std::vector<Address>& current_validator_set,
    std::uint64_t current_epoch,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error);

} // namespace primechain::protocol
