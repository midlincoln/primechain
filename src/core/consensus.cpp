#include "primechain/core/consensus.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include "primechain/crypto/hash.hpp"
#include "primechain/math/number_theory.hpp"

namespace primechain::core {

namespace {

void appendUint64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void appendHash(std::vector<std::uint8_t>& out, const Hash256& hash) {
    out.insert(out.end(), hash.begin(), hash.end());
}

void appendString(std::vector<std::uint8_t>& out, std::string_view value) {
    appendUint64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

} // namespace

std::size_t requiredValidatorQuorum(std::size_t validator_count) {
    if (validator_count == 0) return 0;
    if (validator_count == 1) return 1;
    return (validator_count * 2 + 2) / 3;
}

bool validValidatorSetSize(std::size_t validator_count) {
    return validator_count >= 1;
}

Hash256 blockHash(const BlockHeader& header) {
    std::vector<std::uint8_t> bytes;
    appendUint64(bytes, header.version);
    appendHash(bytes, header.previous_block_hash);
    appendUint64(bytes, header.prime_value);
    appendHash(bytes, header.prime_certificate_hash);
    appendUint64(bytes, header.composite_range_start);
    appendUint64(bytes, header.composite_range_end);
    appendHash(bytes, header.composite_merkle_root);
    appendHash(bytes, header.transaction_merkle_root);
    appendHash(bytes, header.state_commitment_root);
    appendUint64(bytes, header.timestamp);
    appendString(bytes, header.miner_address);
    return crypto::sha3_256(bytes);
}

bool ConsensusEngine::validateBlock(const Block& block, const ChainState& previous, std::string& error) const {
    if (!validateHeader(block, previous, error)) {
        return false;
    }
    if (!math::verifyPrimeCertificate(block.header.prime_value, block.prime_certificate)) {
        error = "prime certificate does not verify";
        return false;
    }
    if (!validateCompositeCoverage(block, previous, error)) {
        return false;
    }
    return true;
}

ChainState ConsensusEngine::applyBlock(const Block& block, const ChainState& previous) const {
    ChainState next;
    next.height = previous.height + 1;
    next.last_block_hash = blockHash(block.header);
    next.frontier_prime = block.header.prime_value;
    return next;
}

bool ConsensusEngine::validateHeader(const Block& block, const ChainState& previous, std::string& error) const {
    const PrimeValue expected = math::nextPrimeAfter(previous.frontier_prime);
    if (expected == 0) {
        error = "next prime overflow";
        return false;
    }
    if (block.header.previous_block_hash != previous.last_block_hash) {
        error = "previous block hash mismatch";
        return false;
    }
    if (block.header.prime_value != expected) {
        error = "block prime is not the next prime after frontier";
        return false;
    }
    if (block.header.composite_range_start != previous.frontier_prime + 1) {
        error = "composite range start is incorrect";
        return false;
    }
    if (block.header.composite_range_end != block.header.prime_value - 1) {
        error = "composite range end is incorrect";
        return false;
    }
    return true;
}

bool ConsensusEngine::validateCompositeCoverage(const Block& block, const ChainState& previous, std::string& error) const {
    const PrimeValue start = previous.frontier_prime + 1;
    const PrimeValue end = block.header.prime_value - 1;

    for (PrimeValue m = start; m <= end; ++m) {
        auto matchesM = [m](const CompositeProof& proof) {
            return proof.m == m;
        };
        const auto first = std::find_if(block.composite_proofs.begin(), block.composite_proofs.end(), matchesM);
        if (first == block.composite_proofs.end()) {
            error = "missing composite proof for " + std::to_string(m);
            return false;
        }
        if (!math::verifyCompositeProof(*first)) {
            error = "invalid composite proof for " + std::to_string(m);
            return false;
        }
        const auto second = std::find_if(std::next(first), block.composite_proofs.end(), matchesM);
        if (second != block.composite_proofs.end()) {
            error = "duplicate composite proof for " + std::to_string(m);
            return false;
        }
    }

    return true;
}

} // namespace primechain::core
