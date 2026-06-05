#include "primechain/protocol/records.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>
#include <string_view>

#include "primechain/crypto/hash.hpp"

namespace primechain::protocol {

namespace {

constexpr std::uint64_t kCompositeRecordTag = 1;
constexpr std::uint64_t kPrimeRecordTag = 2;
constexpr std::string_view kDevelopmentFinalizationRule = "fixed-2-of-3-dev";
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
           reader.readHash(vote.record_hash) &&
           reader.readUint64(vote.round) &&
           reader.readBytes(vote.signature);
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

Address developmentAddressFromPublicKey(const Bytes& public_key) {
    const Hash256 hash = crypto::devHash256(public_key);
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
    return crypto::devHash256(serializeTransaction(tx, true));
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
    return crypto::devHash256(payload);
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
    return crypto::devHash256(serializeCompositeRecordInternal(record, false));
}

Hash256 candidateRecordHash(const PrimeRecordV0& record) {
    return crypto::devHash256(serializePrimeRecordInternal(record, false));
}

Hash256 finalizedRecordHash(const CompositeRecordV0& record) {
    return crypto::devHash256(serializeCompositeRecord(record));
}

Hash256 finalizedRecordHash(const PrimeRecordV0& record) {
    return crypto::devHash256(serializePrimeRecord(record));
}

Bytes developmentVoteSignature(const Address& validator_address, const Hash256& record_hash, std::uint64_t round) {
    std::vector<std::uint8_t> payload;
    appendString(payload, kDevelopmentVoteDomain);
    appendAddress(payload, validator_address);
    appendHash(payload, record_hash);
    appendUint64(payload, round);

    const Hash256 hash = crypto::devHash256(payload);
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
    const Hash256 hash = crypto::devHash256(payload);
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

} // namespace primechain::protocol
