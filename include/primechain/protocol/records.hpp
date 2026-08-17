#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "primechain/types.hpp"

namespace primechain::protocol {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t kBinaryTransactionMerkleRecordVersion = 12;

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
    std::uint64_t locked_round{0};
    std::string locked_candidate_kind;
    Hash256 locked_candidate_hash{};
    Bytes locked_candidate_payload;
    Bytes signature;
};

struct FinalizationProofV0 {
    std::string rule{"fixed-2-of-3-dev"};
    std::vector<RoundChangeVoteV1> round_changes;
    std::vector<ValidatorVoteV0> votes;
};

struct CompositeLotteryProofV1 {
    std::uint64_t round{0};
    std::uint64_t win_bps{0};
    Hash256 subject_hash{};
    Address assigned_validator;
    Bytes public_key;
    Bytes signature;
};

struct GenesisConfigV1 {
    std::vector<Address> validator_set;
    std::string genesis_message;
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

struct ValidatorEndpointUpdateV1 {
    Address validator_address;
    std::string host;
    std::uint64_t port{0};
    PrimeValue effective_integer{0};
    std::uint64_t sequence{0};
    Bytes public_key;
    Bytes signature;
};

struct ValidatorApplicationV1 {
    Address candidate_address;
    std::string host;
    std::uint64_t port{0};
    PrimeValue record_integer{0};
    std::uint64_t sequence{0};
    std::uint64_t observed_successful{0};
    std::uint64_t observed_total{0};
    Bytes public_key;
    Bytes signature;
};

struct ValidatorWorkBindingV1 {
    Address candidate_address;
    Address miner_address;
    PrimeValue record_integer{0};
    std::uint64_t sequence{0};
    Bytes miner_public_key;
    Bytes miner_signature;
};

struct EconomicPolicyVoteV1 {
    Address validator_address;
    Bytes public_key;
    Bytes signature;
};

struct EconomicPolicyUpdateV1 {
    std::uint64_t transfer_fee_micro_units{0};
    std::uint64_t validator_min_reserve_micro_units{0};
    PrimeValue effective_integer{0};
    std::uint64_t sequence{0};
    std::vector<EconomicPolicyVoteV1> votes;
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
    std::vector<ValidatorEndpointUpdateV1> validator_endpoints;
    EconomicPolicyUpdateV1 economic_policy;
    std::vector<ValidatorApplicationV1> validator_applications;
    std::vector<ValidatorWorkBindingV1> validator_work_bindings;
    CompositeLotteryProofV1 composite_lottery;
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
    std::vector<ValidatorEndpointUpdateV1> validator_endpoints;
    EconomicPolicyUpdateV1 economic_policy;
    std::vector<ValidatorApplicationV1> validator_applications;
    std::vector<ValidatorWorkBindingV1> validator_work_bindings;
    FinalizationProofV0 finalized_by;
};

bool isDevelopmentAddress(const Address& address);
bool isProtocolFeePoolAddress(const Address& address);
bool isProtocolValidatorRewardPoolAddress(const Address& address);
bool isProtocolValidatorReserveAddress(const Address& address);
bool isProtocolAddress(const Address& address);
Address validatorFeePoolAddress(std::uint64_t validator_epoch);
Address validatorRewardPoolAddress(std::uint64_t validator_epoch);
Address validatorReserveAddress(const Address& validator_address);
std::optional<Address> validatorAddressFromReserveAddress(const Address& reserve_address);
Address developmentAddressFromPublicKey(const Bytes& public_key);

std::vector<std::uint8_t> serializeTransaction(const TransactionV0& tx, bool include_signature);
std::vector<std::uint8_t> serializeCompositeRecord(const CompositeRecordV0& record);
std::vector<std::uint8_t> serializePrimeRecord(const PrimeRecordV0& record);
std::optional<TransactionV0> deserializeTransaction(const std::vector<std::uint8_t>& bytes, std::string& error);
std::optional<CompositeRecordV0> deserializeCompositeRecord(const std::vector<std::uint8_t>& bytes, std::string& error);
std::optional<PrimeRecordV0> deserializePrimeRecord(const std::vector<std::uint8_t>& bytes, std::string& error);

Hash256 transactionHash(const TransactionV0& tx);
Hash256 transactionMerkleRoot(const std::vector<TransactionV0>& transactions);
Hash256 transactionBatchRoot(const std::vector<TransactionV0>& transactions, std::uint64_t record_version);
void updateTransactionBatch(CompositeRecordV0& record);
void updateTransactionBatch(PrimeRecordV0& record);
Hash256 compositeLotterySubjectHash(const CompositeRecordV0& record);
std::optional<Address> assignedCompositeLotteryValidator(
    const CompositeRecordV0& record,
    const std::vector<Address>& validator_set,
    std::string& error);
bool verifyCompositeLotteryProof(
    const CompositeRecordV0& record,
    const std::vector<Address>& validator_set,
    std::string& error);
Hash256 subjectRecordHash(const CompositeRecordV0& record);
Hash256 subjectRecordHash(const PrimeRecordV0& record);
Hash256 canonicalStoredRecordHash(const CompositeRecordV0& record);
Hash256 canonicalStoredRecordHash(const PrimeRecordV0& record);
Hash256 candidateRecordHash(const CompositeRecordV0& record);
Hash256 candidateRecordHash(const PrimeRecordV0& record);
Hash256 legacyCandidateRecordHashWithoutFinalization(const CompositeRecordV0& record);
Hash256 legacyCandidateRecordHashWithoutFinalization(const PrimeRecordV0& record);
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
bool verifyValidatorEndpointUpdates(
    const std::vector<ValidatorEndpointUpdateV1>& updates,
    const std::vector<Address>& current_validator_set,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error);
bool verifyEconomicPolicyUpdate(
    const EconomicPolicyUpdateV1& update,
    const std::vector<Address>& current_validator_set,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error);
bool verifyValidatorApplications(
    const std::vector<ValidatorApplicationV1>& applications,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error);
bool verifyValidatorWorkBindings(
    const std::vector<ValidatorWorkBindingV1>& bindings,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error);

} // namespace primechain::protocol
