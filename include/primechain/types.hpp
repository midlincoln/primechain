#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace primechain {

using Hash256 = std::array<std::uint8_t, 32>;
using PrimeValue = std::uint64_t;
using Address = std::string;

struct Rational {
    std::int64_t numerator{0};
    std::int64_t denominator{1};
};

struct BlockHeader {
    std::uint32_t version{1};
    Hash256 previous_block_hash{};
    PrimeValue prime_value{0};
    Hash256 prime_certificate_hash{};
    PrimeValue composite_range_start{0};
    PrimeValue composite_range_end{0};
    Hash256 composite_merkle_root{};
    Hash256 transaction_merkle_root{};
    Hash256 state_commitment_root{};
    std::uint64_t timestamp{0};
    Address miner_address;
};

struct PrimeCertificate {
    // MVP certificate: a placeholder payload. Real ECPP/Pratt certificates will replace this.
    std::vector<std::uint8_t> data;
};

struct CompositeProof {
    PrimeValue m{0};
    PrimeValue d{0};
    PrimeValue e{0};
    std::array<std::uint8_t, 32> nonce{};
    Address provider_address;
    std::vector<std::uint8_t> signature;
};

struct TxInput {
    PrimeValue prime{0};
    Rational amount;
};

struct TxOutput {
    PrimeValue prime{0};
    Rational amount;
    Address receiver_address;
};

struct FeeSpec {
    PrimeValue prime{0};
    Rational amount;
};

struct Transaction {
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    FeeSpec fee;
    Address sender_address;
    std::vector<std::uint8_t> signature;
};

struct RewardRecord {
    PrimeValue prime{0};
    Rational amount;
    Address recipient;
};

struct Block {
    BlockHeader header;
    PrimeCertificate prime_certificate;
    std::vector<CompositeProof> composite_proofs;
    std::vector<Transaction> transactions;
    std::vector<RewardRecord> rewards;
};

struct ChainState {
    std::uint64_t height{0};
    Hash256 last_block_hash{};
    PrimeValue frontier_prime{2};
};

} // namespace primechain
