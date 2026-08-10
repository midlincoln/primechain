#include "primechain/protocol/records.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <set>
#include <string_view>

#include "primechain/core/consensus.hpp"
#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"

namespace primechain::protocol {

namespace {

constexpr std::uint64_t kCompositeRecordTag = 1;
constexpr std::uint64_t kPrimeRecordTag = 2;
constexpr std::string_view kDevelopmentFinalizationRule = "fixed-2-of-3-dev";
constexpr std::string_view kSignedFinalizationRule = "fixed-2-of-3-mldsa65-v2";
constexpr std::string_view kRoundFinalizationRule = "fixed-2-of-3-mldsa65-rounds-v3";
constexpr std::string_view kLockedRoundFinalizationRule = "fixed-2-of-3-mldsa65-rounds-locks-v4";
constexpr std::string_view kDevelopmentVoteDomain = "primechain-dev-vote-v0";
constexpr std::uint64_t kMaxDecodedPrattFactors = 1024;
constexpr std::uint64_t kMaxDecodedTransactions = 10000;
constexpr std::uint64_t kMaxDecodedTxInputs = 1024;
constexpr std::uint64_t kMaxDecodedTxOutputs = 1024;
constexpr std::uint64_t kMaxDecodedValidatorEndpointUpdates = 64;
constexpr std::uint64_t kMaxDecodedValidatorApplications = 64;
constexpr std::uint64_t kMaxDecodedValidatorWorkBindings = 64;
constexpr std::uint64_t kMaxDecodedEconomicPolicyVotes = 64;

bool isCanonicalProtocolValidatorSet(const std::vector<Address>& validators) {
    return core::validValidatorSetSize(validators.size()) &&
           std::is_sorted(validators.begin(), validators.end()) &&
           std::adjacent_find(validators.begin(), validators.end()) == validators.end() &&
           std::all_of(validators.begin(), validators.end(), crypto::isProtocolSignatureAddress);
}

bool hasQuorumVoteCount(std::size_t vote_count, std::size_t validator_count) {
    return vote_count >= core::requiredValidatorQuorum(validator_count) &&
           vote_count <= validator_count;
}

bool isZeroHash(const Hash256& hash) {
    return std::all_of(hash.begin(), hash.end(), [](std::uint8_t byte) { return byte == 0; });
}

class ByteReader;
void appendValidatorApplications(
    std::vector<std::uint8_t>& out,
    const std::vector<ValidatorApplicationV1>& applications);
bool readValidatorApplications(
    ByteReader& reader,
    std::vector<ValidatorApplicationV1>& applications);
void appendValidatorWorkBindings(
    std::vector<std::uint8_t>& out,
    const std::vector<ValidatorWorkBindingV1>& bindings);
bool readValidatorWorkBindings(
    ByteReader& reader,
    std::vector<ValidatorWorkBindingV1>& bindings);

class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& bytes)
        : bytes_(bytes) {}

    bool readUint64(std::uint64_t& value) {
        if (remaining() < 8) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << (i * 8);
        }
        return true;
    }

    bool readHash(Hash256& hash) {
        if (remaining() < hash.size()) {
            return false;
        }
        std::memcpy(hash.data(), bytes_.data() + offset_, hash.size());
        offset_ += hash.size();
        return true;
    }

    bool readBytes(Bytes& out) {
        std::uint64_t size = 0;
        if (!readUint64(size) || size > remaining()) {
            return false;
        }
        out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += static_cast<std::size_t>(size);
        return true;
    }

    bool readString(std::string& out) {
        Bytes bytes;
        if (!readBytes(bytes)) {
            return false;
        }
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    bool consumed() const {
        return offset_ == bytes_.size();
    }

    std::size_t offset() const {
        return offset_;
    }

    void setOffset(std::size_t offset) {
        offset_ = offset;
    }

private:
    std::size_t remaining() const {
        if (offset_ > bytes_.size()) return 0;
        return bytes_.size() - offset_;
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_{0};
};

void appendUint64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void appendHash(std::vector<std::uint8_t>& out, const Hash256& hash) {
    out.insert(out.end(), hash.begin(), hash.end());
}

void appendBytes(std::vector<std::uint8_t>& out, const Bytes& bytes) {
    appendUint64(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendString(std::vector<std::uint8_t>& out, std::string_view value) {
    appendUint64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void appendAddress(std::vector<std::uint8_t>& out, const Address& address) {
    appendString(out, address);
}

void appendAmount(std::vector<std::uint8_t>& out, const Amount& amount) {
    appendUint64(out, amount.numerator);
    appendUint64(out, amount.denominator);
}

void appendTxInput(std::vector<std::uint8_t>& out, const TxInputV0& input) {
    appendUint64(out, input.prime);
    appendAmount(out, input.amount);
}

void appendTxOutput(std::vector<std::uint8_t>& out, const TxOutputV0& output) {
    appendUint64(out, output.prime);
    appendAmount(out, output.amount);
    appendAddress(out, output.receiver_address);
}

void appendFeeSpec(std::vector<std::uint8_t>& out, const FeeSpecV0& fee) {
    appendUint64(out, fee.prime);
    appendAmount(out, fee.amount);
}

void appendTransactionBatch(std::vector<std::uint8_t>& out, const TransactionBatchV0& batch) {
    appendUint64(out, batch.transaction_count);
    appendHash(out, batch.transaction_merkle_root);
}

void appendTransactionList(std::vector<std::uint8_t>& out, const std::vector<TransactionV0>& transactions) {
    appendUint64(out, transactions.size());
    for (const auto& tx : transactions) {
        appendBytes(out, serializeTransaction(tx, true));
    }
}

void appendCompositeProof(std::vector<std::uint8_t>& out, const CompositeProofV0& proof) {
    appendUint64(out, proof.g);
    appendUint64(out, proof.d);
    appendUint64(out, proof.e);
    appendAddress(out, proof.provider_address);
    appendBytes(out, proof.signature);
}

void appendPrimePower(std::vector<std::uint8_t>& out, const PrimePowerV0& factor) {
    appendUint64(out, factor.prime);
    appendUint64(out, factor.exponent);
}

void appendPrattProof(std::vector<std::uint8_t>& out, const PrattPrimeProofV0& proof) {
    appendUint64(out, proof.p);
    appendUint64(out, proof.witness);
    appendUint64(out, proof.factors_of_p_minus_1.size());
    for (const auto& factor : proof.factors_of_p_minus_1) {
        appendPrimePower(out, factor);
    }
    appendAddress(out, proof.provider_address);
    appendBytes(out, proof.signature);
}

void appendValidatorVote(std::vector<std::uint8_t>& out, const ValidatorVoteV0& vote) {
    appendAddress(out, vote.validator_address);
    appendBytes(out, vote.public_key);
    appendHash(out, vote.record_hash);
    appendUint64(out, vote.round);
    appendBytes(out, vote.signature);
}

void appendRoundChangeVote(
    std::vector<std::uint8_t>& out,
    const RoundChangeVoteV1& vote) {
    appendAddress(out, vote.validator_address);
    appendBytes(out, vote.public_key);
    appendHash(out, vote.previous_record_hash);
    appendUint64(out, vote.integer);
    appendUint64(out, vote.new_round);
    appendBytes(out, vote.signature);
}

void appendLockedRoundChangeVote(
    std::vector<std::uint8_t>& out,
    const RoundChangeVoteV1& vote) {
    appendAddress(out, vote.validator_address);
    appendBytes(out, vote.public_key);
    appendHash(out, vote.previous_record_hash);
    appendUint64(out, vote.integer);
    appendUint64(out, vote.new_round);
    appendUint64(out, vote.locked_round);
    appendString(out, vote.locked_candidate_kind);
    appendHash(out, vote.locked_candidate_hash);
    appendBytes(out, vote.locked_candidate_payload);
    appendBytes(out, vote.signature);
}

void appendCompositeLotteryProof(std::vector<std::uint8_t>& out, const CompositeLotteryProofV1& proof) {
    appendUint64(out, proof.round);
    appendUint64(out, proof.win_bps);
    appendHash(out, proof.subject_hash);
    appendAddress(out, proof.assigned_validator);
    appendBytes(out, proof.public_key);
    appendBytes(out, proof.signature);
}

void appendFinalizationProof(
    std::vector<std::uint8_t>& out,
    const FinalizationProofV0& proof,
    bool include_votes) {
    appendString(out, proof.rule);
    if (proof.rule == kRoundFinalizationRule || proof.rule == kLockedRoundFinalizationRule) {
        appendUint64(out, proof.round_changes.size());
        for (const auto& vote : proof.round_changes) {
            if (proof.rule == kLockedRoundFinalizationRule) appendLockedRoundChangeVote(out, vote);
            else appendRoundChangeVote(out, vote);
        }
    }
    if (!include_votes) {
        appendUint64(out, 0);
        return;
    }
    appendUint64(out, proof.votes.size());
    for (const auto& vote : proof.votes) appendValidatorVote(out, vote);
}

void appendCommitCertificateEntry(
    std::vector<std::uint8_t>& out,
    const CommitCertificateEntryV1& entry) {
    appendHash(out, entry.commitment_hash);
    appendAddress(out, entry.provider_address);
    appendBytes(out, entry.public_key);
    appendBytes(out, entry.signature);
}

void appendCommitCertificateVote(
    std::vector<std::uint8_t>& out,
    const CommitCertificateVoteV1& vote) {
    appendAddress(out, vote.validator_address);
    appendBytes(out, vote.public_key);
    appendBytes(out, vote.signature);
}

void appendCommitPhaseCertificate(
    std::vector<std::uint8_t>& out,
    const CommitPhaseCertificateV1& certificate) {
    appendUint64(out, certificate.integer);
    appendHash(out, certificate.snapshot_hash);
    appendUint64(out, certificate.validator_set.size());
    for (const auto& validator : certificate.validator_set) appendAddress(out, validator);
    appendUint64(out, certificate.commitments.size());
    for (const auto& commitment : certificate.commitments) {
        appendCommitCertificateEntry(out, commitment);
    }
    appendUint64(out, certificate.votes.size());
    for (const auto& vote : certificate.votes) appendCommitCertificateVote(out, vote);
}

void appendGenesisConfig(
    std::vector<std::uint8_t>& out,
    const GenesisConfigV1& config) {
    appendUint64(out, config.validator_set.size());
    for (const auto& validator : config.validator_set) appendAddress(out, validator);
}

void appendValidatorEpochTransition(
    std::vector<std::uint8_t>& out,
    const ValidatorEpochTransitionV1& transition) {
    appendUint64(out, transition.epoch);
    appendUint64(out, transition.activation_integer);
    appendUint64(out, transition.next_validator_set.size());
    for (const auto& validator : transition.next_validator_set) appendAddress(out, validator);
    appendUint64(out, transition.votes.size());
    for (const auto& vote : transition.votes) {
        appendAddress(out, vote.validator_address);
        appendBytes(out, vote.public_key);
        appendBytes(out, vote.signature);
    }
}

void appendValidatorEndpointUpdates(
    std::vector<std::uint8_t>& out,
    const std::vector<ValidatorEndpointUpdateV1>& updates) {
    appendUint64(out, updates.size());
    for (const auto& update : updates) {
        appendAddress(out, update.validator_address);
        appendString(out, update.host);
        appendUint64(out, update.port);
        appendUint64(out, update.effective_integer);
        appendUint64(out, update.sequence);
        appendBytes(out, update.public_key);
        appendBytes(out, update.signature);
    }
}

void appendValidatorApplications(
    std::vector<std::uint8_t>& out,
    const std::vector<ValidatorApplicationV1>& applications) {
    appendUint64(out, applications.size());
    for (const auto& application : applications) {
        appendAddress(out, application.candidate_address);
        appendString(out, application.host);
        appendUint64(out, application.port);
        appendUint64(out, application.record_integer);
        appendUint64(out, application.sequence);
        appendUint64(out, application.observed_successful);
        appendUint64(out, application.observed_total);
        appendBytes(out, application.public_key);
        appendBytes(out, application.signature);
    }
}

void appendValidatorWorkBindings(
    std::vector<std::uint8_t>& out,
    const std::vector<ValidatorWorkBindingV1>& bindings) {
    appendUint64(out, bindings.size());
    for (const auto& binding : bindings) {
        appendAddress(out, binding.candidate_address);
        appendAddress(out, binding.miner_address);
        appendUint64(out, binding.record_integer);
        appendUint64(out, binding.sequence);
        appendBytes(out, binding.miner_public_key);
        appendBytes(out, binding.miner_signature);
    }
}

void appendEconomicPolicyUpdate(
    std::vector<std::uint8_t>& out,
    const EconomicPolicyUpdateV1& update,
    std::uint64_t record_version) {
    appendUint64(out, update.transfer_fee_micro_units);
    if (record_version >= 7) appendUint64(out, update.validator_min_reserve_micro_units);
    appendUint64(out, update.effective_integer);
    appendUint64(out, update.sequence);
    appendUint64(out, update.votes.size());
    for (const auto& vote : update.votes) {
        appendAddress(out, vote.validator_address);
        appendBytes(out, vote.public_key);
        appendBytes(out, vote.signature);
    }
}

bool readTransactionBatch(ByteReader& reader, TransactionBatchV0& batch) {
    return reader.readUint64(batch.transaction_count) &&
           reader.readHash(batch.transaction_merkle_root);
}

bool readAmount(ByteReader& reader, Amount& amount) {
    return reader.readUint64(amount.numerator) &&
           reader.readUint64(amount.denominator);
}

bool readTransactionList(ByteReader& reader, std::vector<TransactionV0>& transactions, std::string& error) {
    std::uint64_t count = 0;
    if (!reader.readUint64(count) || count > kMaxDecodedTransactions) {
        return false;
    }
    transactions.clear();
    transactions.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        Bytes bytes;
        if (!reader.readBytes(bytes)) {
            return false;
        }
        auto tx = deserializeTransaction(bytes, error);
        if (!tx.has_value()) {
            return false;
        }
        transactions.push_back(*tx);
    }
    return true;
}

bool readCompositeProof(ByteReader& reader, CompositeProofV0& proof) {
    return reader.readUint64(proof.g) &&
           reader.readUint64(proof.d) &&
           reader.readUint64(proof.e) &&
           reader.readString(proof.provider_address) &&
           reader.readBytes(proof.signature);
}

bool readPrimePower(ByteReader& reader, PrimePowerV0& factor) {
    return reader.readUint64(factor.prime) &&
           reader.readUint64(factor.exponent);
}

bool readPrattProof(ByteReader& reader, PrattPrimeProofV0& proof) {
    std::uint64_t factor_count = 0;
    if (!reader.readUint64(proof.p) ||
        !reader.readUint64(proof.witness) ||
        !reader.readUint64(factor_count) ||
        factor_count > kMaxDecodedPrattFactors) {
        return false;
    }
    proof.factors_of_p_minus_1.clear();
    proof.factors_of_p_minus_1.reserve(static_cast<std::size_t>(factor_count));
    for (std::uint64_t i = 0; i < factor_count; ++i) {
        PrimePowerV0 factor;
        if (!readPrimePower(reader, factor)) {
            return false;
        }
        proof.factors_of_p_minus_1.push_back(factor);
    }
    return reader.readString(proof.provider_address) &&
           reader.readBytes(proof.signature);
}

bool readValidatorVote(ByteReader& reader, ValidatorVoteV0& vote) {
    return reader.readString(vote.validator_address) &&
           reader.readBytes(vote.public_key) &&
           reader.readHash(vote.record_hash) &&
           reader.readUint64(vote.round) &&
           reader.readBytes(vote.signature);
}

bool readCompositeLotteryProof(ByteReader& reader, CompositeLotteryProofV1& proof) {
    return reader.readUint64(proof.round) &&
           reader.readUint64(proof.win_bps) &&
           reader.readHash(proof.subject_hash) &&
           reader.readString(proof.assigned_validator) &&
           reader.readBytes(proof.public_key) &&
           reader.readBytes(proof.signature);
}

bool readCommitPhaseCertificate(ByteReader& reader, CommitPhaseCertificateV1& certificate) {
    std::uint64_t validator_count = 0;
    std::uint64_t commitment_count = 0;
    std::uint64_t vote_count = 0;
    if (!reader.readUint64(certificate.integer) ||
        !reader.readHash(certificate.snapshot_hash) ||
        !reader.readUint64(validator_count) || validator_count > 16) return false;
    certificate.validator_set.clear();
    for (std::uint64_t i = 0; i < validator_count; ++i) {
        Address validator;
        if (!reader.readString(validator)) return false;
        certificate.validator_set.push_back(std::move(validator));
    }
    if (!reader.readUint64(commitment_count) || commitment_count > 1024) return false;
    certificate.commitments.clear();
    for (std::uint64_t i = 0; i < commitment_count; ++i) {
        CommitCertificateEntryV1 entry;
        if (!reader.readHash(entry.commitment_hash) ||
            !reader.readString(entry.provider_address) ||
            !reader.readBytes(entry.public_key) || !reader.readBytes(entry.signature)) return false;
        certificate.commitments.push_back(std::move(entry));
    }
    if (!reader.readUint64(vote_count) || vote_count > 16) return false;
    certificate.votes.clear();
    for (std::uint64_t i = 0; i < vote_count; ++i) {
        CommitCertificateVoteV1 vote;
        if (!reader.readString(vote.validator_address) ||
            !reader.readBytes(vote.public_key) || !reader.readBytes(vote.signature)) return false;
        certificate.votes.push_back(std::move(vote));
    }
    return true;
}

bool readGenesisConfig(ByteReader& reader, GenesisConfigV1& config) {
    std::uint64_t validator_count = 0;
    if (!reader.readUint64(validator_count) || validator_count > 16) return false;
    config.validator_set.clear();
    for (std::uint64_t i = 0; i < validator_count; ++i) {
        Address validator;
        if (!reader.readString(validator)) return false;
        config.validator_set.push_back(std::move(validator));
    }
    return true;
}

bool readValidatorEpochTransition(
    ByteReader& reader,
    ValidatorEpochTransitionV1& transition) {
    std::uint64_t validator_count = 0;
    std::uint64_t vote_count = 0;
    if (!reader.readUint64(transition.epoch) ||
        !reader.readUint64(transition.activation_integer) ||
        !reader.readUint64(validator_count) || validator_count > 16) return false;
    transition.next_validator_set.clear();
    for (std::uint64_t i = 0; i < validator_count; ++i) {
        Address validator;
        if (!reader.readString(validator)) return false;
        transition.next_validator_set.push_back(std::move(validator));
    }
    if (!reader.readUint64(vote_count) || vote_count > 16) return false;
    transition.votes.clear();
    for (std::uint64_t i = 0; i < vote_count; ++i) {
        ValidatorEpochVoteV1 vote;
        if (!reader.readString(vote.validator_address) ||
            !reader.readBytes(vote.public_key) ||
            !reader.readBytes(vote.signature)) return false;
        transition.votes.push_back(std::move(vote));
    }
    return true;
}

bool readValidatorEndpointUpdates(
    ByteReader& reader,
    std::vector<ValidatorEndpointUpdateV1>& updates) {
    std::uint64_t count = 0;
    if (!reader.readUint64(count) || count > kMaxDecodedValidatorEndpointUpdates) return false;
    updates.clear();
    updates.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        ValidatorEndpointUpdateV1 update;
        if (!reader.readString(update.validator_address) ||
            !reader.readString(update.host) ||
            !reader.readUint64(update.port) ||
            !reader.readUint64(update.effective_integer) ||
            !reader.readUint64(update.sequence) ||
            !reader.readBytes(update.public_key) ||
            !reader.readBytes(update.signature)) return false;
        updates.push_back(std::move(update));
    }
    return true;
}

bool readValidatorApplications(
    ByteReader& reader,
    std::vector<ValidatorApplicationV1>& applications) {
    std::uint64_t count = 0;
    if (!reader.readUint64(count) || count > kMaxDecodedValidatorApplications) return false;
    applications.clear();
    applications.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        ValidatorApplicationV1 application;
        if (!reader.readString(application.candidate_address) ||
            !reader.readString(application.host) ||
            !reader.readUint64(application.port) ||
            !reader.readUint64(application.record_integer) ||
            !reader.readUint64(application.sequence) ||
            !reader.readUint64(application.observed_successful) ||
            !reader.readUint64(application.observed_total) ||
            !reader.readBytes(application.public_key) ||
            !reader.readBytes(application.signature)) return false;
        applications.push_back(std::move(application));
    }
    return true;
}

bool readValidatorWorkBindings(
    ByteReader& reader,
    std::vector<ValidatorWorkBindingV1>& bindings) {
    std::uint64_t count = 0;
    if (!reader.readUint64(count) || count > kMaxDecodedValidatorWorkBindings) return false;
    bindings.clear();
    bindings.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        ValidatorWorkBindingV1 binding;
        if (!reader.readString(binding.candidate_address) ||
            !reader.readString(binding.miner_address) ||
            !reader.readUint64(binding.record_integer) ||
            !reader.readUint64(binding.sequence) ||
            !reader.readBytes(binding.miner_public_key) ||
            !reader.readBytes(binding.miner_signature)) return false;
        bindings.push_back(std::move(binding));
    }
    return true;
}

bool readEconomicPolicyUpdate(
    ByteReader& reader,
    EconomicPolicyUpdateV1& update,
    std::uint64_t record_version) {
    std::uint64_t vote_count = 0;
    update = {};
    if (!reader.readUint64(update.transfer_fee_micro_units)) return false;
    if (record_version >= 7 && !reader.readUint64(update.validator_min_reserve_micro_units)) return false;
    if (!reader.readUint64(update.effective_integer) ||
        !reader.readUint64(update.sequence) ||
        !reader.readUint64(vote_count) ||
        vote_count > kMaxDecodedEconomicPolicyVotes) return false;
    update.votes.reserve(static_cast<std::size_t>(vote_count));
    for (std::uint64_t i = 0; i < vote_count; ++i) {
        EconomicPolicyVoteV1 vote;
        if (!reader.readString(vote.validator_address) ||
            !reader.readBytes(vote.public_key) ||
            !reader.readBytes(vote.signature)) return false;
        update.votes.push_back(std::move(vote));
    }
    return true;
}

bool readRoundChangeVote(ByteReader& reader, RoundChangeVoteV1& vote) {
    vote.locked_round = 0;
    vote.locked_candidate_kind.clear();
    vote.locked_candidate_hash = {};
    vote.locked_candidate_payload.clear();
    return reader.readString(vote.validator_address) &&
           reader.readBytes(vote.public_key) &&
           reader.readHash(vote.previous_record_hash) &&
           reader.readUint64(vote.integer) &&
           reader.readUint64(vote.new_round) &&
           reader.readBytes(vote.signature);
}

bool readLockedRoundChangeVote(ByteReader& reader, RoundChangeVoteV1& vote) {
    return reader.readString(vote.validator_address) &&
           reader.readBytes(vote.public_key) &&
           reader.readHash(vote.previous_record_hash) &&
           reader.readUint64(vote.integer) &&
           reader.readUint64(vote.new_round) &&
           reader.readUint64(vote.locked_round) &&
           reader.readString(vote.locked_candidate_kind) &&
           reader.readHash(vote.locked_candidate_hash) &&
           reader.readBytes(vote.locked_candidate_payload) &&
           reader.readBytes(vote.signature);
}

bool readFinalizationProof(ByteReader& reader, FinalizationProofV0& proof) {
    std::uint64_t round_change_count = 0;
    std::uint64_t vote_count = 0;
    if (!reader.readString(proof.rule)) return false;
    proof.round_changes.clear();
    if (proof.rule == kRoundFinalizationRule || proof.rule == kLockedRoundFinalizationRule) {
        if (!reader.readUint64(round_change_count) || round_change_count > 16) return false;
        for (std::uint64_t i = 0; i < round_change_count; ++i) {
            RoundChangeVoteV1 vote;
            if (proof.rule == kLockedRoundFinalizationRule) {
                if (!readLockedRoundChangeVote(reader, vote)) return false;
            } else if (!readRoundChangeVote(reader, vote)) return false;
            proof.round_changes.push_back(std::move(vote));
        }
    }
    if (!reader.readUint64(vote_count) || vote_count > 16) return false;
    proof.votes.clear();
    proof.votes.reserve(static_cast<std::size_t>(vote_count));
    for (std::uint64_t i = 0; i < vote_count; ++i) {
        ValidatorVoteV0 vote;
        if (!readValidatorVote(reader, vote)) return false;
        proof.votes.push_back(std::move(vote));
    }
    return true;
}

bool readOptionalMetadataAndFinalization(
    ByteReader& reader,
    std::uint64_t version,
    std::vector<ValidatorEndpointUpdateV1>& updates,
    EconomicPolicyUpdateV1& policy,
    std::vector<ValidatorApplicationV1>& applications,
    std::vector<ValidatorWorkBindingV1>& work_bindings,
    FinalizationProofV0& finalization) {
    policy = {};
    applications.clear();
    work_bindings.clear();
    if (version < 3) {
        updates.clear();
        return readFinalizationProof(reader, finalization);
    }
    if (version >= 6) {
        return readValidatorEndpointUpdates(reader, updates) &&
               readEconomicPolicyUpdate(reader, policy, version) &&
               readValidatorApplications(reader, applications) &&
               readValidatorWorkBindings(reader, work_bindings) &&
               readFinalizationProof(reader, finalization);
    }
    if (version >= 5) {
        return readValidatorEndpointUpdates(reader, updates) &&
               readEconomicPolicyUpdate(reader, policy, version) &&
               readValidatorApplications(reader, applications) &&
               readFinalizationProof(reader, finalization);
    }
    if (version >= 4) {
        return readValidatorEndpointUpdates(reader, updates) &&
               readEconomicPolicyUpdate(reader, policy, version) &&
               readFinalizationProof(reader, finalization);
    }

    auto with_endpoints = reader;
    std::vector<ValidatorEndpointUpdateV1> parsed_updates;
    FinalizationProofV0 parsed_finalization;
    if (readValidatorEndpointUpdates(with_endpoints, parsed_updates) &&
        readFinalizationProof(with_endpoints, parsed_finalization) &&
        with_endpoints.consumed()) {
        updates = std::move(parsed_updates);
        finalization = std::move(parsed_finalization);
        reader.setOffset(with_endpoints.offset());
        return true;
    }

    updates.clear();
    return readFinalizationProof(reader, finalization);
}

std::vector<std::uint8_t> serializeCompositeRecordInternal(
    const CompositeRecordV0& record,
    bool include_votes) {
    std::vector<std::uint8_t> out;
    appendUint64(out, kCompositeRecordTag);
    appendUint64(out, record.version);
    appendUint64(out, record.height);
    appendHash(out, record.previous_record_hash);
    appendUint64(out, record.integer);
    appendCompositeProof(out, record.proof);
    appendTransactionBatch(out, record.tx_batch);
    appendTransactionList(out, record.transactions);
    appendHash(out, record.state_root);
    if (record.version >= 1 && record.version < 9) appendCommitPhaseCertificate(out, record.commit_phase);
    if (record.version >= 2) appendValidatorEpochTransition(out, record.validator_epoch);
    if (record.version >= 3) appendValidatorEndpointUpdates(out, record.validator_endpoints);
    if (record.version >= 4) appendEconomicPolicyUpdate(out, record.economic_policy, record.version);
    if (record.version >= 5) appendValidatorApplications(out, record.validator_applications);
    if (record.version >= 6) appendValidatorWorkBindings(out, record.validator_work_bindings);
    if (record.version >= 10) appendCompositeLotteryProof(out, record.composite_lottery);
    appendFinalizationProof(out, record.finalized_by, include_votes);
    return out;
}

std::vector<std::uint8_t> serializePrimeRecordInternal(
    const PrimeRecordV0& record,
    bool include_votes) {
    std::vector<std::uint8_t> out;
    appendUint64(out, kPrimeRecordTag);
    appendUint64(out, record.version);
    appendUint64(out, record.height);
    appendHash(out, record.previous_record_hash);
    appendUint64(out, record.integer);
    appendPrattProof(out, record.proof);
    appendTransactionBatch(out, record.tx_batch);
    appendTransactionList(out, record.transactions);
    appendHash(out, record.state_root);
    if (record.height == 0) appendGenesisConfig(out, record.genesis_config);
    if (record.version >= 2) appendValidatorEpochTransition(out, record.validator_epoch);
    if (record.version >= 3) appendValidatorEndpointUpdates(out, record.validator_endpoints);
    if (record.version >= 4) appendEconomicPolicyUpdate(out, record.economic_policy, record.version);
    if (record.version >= 5) appendValidatorApplications(out, record.validator_applications);
    if (record.version >= 6) appendValidatorWorkBindings(out, record.validator_work_bindings);
    appendFinalizationProof(out, record.finalized_by, include_votes);
    return out;
}

} // namespace

bool isDevelopmentAddress(const Address& address) {
    constexpr std::string_view prefix = "pcdev1_";
    if (address.size() <= prefix.size() || address.size() > 64) {
        return false;
    }
    if (address.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    return std::all_of(address.begin(), address.end(), [](unsigned char ch) {
        return ch >= 0x21 && ch <= 0x7e;
    });
}

bool isProtocolFeePoolAddress(const Address& address) {
    constexpr std::string_view prefix = "pcpool_validator_fees_epoch_";
    if (address.size() <= prefix.size() ||
        address.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    return std::all_of(address.begin() + static_cast<std::ptrdiff_t>(prefix.size()), address.end(), [](unsigned char ch) {
        return ch >= '0' && ch <= '9';
    });
}

bool isProtocolValidatorRewardPoolAddress(const Address& address) {
    constexpr std::string_view prefix = "pcpool_validator_rewards_epoch_";
    if (address.size() <= prefix.size() ||
        address.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    return std::all_of(address.begin() + static_cast<std::ptrdiff_t>(prefix.size()), address.end(), [](unsigned char ch) {
        return ch >= '0' && ch <= '9';
    });
}

std::optional<Address> validatorAddressFromReserveAddress(const Address& reserve_address) {
    constexpr std::string_view prefix = "pcreserve_validator_";
    if (reserve_address.size() <= prefix.size() ||
        reserve_address.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) {
        return std::nullopt;
    }
    Address validator_address = reserve_address.substr(prefix.size());
    if (!crypto::isProtocolSignatureAddress(validator_address)) return std::nullopt;
    return validator_address;
}

bool isProtocolValidatorReserveAddress(const Address& address) {
    return validatorAddressFromReserveAddress(address).has_value();
}

bool isProtocolAddress(const Address& address) {
    return isDevelopmentAddress(address) ||
           isProtocolFeePoolAddress(address) ||
           isProtocolValidatorRewardPoolAddress(address) ||
           isProtocolValidatorReserveAddress(address) ||
           crypto::isProtocolSignatureAddress(address);
}

Address validatorFeePoolAddress(std::uint64_t validator_epoch) {
    return "pcpool_validator_fees_epoch_" + std::to_string(validator_epoch);
}

Address validatorRewardPoolAddress(std::uint64_t validator_epoch) {
    return "pcpool_validator_rewards_epoch_" + std::to_string(validator_epoch);
}

Address validatorReserveAddress(const Address& validator_address) {
    return "pcreserve_validator_" + validator_address;
}

Address developmentAddressFromPublicKey(const Bytes& public_key) {
    const Hash256 hash = crypto::sha3_256(public_key);
    return "pcdev1_" + crypto::toHex(hash).substr(0, 32);
}

std::vector<std::uint8_t> serializeTransaction(const TransactionV0& tx, bool include_signature) {
    std::vector<std::uint8_t> out;
    appendUint64(out, tx.version);
    appendUint64(out, tx.inputs.size());
    for (const auto& input : tx.inputs) {
        appendTxInput(out, input);
    }
    appendUint64(out, tx.outputs.size());
    for (const auto& output : tx.outputs) {
        appendTxOutput(out, output);
    }
    appendFeeSpec(out, tx.fee);
    appendUint64(out, tx.nonce);
    appendAddress(out, tx.sender_address);
    appendBytes(out, tx.sender_public_key);
    if (include_signature) {
        appendBytes(out, tx.signature);
    } else {
        appendUint64(out, 0);
    }
    return out;
}

std::optional<TransactionV0> deserializeTransaction(const std::vector<std::uint8_t>& bytes, std::string& error) {
    ByteReader reader(bytes);
    TransactionV0 tx;
    std::uint64_t input_count = 0;
    std::uint64_t output_count = 0;
    if (!reader.readUint64(tx.version) || !reader.readUint64(input_count) ||
        input_count > kMaxDecodedTxInputs) {
        error = "truncated or oversized transaction header";
        return std::nullopt;
    }
    tx.inputs.reserve(static_cast<std::size_t>(input_count));
    for (std::uint64_t i = 0; i < input_count; ++i) {
        TxInputV0 input;
        if (!reader.readUint64(input.prime) || !readAmount(reader, input.amount)) {
            error = "truncated transaction input";
            return std::nullopt;
        }
        tx.inputs.push_back(input);
    }
    if (!reader.readUint64(output_count) || output_count > kMaxDecodedTxOutputs) {
        error = "truncated or oversized transaction output count";
        return std::nullopt;
    }
    tx.outputs.reserve(static_cast<std::size_t>(output_count));
    for (std::uint64_t i = 0; i < output_count; ++i) {
        TxOutputV0 output;
        if (!reader.readUint64(output.prime) ||
            !readAmount(reader, output.amount) ||
            !reader.readString(output.receiver_address)) {
            error = "truncated transaction output";
            return std::nullopt;
        }
        tx.outputs.push_back(output);
    }
    if (!reader.readUint64(tx.fee.prime) ||
        !readAmount(reader, tx.fee.amount) ||
        !reader.readUint64(tx.nonce) ||
        !reader.readString(tx.sender_address) ||
        !reader.readBytes(tx.sender_public_key) ||
        !reader.readBytes(tx.signature)) {
        error = "truncated transaction footer";
        return std::nullopt;
    }
    if (!reader.consumed()) {
        error = "trailing bytes in transaction";
        return std::nullopt;
    }
    return tx;
}

std::vector<std::uint8_t> serializeCompositeRecord(const CompositeRecordV0& record) {
    return serializeCompositeRecordInternal(record, true);
}

std::vector<std::uint8_t> serializePrimeRecord(const PrimeRecordV0& record) {
    return serializePrimeRecordInternal(record, true);
}

std::optional<CompositeRecordV0> deserializeCompositeRecord(const std::vector<std::uint8_t>& bytes, std::string& error) {
    ByteReader reader(bytes);
    std::uint64_t tag = 0;
    CompositeRecordV0 record;
    if (!reader.readUint64(tag) || tag != kCompositeRecordTag) {
        error = "invalid composite record tag";
        return std::nullopt;
    }
    if (!reader.readUint64(record.version) ||
        !reader.readUint64(record.height) ||
        !reader.readHash(record.previous_record_hash) ||
        !reader.readUint64(record.integer) ||
        !readCompositeProof(reader, record.proof) ||
        !readTransactionBatch(reader, record.tx_batch) ||
        !readTransactionList(reader, record.transactions, error) ||
        !reader.readHash(record.state_root) ||
        (record.version >= 1 && record.version < 9 && !readCommitPhaseCertificate(reader, record.commit_phase)) ||
        (record.version >= 2 && !readValidatorEpochTransition(reader, record.validator_epoch))) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    record.economic_policy = {};
    record.validator_applications.clear();
    record.validator_work_bindings.clear();
    if (record.version >= 3 && !readValidatorEndpointUpdates(reader, record.validator_endpoints)) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    if (record.version >= 4 && !readEconomicPolicyUpdate(reader, record.economic_policy, record.version)) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    if (record.version >= 5 && !readValidatorApplications(reader, record.validator_applications)) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    if (record.version >= 6 && !readValidatorWorkBindings(reader, record.validator_work_bindings)) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    if (record.version >= 10 && !readCompositeLotteryProof(reader, record.composite_lottery)) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    if (!readFinalizationProof(reader, record.finalized_by)) {
        error = "truncated composite record payload";
        return std::nullopt;
    }
    if (!reader.consumed()) {
        error = "trailing bytes in composite record payload";
        return std::nullopt;
    }
    return record;
}

std::optional<PrimeRecordV0> deserializePrimeRecord(const std::vector<std::uint8_t>& bytes, std::string& error) {
    ByteReader reader(bytes);
    std::uint64_t tag = 0;
    PrimeRecordV0 record;
    if (!reader.readUint64(tag) || tag != kPrimeRecordTag) {
        error = "invalid prime record tag";
        return std::nullopt;
    }
    if (!reader.readUint64(record.version) ||
        !reader.readUint64(record.height) ||
        !reader.readHash(record.previous_record_hash) ||
        !reader.readUint64(record.integer) ||
        !readPrattProof(reader, record.proof) ||
        !readTransactionBatch(reader, record.tx_batch) ||
        !readTransactionList(reader, record.transactions, error) ||
        !reader.readHash(record.state_root)) {
        error = "truncated prime record payload";
        return std::nullopt;
    }

    const auto read_metadata = [&](bool read_genesis_config) {
        auto candidate_reader = reader;
        record.genesis_config = {};
        record.validator_epoch = {};
        record.validator_endpoints.clear();
        record.economic_policy = {};
        record.validator_applications.clear();
        record.validator_work_bindings.clear();
        record.finalized_by = {};
        if (read_genesis_config && !readGenesisConfig(candidate_reader, record.genesis_config)) {
            return false;
        }
        if (record.version >= 2 &&
            !readValidatorEpochTransition(candidate_reader, record.validator_epoch)) {
            return false;
        }
        if (!readOptionalMetadataAndFinalization(
                candidate_reader, record.version, record.validator_endpoints,
                record.economic_policy, record.validator_applications,
                record.validator_work_bindings, record.finalized_by) ||
            !candidate_reader.consumed()) {
            return false;
        }
        reader.setOffset(candidate_reader.offset());
        return true;
    };

    bool decoded_metadata = false;
    if (record.height == 0) {
        decoded_metadata = read_metadata(true);
    } else {
        decoded_metadata = read_metadata(false);
    }
    if (!decoded_metadata) {
        error = "truncated prime record payload";
        return std::nullopt;
    }
    if (!reader.consumed()) {
        error = "trailing bytes in prime record payload";
        return std::nullopt;
    }
    return record;
}

Hash256 transactionHash(const TransactionV0& tx) {
    return crypto::sha3_256(serializeTransaction(tx, true));
}

Hash256 transactionMerkleRoot(const std::vector<TransactionV0>& transactions) {
    if (transactions.empty()) {
        return {};
    }
    std::vector<std::uint8_t> payload;
    appendString(payload, "primechain-dev-tx-root-v0");
    appendUint64(payload, transactions.size());
    for (const auto& tx : transactions) {
        appendHash(payload, transactionHash(tx));
    }
    return crypto::sha3_256(payload);
}

void updateTransactionBatch(CompositeRecordV0& record) {
    record.tx_batch.transaction_count = record.transactions.size();
    record.tx_batch.transaction_merkle_root = transactionMerkleRoot(record.transactions);
}

void updateTransactionBatch(PrimeRecordV0& record) {
    record.tx_batch.transaction_count = record.transactions.size();
    record.tx_batch.transaction_merkle_root = transactionMerkleRoot(record.transactions);
}

std::vector<std::uint8_t> serializeCompositeRecordWithoutFinalization(const CompositeRecordV0& record) {
    std::vector<std::uint8_t> out;
    appendUint64(out, kCompositeRecordTag);
    appendUint64(out, record.version);
    appendUint64(out, record.height);
    appendHash(out, record.previous_record_hash);
    appendUint64(out, record.integer);
    appendCompositeProof(out, record.proof);
    appendTransactionBatch(out, record.tx_batch);
    appendTransactionList(out, record.transactions);
    appendHash(out, record.state_root);
    if (record.version >= 1 && record.version < 9) appendCommitPhaseCertificate(out, record.commit_phase);
    if (record.version >= 2) appendValidatorEpochTransition(out, record.validator_epoch);
    if (record.version >= 3) appendValidatorEndpointUpdates(out, record.validator_endpoints);
    if (record.version >= 4) appendEconomicPolicyUpdate(out, record.economic_policy, record.version);
    if (record.version >= 5) appendValidatorApplications(out, record.validator_applications);
    if (record.version >= 6) appendValidatorWorkBindings(out, record.validator_work_bindings);
    if (record.version >= 10) appendCompositeLotteryProof(out, record.composite_lottery);
    return out;
}

std::vector<std::uint8_t> serializePrimeRecordWithoutFinalization(const PrimeRecordV0& record) {
    std::vector<std::uint8_t> out;
    appendUint64(out, kPrimeRecordTag);
    appendUint64(out, record.version);
    appendUint64(out, record.height);
    appendHash(out, record.previous_record_hash);
    appendUint64(out, record.integer);
    appendPrattProof(out, record.proof);
    appendTransactionBatch(out, record.tx_batch);
    appendTransactionList(out, record.transactions);
    appendHash(out, record.state_root);
    if (record.height == 0) appendGenesisConfig(out, record.genesis_config);
    if (record.version >= 2) appendValidatorEpochTransition(out, record.validator_epoch);
    if (record.version >= 3) appendValidatorEndpointUpdates(out, record.validator_endpoints);
    if (record.version >= 4) appendEconomicPolicyUpdate(out, record.economic_policy, record.version);
    if (record.version >= 5) appendValidatorApplications(out, record.validator_applications);
    if (record.version >= 6) appendValidatorWorkBindings(out, record.validator_work_bindings);
    return out;
}

Hash256 legacyCandidateRecordHashWithoutFinalization(const CompositeRecordV0& record) {
    return crypto::sha3_256(serializeCompositeRecordWithoutFinalization(record));
}

Hash256 legacyCandidateRecordHashWithoutFinalization(const PrimeRecordV0& record) {
    return crypto::sha3_256(serializePrimeRecordWithoutFinalization(record));
}

Hash256 compositeLotterySubjectHash(const CompositeRecordV0& record) {
    CompositeRecordV0 subject = record;
    subject.composite_lottery = {};
    subject.finalized_by = {};
    return crypto::sha3_256(serializeCompositeRecordInternal(subject, false));
}

std::optional<Address> assignedCompositeLotteryValidator(
    const CompositeRecordV0& record,
    const std::vector<Address>& validator_set,
    std::string& error) {
    if (!isCanonicalProtocolValidatorSet(validator_set)) {
        error = "composite lottery requires canonical validator set";
        return std::nullopt;
    }
    const auto subject_hash = compositeLotterySubjectHash(record);
    Bytes payload;
    appendString(payload, "primechain-composite-lottery-assignment-v1");
    appendHash(payload, record.previous_record_hash);
    appendUint64(payload, record.integer);
    appendHash(payload, subject_hash);
    appendUint64(payload, validator_set.size());
    for (const auto& validator : validator_set) appendString(payload, validator);
    const auto assignment_hash = crypto::sha3_256(payload);
    std::uint64_t number = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        number = (number << 8) | assignment_hash[i];
    }
    return validator_set[number % validator_set.size()];
}

bool verifyCompositeLotteryProof(
    const CompositeRecordV0& record,
    const std::vector<Address>& validator_set,
    std::string& error) {
    if (record.version < 10) return true;
    if (record.composite_lottery.round == 0) {
        error = "composite lottery round must be positive";
        return false;
    }
    if (record.composite_lottery.win_bps > 10000) {
        error = "composite lottery win rate is invalid";
        return false;
    }
    const auto subject_hash = compositeLotterySubjectHash(record);
    if (record.composite_lottery.subject_hash != subject_hash) {
        error = "composite lottery subject hash mismatch";
        return false;
    }
    const auto assigned = assignedCompositeLotteryValidator(record, validator_set, error);
    if (!assigned.has_value()) return false;
    if (record.composite_lottery.assigned_validator != *assigned) {
        error = "composite lottery assigned validator mismatch";
        return false;
    }
    if (record.composite_lottery.assigned_validator !=
        crypto::addressFromProtocolPublicKey(record.composite_lottery.public_key)) {
        error = "composite lottery public key address mismatch";
        return false;
    }
    std::string signature_error;
    if (!crypto::verifyProtocolMessageSignature(
            record.composite_lottery.public_key,
            crypto::compositeLotteryWinSigningPayload(
                record.previous_record_hash, record.integer, subject_hash,
                record.composite_lottery.round, record.composite_lottery.win_bps,
                record.composite_lottery.assigned_validator),
            record.composite_lottery.signature,
            signature_error)) {
        error = "invalid composite lottery win signature";
        return false;
    }
    return true;
}

Hash256 candidateRecordHash(const CompositeRecordV0& record) {
    return crypto::sha3_256(serializeCompositeRecordInternal(record, false));
}

Hash256 candidateRecordHash(const PrimeRecordV0& record) {
    return crypto::sha3_256(serializePrimeRecordInternal(record, false));
}

Hash256 finalizedRecordHash(const CompositeRecordV0& record) {
    return crypto::sha3_256(serializeCompositeRecord(record));
}

Hash256 finalizedRecordHash(const PrimeRecordV0& record) {
    return crypto::sha3_256(serializePrimeRecord(record));
}

Hash256 commitPhaseSnapshotHash(
    PrimeValue integer,
    const std::vector<CommitCertificateEntryV1>& commitments) {
    Bytes payload;
    appendString(payload, "primechain-commit-snapshot-v1");
    appendUint64(payload, integer);
    appendUint64(payload, commitments.size());
    for (const auto& commitment : commitments) {
        appendCommitCertificateEntry(payload, commitment);
    }
    return crypto::sha3_256(payload);
}

bool verifyCommitPhaseCertificate(
    const CompositeRecordV0& record,
    std::string& error) {
    if (record.version == 0 || record.version >= 9) return true;
    if (record.version != 1 && record.version != 2 && record.version != 3 &&
        record.version != 4 && record.version != 5 && record.version != 6 && record.version != 7 && record.version != 8) {
        error = "unsupported composite record version";
        return false;
    }
    const auto& certificate = record.commit_phase;
    if (certificate.integer != record.integer) {
        error = "commit-phase certificate integer mismatch";
        return false;
    }
    if (!isCanonicalProtocolValidatorSet(certificate.validator_set)) {
        error = "commit-phase validator set is not canonical";
        return false;
    }
    if (certificate.commitments.empty() || certificate.commitments.size() > 1024) {
        error = "invalid embedded commitment count";
        return false;
    }
    for (std::size_t i = 0; i < certificate.commitments.size(); ++i) {
        const auto& commitment = certificate.commitments[i];
        if (i != 0) {
            const auto& previous = certificate.commitments[i - 1];
            if (previous.commitment_hash > commitment.commitment_hash ||
                (previous.commitment_hash == commitment.commitment_hash &&
                 previous.provider_address >= commitment.provider_address)) {
                error = "embedded commitments are not canonical";
                return false;
            }
        }
        if (commitment.provider_address !=
            crypto::addressFromProtocolPublicKey(commitment.public_key)) {
            error = "embedded commitment address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                commitment.public_key,
                crypto::compositeCommitSigningPayload(
                    record.integer, commitment.commitment_hash,
                    commitment.provider_address),
                commitment.signature,
                signature_error)) {
            error = "invalid embedded commitment signature";
            return false;
        }
    }
    if (certificate.snapshot_hash !=
        commitPhaseSnapshotHash(record.integer, certificate.commitments)) {
        error = "commit-phase snapshot hash mismatch";
        return false;
    }
    if (!hasQuorumVoteCount(certificate.votes.size(), certificate.validator_set.size())) {
        error = "commit-phase certificate does not meet validator quorum";
        return false;
    }
    Address previous_vote;
    for (const auto& vote : certificate.votes) {
        if (!previous_vote.empty() && previous_vote >= vote.validator_address) {
            error = "commit-phase votes are not canonical";
            return false;
        }
        previous_vote = vote.validator_address;
        if (!std::binary_search(
                certificate.validator_set.begin(), certificate.validator_set.end(),
                vote.validator_address)) {
            error = "commit-phase vote is outside validator set";
            return false;
        }
        if (vote.validator_address != crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "commit-phase vote address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                vote.public_key,
                crypto::commitPhaseVoteSigningPayload(
                    record.integer, certificate.snapshot_hash,
                    vote.validator_address),
                vote.signature,
                signature_error)) {
            error = "invalid embedded commit-phase vote signature";
            return false;
        }
    }
    const auto& winner = certificate.commitments.front();
    if (winner.provider_address != record.proof.provider_address) {
        error = "composite provider is not embedded commitment winner";
        return false;
    }
    if (!crypto::packedCompositeRevealMatchesCommitment(
            record.integer, record.proof.d, record.proof.e,
            record.proof.provider_address, record.proof.signature,
            winner.commitment_hash, error)) {
        return false;
    }
    return true;
}

bool verifyGenesisConfig(const PrimeRecordV0& record, std::string& error) {
    if (record.height != 0) {
        if (!record.genesis_config.validator_set.empty()) {
            error = "genesis configuration is only valid at height zero";
            return false;
        }
        return true;
    }
    if (record.version == 0) {
        if (!record.genesis_config.validator_set.empty()) {
            error = "legacy genesis cannot contain validator configuration";
            return false;
        }
        return true;
    }
    if (record.version != 1) {
        error = "unsupported genesis record version";
        return false;
    }
    const auto& validators = record.genesis_config.validator_set;
    if (!isCanonicalProtocolValidatorSet(validators)) {
        error = "genesis validator set must contain canonical validator addresses";
        return false;
    }
    return true;
}

bool verifyValidatorEpochTransition(
    const ValidatorEpochTransitionV1& transition,
    const std::vector<Address>& current_validator_set,
    std::uint64_t current_epoch,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error) {
    const bool absent = transition.epoch == 0 && transition.activation_integer == 0 &&
        transition.next_validator_set.empty() && transition.votes.empty();
    if (absent) return true;
    if (!isCanonicalProtocolValidatorSet(current_validator_set)) {
        error = "validator epoch requires an active canonical validator set";
        return false;
    }
    if (transition.epoch != current_epoch + 1) {
        error = "validator epoch number is not sequential";
        return false;
    }
    if (transition.activation_integer != record_integer + 1) {
        error = "validator epoch must activate at the next integer";
        return false;
    }
    if (!isCanonicalProtocolValidatorSet(transition.next_validator_set)) {
        error = "next validator set must contain canonical validator addresses";
        return false;
    }
    if (!hasQuorumVoteCount(transition.votes.size(), current_validator_set.size())) {
        error = "validator epoch votes do not meet current validator quorum";
        return false;
    }
    Address previous_vote;
    for (const auto& vote : transition.votes) {
        if (!previous_vote.empty() && previous_vote >= vote.validator_address) {
            error = "validator epoch votes are not canonical";
            return false;
        }
        previous_vote = vote.validator_address;
        if (!std::binary_search(current_validator_set.begin(), current_validator_set.end(),
                                vote.validator_address)) {
            error = "validator epoch vote is outside current set";
            return false;
        }
        if (vote.validator_address != crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "validator epoch vote address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                vote.public_key,
                crypto::validatorEpochVoteSigningPayload(
                    previous_record_hash, record_integer, transition.epoch,
                    transition.activation_integer, transition.next_validator_set,
                    vote.validator_address),
                vote.signature, signature_error)) {
            error = "invalid validator epoch vote signature";
            return false;
        }
    }
    return true;
}

Bytes developmentVoteSignature(const Address& validator_address, const Hash256& record_hash, std::uint64_t round) {
    std::vector<std::uint8_t> payload;
    appendString(payload, kDevelopmentVoteDomain);
    appendAddress(payload, validator_address);
    appendHash(payload, record_hash);
    appendUint64(payload, round);

    const Hash256 hash = crypto::sha3_256(payload);
    return Bytes(hash.begin(), hash.end());
}

ValidatorVoteV0 makeDevelopmentVote(const Address& validator_address, const Hash256& record_hash, std::uint64_t round) {
    ValidatorVoteV0 vote;
    vote.validator_address = validator_address;
    vote.record_hash = record_hash;
    vote.round = round;
    vote.signature = developmentVoteSignature(validator_address, record_hash, round);
    return vote;
}

Bytes developmentTransactionSignature(const TransactionV0& tx) {
    std::vector<std::uint8_t> payload;
    appendString(payload, "primechain-dev-tx-signature-v0");
    appendBytes(payload, tx.sender_public_key);
    const auto unsigned_tx = serializeTransaction(tx, false);
    appendBytes(payload, unsigned_tx);
    const Hash256 hash = crypto::sha3_256(payload);
    return Bytes(hash.begin(), hash.end());
}

bool verifyDevelopmentTransactionSignature(const TransactionV0& tx) {
    return tx.sender_address == developmentAddressFromPublicKey(tx.sender_public_key) &&
           tx.signature == developmentTransactionSignature(tx);
}

bool verifyAuthenticatedTransactionSignature(const TransactionV0& tx, std::string& error) {
    if (!crypto::isProtocolSignatureAddress(tx.sender_address)) {
        error = "transaction sender is not an ML-DSA-65 address";
        return false;
    }
    if (tx.sender_address != crypto::addressFromProtocolPublicKey(tx.sender_public_key)) {
        error = "transaction sender address does not match public key";
        return false;
    }
    return crypto::verifyProtocolMessageSignature(
        tx.sender_public_key,
        crypto::transactionSigningPayload(serializeTransaction(tx, false)),
        tx.signature,
        error);
}

void applyDevelopmentFinalization(CompositeRecordV0& record) {
    updateTransactionBatch(record);
    record.finalized_by.rule = std::string(kDevelopmentFinalizationRule);
    record.finalized_by.votes.clear();
    const Hash256 candidate_hash = candidateRecordHash(record);
    record.finalized_by.votes.push_back(makeDevelopmentVote("pcdev1_validator_a", candidate_hash, 1));
    record.finalized_by.votes.push_back(makeDevelopmentVote("pcdev1_validator_b", candidate_hash, 1));
}

void applyDevelopmentFinalization(PrimeRecordV0& record) {
    updateTransactionBatch(record);
    record.finalized_by.rule = std::string(kDevelopmentFinalizationRule);
    record.finalized_by.votes.clear();
    const Hash256 candidate_hash = candidateRecordHash(record);
    record.finalized_by.votes.push_back(makeDevelopmentVote("pcdev1_validator_a", candidate_hash, 1));
    record.finalized_by.votes.push_back(makeDevelopmentVote("pcdev1_validator_b", candidate_hash, 1));
}

bool verifyDevelopmentFinalization(const FinalizationProofV0& proof, const Hash256& candidate_hash, std::string& error) {
    if (proof.rule != kDevelopmentFinalizationRule) {
        error = "unsupported finalization rule";
        return false;
    }
    if (proof.votes.size() < 2 || proof.votes.size() > 3) {
        error = "development finalization requires 2 or 3 votes";
        return false;
    }

    std::set<Address> seen_validators;
    for (const auto& vote : proof.votes) {
        if (!isDevelopmentAddress(vote.validator_address)) {
            error = "invalid development validator address";
            return false;
        }
        if (!seen_validators.insert(vote.validator_address).second) {
            error = "duplicate development validator vote";
            return false;
        }
        if (vote.record_hash != candidate_hash) {
            error = "validator vote record hash mismatch";
            return false;
        }
        if (vote.signature != developmentVoteSignature(vote.validator_address, vote.record_hash, vote.round)) {
            error = "invalid development validator signature";
            return false;
        }
    }

    return true;
}

ValidatorVoteV0 makeSignedValidatorVote(
    const Address& validator_address,
    const Bytes& public_key,
    const Bytes& private_key,
    const Hash256& record_hash,
    std::uint64_t round,
    std::string& error) {
    ValidatorVoteV0 vote;
    vote.validator_address = validator_address;
    vote.public_key = public_key;
    vote.record_hash = record_hash;
    vote.round = round;
    const auto signature = crypto::signProtocolMessage(
        private_key,
        crypto::recordFinalizationVoteSigningPayload(record_hash, round, validator_address),
        error);
    if (signature.has_value()) vote.signature = *signature;
    return vote;
}

bool verifyRoundChangeCertificate(
    const FinalizationProofV0& proof,
    const Hash256& previous_record_hash,
    PrimeValue integer,
    const std::vector<Address>& validator_set,
    std::uint64_t& round,
    std::string& error) {
    round = 1;
    if (proof.round_changes.empty()) {
        if (proof.rule != kSignedFinalizationRule) {
            error = "round one must use the original signed-finalization rule";
            return false;
        }
        return true;
    }
    const bool locked_round_rule = proof.rule == kLockedRoundFinalizationRule;
    if ((proof.rule != kRoundFinalizationRule && !locked_round_rule) ||
        !hasQuorumVoteCount(proof.round_changes.size(), validator_set.size())) {
        error = "later finalization rounds require a validator-quorum round-change certificate";
        return false;
    }
    round = proof.round_changes.front().new_round;
    if (round < 2) { error = "round-change target must be at least two"; return false; }
    Address previous_round_validator;
    for (const auto& change : proof.round_changes) {
        if (!previous_round_validator.empty() && previous_round_validator >= change.validator_address) {
            error = "round-change votes are not canonical"; return false;
        }
        previous_round_validator = change.validator_address;
        if (!std::binary_search(validator_set.begin(), validator_set.end(), change.validator_address) ||
            change.validator_address != crypto::addressFromProtocolPublicKey(change.public_key) ||
            change.previous_record_hash != previous_record_hash || change.integer != integer ||
            change.new_round != round) {
            error = "round-change vote target mismatch"; return false;
        }
        if (locked_round_rule) {
            if (change.locked_round == 0) {
                if (!change.locked_candidate_kind.empty() || !isZeroHash(change.locked_candidate_hash) ||
                    !change.locked_candidate_payload.empty()) {
                    error = "empty round-change lock carries candidate data"; return false;
                }
            } else {
                if (change.locked_round >= change.new_round ||
                    (change.locked_candidate_kind != "PRIME" && change.locked_candidate_kind != "COMPOSITE") ||
                    change.locked_candidate_payload.empty()) {
                    error = "invalid round-change lock target"; return false;
                }
                std::string candidate_error;
                Hash256 computed_hash{};
                if (change.locked_candidate_kind == "PRIME") {
                    auto locked = deserializePrimeRecord(change.locked_candidate_payload, candidate_error);
                    if (!locked.has_value() || locked->height == 0 || locked->previous_record_hash != previous_record_hash ||
                        locked->integer != integer) { error = "invalid locked prime candidate"; return false; }
                    computed_hash = legacyCandidateRecordHashWithoutFinalization(*locked);
                } else {
                    auto locked = deserializeCompositeRecord(change.locked_candidate_payload, candidate_error);
                    if (!locked.has_value() || locked->previous_record_hash != previous_record_hash ||
                        locked->integer != integer) { error = "invalid locked composite candidate"; return false; }
                    computed_hash = legacyCandidateRecordHashWithoutFinalization(*locked);
                }
                if (computed_hash != change.locked_candidate_hash) {
                    error = "round-change lock candidate hash mismatch"; return false;
                }
            }
        } else if (change.locked_round != 0 || !change.locked_candidate_kind.empty() ||
                   !isZeroHash(change.locked_candidate_hash) || !change.locked_candidate_payload.empty()) {
            error = "legacy round-change vote carries lock data"; return false;
        }
        std::string signature_error;
        const auto signing_payload = locked_round_rule
            ? crypto::lockedRoundChangeVoteSigningPayload(
                change.previous_record_hash, change.integer, change.new_round,
                change.locked_round, change.locked_candidate_kind,
                change.locked_candidate_hash, change.locked_candidate_payload,
                change.validator_address)
            : crypto::roundChangeVoteSigningPayload(change.previous_record_hash,
                change.integer, change.new_round, change.validator_address);
        if (!crypto::verifyProtocolMessageSignature(change.public_key,
                signing_payload, change.signature, signature_error)) {
            error = "invalid round-change vote signature"; return false;
        }
    }
    return true;
}

bool verifyRecordFinalization(
    const FinalizationProofV0& proof,
    const Hash256& candidate_hash,
    const Hash256& previous_record_hash,
    PrimeValue integer,
    const std::vector<Address>& validator_set,
    std::string& error) {
    if (validator_set.empty()) return verifyDevelopmentFinalization(proof, candidate_hash, error);
    if (proof.rule != kSignedFinalizationRule && proof.rule != kRoundFinalizationRule &&
        proof.rule != kLockedRoundFinalizationRule) {
        error = "quorum records require ML-DSA-65 finalization"; return false;
    }
    if (!isCanonicalProtocolValidatorSet(validator_set) ||
        !hasQuorumVoteCount(proof.votes.size(), validator_set.size())) {
        error = "signed finalization requires validator-quorum votes from the active set"; return false;
    }
    std::uint64_t round = 0;
    if (!verifyRoundChangeCertificate(
            proof, previous_record_hash, integer, validator_set, round, error)) return false;
    if (proof.votes.front().round != round) {
        error = "finalization votes do not match certified round";
        return false;
    }
    if (proof.rule == kLockedRoundFinalizationRule) {
        std::uint64_t highest_locked_round = 0;
        Hash256 highest_locked_hash{};
        bool have_highest_lock = false;
        for (const auto& change : proof.round_changes) {
            if (change.locked_round == 0) continue;
            if (change.locked_round > highest_locked_round) {
                highest_locked_round = change.locked_round;
                highest_locked_hash = change.locked_candidate_hash;
                have_highest_lock = true;
            } else if (change.locked_round == highest_locked_round &&
                       change.locked_candidate_hash != highest_locked_hash) {
                error = "conflicting highest round-change locks";
                return false;
            }
        }
        if (have_highest_lock && highest_locked_hash != candidate_hash) {
            error = "finalized candidate does not match highest round-change lock";
            return false;
        }
    }
    Address previous;
    for (const auto& vote : proof.votes) {
        if (!previous.empty() && previous >= vote.validator_address) {
            error = "finalization votes are not canonical"; return false;
        }
        previous = vote.validator_address;
        if (!std::binary_search(validator_set.begin(), validator_set.end(), vote.validator_address)) {
            error = "finalization vote is outside active validator set"; return false;
        }
        if (vote.validator_address != crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "finalization vote address mismatch"; return false;
        }
        if (vote.record_hash != candidate_hash || vote.round != round) {
            error = "finalization vote target mismatch"; return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(vote.public_key,
                crypto::recordFinalizationVoteSigningPayload(vote.record_hash, vote.round, vote.validator_address),
                vote.signature, signature_error)) {
            error = "invalid finalization vote signature"; return false;
        }
    }
    return true;
}

bool validValidatorEndpointHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    return std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return ch > 0x20 && ch < 0x7f;
    });
}

bool verifyValidatorEndpointUpdates(
    const std::vector<ValidatorEndpointUpdateV1>& updates,
    const std::vector<Address>& current_validator_set,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error) {
    Address previous_validator;
    for (const auto& update : updates) {
        if (!previous_validator.empty() && previous_validator >= update.validator_address) {
            error = "validator endpoint updates are not canonical";
            return false;
        }
        previous_validator = update.validator_address;
        if (!std::binary_search(current_validator_set.begin(), current_validator_set.end(), update.validator_address)) {
            error = "validator endpoint update is outside current set";
            return false;
        }
        if (!validValidatorEndpointHost(update.host) || update.port == 0 || update.port > 65535) {
            error = "invalid validator endpoint";
            return false;
        }
        if (update.effective_integer < record_integer) {
            error = "validator endpoint effective integer is stale";
            return false;
        }
        if (update.validator_address != crypto::addressFromProtocolPublicKey(update.public_key)) {
            error = "validator endpoint address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                update.public_key,
                crypto::validatorEndpointSigningPayload(
                    previous_record_hash, record_integer, update.validator_address,
                    update.host, update.port, update.effective_integer, update.sequence),
                update.signature, signature_error)) {
            error = "invalid validator endpoint signature";
            return false;
        }
    }
    return true;
}

bool verifyEconomicPolicyUpdate(
    const EconomicPolicyUpdateV1& update,
    const std::vector<Address>& current_validator_set,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error) {
    const bool absent = update.transfer_fee_micro_units == 0 &&
        update.validator_min_reserve_micro_units == 0 &&
        update.effective_integer == 0 && update.sequence == 0 && update.votes.empty();
    if (absent) return true;
    if (!isCanonicalProtocolValidatorSet(current_validator_set)) {
        error = "economic policy requires an active canonical validator set";
        return false;
    }
    if (update.transfer_fee_micro_units == 0) {
        error = "economic policy transfer fee must be nonzero";
        return false;
    }
    if (update.validator_min_reserve_micro_units == 0) {
        error = "economic policy validator reserve must be nonzero";
        return false;
    }
    if (update.effective_integer != record_integer + 1) {
        error = "economic policy must activate at the next integer";
        return false;
    }
    if (!hasQuorumVoteCount(update.votes.size(), current_validator_set.size())) {
        error = "economic policy votes do not meet validator quorum";
        return false;
    }
    Address previous_vote;
    for (const auto& vote : update.votes) {
        if (!previous_vote.empty() && previous_vote >= vote.validator_address) {
            error = "economic policy votes are not canonical";
            return false;
        }
        previous_vote = vote.validator_address;
        if (!std::binary_search(current_validator_set.begin(), current_validator_set.end(),
                                vote.validator_address)) {
            error = "economic policy vote is outside current set";
            return false;
        }
        if (vote.validator_address != crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "economic policy vote address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                vote.public_key,
                crypto::economicPolicySigningPayload(
                    previous_record_hash, record_integer, update.transfer_fee_micro_units,
                    update.validator_min_reserve_micro_units, update.effective_integer,
                    update.sequence, vote.validator_address),
                vote.signature, signature_error)) {
            error = "invalid economic policy vote signature";
            return false;
        }
    }
    return true;
}

bool verifyValidatorWorkBindings(
    const std::vector<ValidatorWorkBindingV1>& bindings,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error) {
    std::pair<Address, Address> previous_key;
    bool has_previous = false;
    for (const auto& binding : bindings) {
        const std::pair<Address, Address> key{binding.candidate_address, binding.miner_address};
        if (has_previous && previous_key >= key) {
            error = "validator work bindings are not canonical";
            return false;
        }
        has_previous = true;
        previous_key = key;
        if (!crypto::isProtocolSignatureAddress(binding.candidate_address) ||
            !crypto::isProtocolSignatureAddress(binding.miner_address) ||
            binding.miner_address != crypto::addressFromProtocolPublicKey(binding.miner_public_key) ||
            binding.record_integer != record_integer) {
            error = "validator work binding address or record mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                binding.miner_public_key,
                crypto::validatorWorkBindingMinerSigningPayload(
                    previous_record_hash, record_integer, binding.candidate_address,
                    binding.miner_address, binding.sequence),
                binding.miner_signature, signature_error)) {
            error = "invalid validator work binding miner signature";
            return false;
        }
    }
    return true;
}

bool verifyValidatorApplications(
    const std::vector<ValidatorApplicationV1>& applications,
    const Hash256& previous_record_hash,
    PrimeValue record_integer,
    std::string& error) {
    Address previous_candidate;
    for (const auto& application : applications) {
        if (!previous_candidate.empty() && previous_candidate >= application.candidate_address) {
            error = "validator applications are not canonical";
            return false;
        }
        previous_candidate = application.candidate_address;
        if (!crypto::isProtocolSignatureAddress(application.candidate_address) ||
            application.candidate_address != crypto::addressFromProtocolPublicKey(application.public_key)) {
            error = "validator application address mismatch";
            return false;
        }
        if (!validValidatorEndpointHost(application.host) || application.port == 0 || application.port > 65535 ||
            application.record_integer != record_integer) {
            error = "validator application does not match record metadata";
            return false;
        }
        std::string signature_error;
        if (!crypto::verifyProtocolMessageSignature(
                application.public_key,
                crypto::validatorApplicationSigningPayload(
                    previous_record_hash, record_integer, application.candidate_address,
                    application.host, application.port, application.sequence,
                    application.observed_successful, application.observed_total),
                application.signature, signature_error)) {
            error = "invalid validator application signature";
            return false;
        }
    }
    return true;
}

} // namespace primechain::protocol
