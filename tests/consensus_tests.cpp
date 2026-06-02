#include <iostream>
#include <string>

#include "primechain/core/consensus.hpp"
#include "primechain/math/number_theory.hpp"

namespace {

primechain::Block makeBlock(const primechain::ChainState& state, primechain::PrimeValue prime) {
    primechain::Block block;
    block.header.previous_block_hash = state.last_block_hash;
    block.header.prime_value = prime;
    block.header.composite_range_start = state.frontier_prime + 1;
    block.header.composite_range_end = prime - 1;
    block.header.miner_address = "test-miner";
    block.prime_certificate.data = {'t'};

    for (primechain::PrimeValue m = block.header.composite_range_start; m <= block.header.composite_range_end; ++m) {
        auto proof = primechain::math::makeCompositeProof(m, "test-provider");
        if (proof.has_value()) {
            block.composite_proofs.push_back(*proof);
        }
    }

    return block;
}

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    primechain::core::ConsensusEngine consensus;
    primechain::ChainState state;
    std::string error;

    primechain::Block valid = makeBlock(state, 3);
    if (!expect(consensus.validateBlock(valid, state, error), "valid next-prime block")) {
        std::cerr << error << "\n";
        return 1;
    }

    primechain::Block skipped = makeBlock(state, 5);
    error.clear();
    if (!expect(!consensus.validateBlock(skipped, state, error), "reject skipped prime")) {
        return 1;
    }

    state = consensus.applyBlock(valid, state);
    primechain::Block with_composites = makeBlock(state, 5);
    error.clear();
    if (!expect(consensus.validateBlock(with_composites, state, error), "valid empty composite interval")) {
        std::cerr << error << "\n";
        return 1;
    }

    state = consensus.applyBlock(with_composites, state);
    state = consensus.applyBlock(makeBlock(state, 7), state);
    primechain::Block missing = makeBlock(state, 11);
    missing.composite_proofs.pop_back();
    error.clear();
    if (!expect(!consensus.validateBlock(missing, state, error), "reject missing composite proof")) {
        return 1;
    }

    std::cout << "consensus tests passed\n";
    return 0;
}
