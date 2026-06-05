#pragma once

#include <cstdint>
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
    Hash256 record_hash{};
    std::uint64_t round{0};
    Bytes signature;
};

struct FinalizationProofV0 {
    std::string rule{"fixed-2-of-3-dev"};
    std::vector<ValidatorVoteV0> votes;
};

struct CompositeRecordV0 {
    std::uint64_t version{0};
    std::uint64_t height{0};
    Hash256 previous_record_hash{};
    PrimeValue integer{0};
    CompositeProofV0 proof;
    TransactionBatchV0 tx_batch;
    Hash256 state_root{};
    FinalizationProofV0 finalized_by;
};

struct PrimeRecordV0 {
    std::uint64_t version{0};
    std::uint64_t height{0};
    Hash256 previous_record_hash{};
    PrimeValue integer{0};
    PrattPrimeProofV0 proof;
    TransactionBatchV0 tx_batch;
    Hash256 state_root{};
    FinalizationProofV0 finalized_by;
};

bool isDevelopmentAddress(const Address& address);

std::vector<std::uint8_t> serializeTransaction(const TransactionV0& tx, bool include_signature);
std::vector<std::uint8_t> serializeCompositeRecord(const CompositeRecordV0& record);
std::vector<std::uint8_t> serializePrimeRecord(const PrimeRecordV0& record);

Hash256 transactionHash(const TransactionV0& tx);
Hash256 candidateRecordHash(const CompositeRecordV0& record);
Hash256 candidateRecordHash(const PrimeRecordV0& record);
Hash256 finalizedRecordHash(const CompositeRecordV0& record);
Hash256 finalizedRecordHash(const PrimeRecordV0& record);

} // namespace primechain::protocol
