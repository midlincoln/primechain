#include "primechain/protocol/records.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>
#include <string_view>

#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"

namespace primechain::protocol {

namespace {

constexpr std::uint64_t kCompositeRecordTag = 1;
constexpr std::uint64_t kPrimeRecordTag = 2;
constexpr std::string_view kDevelopmentFinalizationRule = "fixed-2-of-3-dev";
constexpr std::string_view kSignedFinalizationRule = "fixed-2-of-3-ed25519-v1";
constexpr std::string_view kDevelopmentVoteDomain = "primechain-dev-vote-v0";

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

private:
    std::size_t remaining() const {
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

void appendFinalizationProof(
    std::vector<std::uint8_t>& out,
    const FinalizationProofV0& proof,
    bool include_votes) {
    appendString(out, proof.rule);
    if (!include_votes) {
        appendUint64(out, 0);
        return;
    }

    appendUint64(out, proof.votes.size());
    for (const auto& vote : proof.votes) {
        appendValidatorVote(out, vote);
    }
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
    if (!reader.readUint64(count)) {
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
        !reader.readUint64(factor_count)) {
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

bool readFinalizationProof(ByteReader& reader, FinalizationProofV0& proof) {
    std::uint64_t vote_count = 0;
    if (!reader.readString(proof.rule) || !reader.readUint64(vote_count)) {
        return false;
    }
    proof.votes.clear();
    proof.votes.reserve(static_cast<std::size_t>(vote_count));
    for (std::uint64_t i = 0; i < vote_count; ++i) {
        ValidatorVoteV0 vote;
        if (!readValidatorVote(reader, vote)) {
            return false;
        }
        proof.votes.push_back(vote);
    }
    return true;
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
    if (record.version >= 1) appendCommitPhaseCertificate(out, record.commit_phase);
    if (record.version >= 2) appendValidatorEpochTransition(out, record.validator_epoch);
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
    if (record.version >= 1) appendGenesisConfig(out, record.genesis_config);
    if (record.version >= 2) appendValidatorEpochTransition(out, record.validator_epoch);
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

bool isProtocolAddress(const Address& address) {
    return isDevelopmentAddress(address) || crypto::isEd25519Address(address);
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
    if (!reader.readUint64(tx.version) || !reader.readUint64(input_count)) {
        error = "truncated transaction header";
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
    if (!reader.readUint64(output_count)) {
        error = "truncated transaction output count";
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
        (record.version >= 1 && !readCommitPhaseCertificate(reader, record.commit_phase)) ||
        (record.version >= 2 && !readValidatorEpochTransition(reader, record.validator_epoch)) ||
        !readFinalizationProof(reader, record.finalized_by)) {
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
        !reader.readHash(record.state_root) ||
        (record.version >= 1 && !readGenesisConfig(reader, record.genesis_config)) ||
        (record.version >= 2 && !readValidatorEpochTransition(reader, record.validator_epoch)) ||
        !readFinalizationProof(reader, record.finalized_by)) {
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
    if (record.version == 0) return true;
    if (record.version != 1 && record.version != 2) {
        error = "unsupported composite record version";
        return false;
    }
    const auto& certificate = record.commit_phase;
    if (certificate.integer != record.integer) {
        error = "commit-phase certificate integer mismatch";
        return false;
    }
    if (certificate.validator_set.size() != 3) {
        error = "commit-phase certificate requires three validators";
        return false;
    }
    if (!std::is_sorted(certificate.validator_set.begin(), certificate.validator_set.end()) ||
        std::adjacent_find(certificate.validator_set.begin(), certificate.validator_set.end()) !=
            certificate.validator_set.end()) {
        error = "commit-phase validator set is not canonical";
        return false;
    }
    for (const auto& validator : certificate.validator_set) {
        if (!crypto::isEd25519Address(validator)) {
            error = "invalid commit-phase validator address";
            return false;
        }
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
            crypto::addressFromEd25519PublicKey(commitment.public_key)) {
            error = "embedded commitment address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::ed25519Verify(
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
    if (certificate.votes.size() < 2 || certificate.votes.size() > 3) {
        error = "commit-phase certificate requires two or three votes";
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
        if (vote.validator_address != crypto::addressFromEd25519PublicKey(vote.public_key)) {
            error = "commit-phase vote address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::ed25519Verify(
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
        if ((record.version != 0 && record.version != 1 && record.version != 2) ||
            !record.genesis_config.validator_set.empty()) {
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
    if (validators.size() != 3 ||
        !std::is_sorted(validators.begin(), validators.end()) ||
        std::adjacent_find(validators.begin(), validators.end()) != validators.end()) {
        error = "genesis validator set must contain three canonical addresses";
        return false;
    }
    for (const auto& validator : validators) {
        if (!crypto::isEd25519Address(validator)) {
            error = "invalid genesis validator address";
            return false;
        }
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
    if (current_validator_set.size() != 3) {
        error = "validator epoch requires an active three-validator set";
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
    if (transition.next_validator_set.size() != 3 ||
        !std::is_sorted(transition.next_validator_set.begin(), transition.next_validator_set.end()) ||
        std::adjacent_find(transition.next_validator_set.begin(), transition.next_validator_set.end()) !=
            transition.next_validator_set.end()) {
        error = "next validator set must contain three canonical addresses";
        return false;
    }
    for (const auto& validator : transition.next_validator_set) {
        if (!crypto::isEd25519Address(validator)) {
            error = "invalid next validator address";
            return false;
        }
    }
    if (transition.votes.size() < 2 || transition.votes.size() > 3) {
        error = "validator epoch requires two or three votes";
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
        if (vote.validator_address != crypto::addressFromEd25519PublicKey(vote.public_key)) {
            error = "validator epoch vote address mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::ed25519Verify(
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
    const auto signature = crypto::ed25519Sign(
        private_key,
        crypto::recordFinalizationVoteSigningPayload(record_hash, round, validator_address),
        error);
    if (signature.has_value()) vote.signature = *signature;
    return vote;
}

bool verifyRecordFinalization(
    const FinalizationProofV0& proof,
    const Hash256& candidate_hash,
    const std::vector<Address>& validator_set,
    std::string& error) {
    if (validator_set.empty()) {
        return verifyDevelopmentFinalization(proof, candidate_hash, error);
    }
    if (proof.rule != kSignedFinalizationRule) {
        error = "quorum records require Ed25519 finalization";
        return false;
    }
    if (validator_set.size() != 3 || proof.votes.size() < 2 || proof.votes.size() > 3) {
        error = "signed finalization requires two or three votes from a three-validator set";
        return false;
    }
    Address previous;
    for (const auto& vote : proof.votes) {
        if (!previous.empty() && previous >= vote.validator_address) {
            error = "finalization votes are not canonical";
            return false;
        }
        previous = vote.validator_address;
        if (!std::binary_search(validator_set.begin(), validator_set.end(), vote.validator_address)) {
            error = "finalization vote is outside active validator set";
            return false;
        }
        if (vote.validator_address != crypto::addressFromEd25519PublicKey(vote.public_key)) {
            error = "finalization vote address mismatch";
            return false;
        }
        if (vote.record_hash != candidate_hash || vote.round != 1) {
            error = "finalization vote target mismatch";
            return false;
        }
        std::string signature_error;
        if (!crypto::ed25519Verify(
                vote.public_key,
                crypto::recordFinalizationVoteSigningPayload(
                    vote.record_hash, vote.round, vote.validator_address),
                vote.signature,
                signature_error)) {
            error = "invalid finalization vote signature";
            return false;
        }
    }
    return true;
}

} // namespace primechain::protocol
