#include "primechain/protocol/records.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "primechain/crypto/hash.hpp"

namespace primechain::protocol {

namespace {

constexpr std::uint64_t kCompositeRecordTag = 1;
constexpr std::uint64_t kPrimeRecordTag = 2;

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
    if (include_signature) {
        appendBytes(out, tx.signature);
    } else {
        appendUint64(out, 0);
    }
    return out;
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

} // namespace primechain::protocol
