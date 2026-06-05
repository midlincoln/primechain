#include <iostream>
#include <cstdio>
#include <map>
#include <optional>
#include <string>

#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

class MapProofIndex final : public primechain::math::CompositeProofIndex {
public:
    void add(const primechain::CompositeProof& proof) {
        proofs_[proof.m] = proof;
    }

    std::optional<primechain::CompositeProof> findCompositeProof(primechain::PrimeValue n) const override {
        const auto found = proofs_.find(n);
        if (found == proofs_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

private:
    std::map<primechain::PrimeValue, primechain::CompositeProof> proofs_;
};

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

primechain::protocol::PrimeRecordV0 makePrimeRecord(
    const primechain::node::SequentialNodeStatus& status,
    primechain::PrimeValue p,
    const primechain::math::PrattProof& proof) {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = status.height + 1;
    record.previous_record_hash = status.latest_record_hash;
    record.integer = p;
    record.proof.p = proof.p;
    record.proof.witness = proof.witness;
    for (const auto& factor : proof.factors_of_p_minus_1.factors) {
        record.proof.factors_of_p_minus_1.push_back({factor.prime, factor.exponent});
    }
    record.proof.provider_address = "pcdev1_prime_miner";
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

primechain::protocol::CompositeRecordV0 makeCompositeRecord(
    const primechain::node::SequentialNodeStatus& status,
    const primechain::CompositeProof& proof) {
    primechain::protocol::CompositeRecordV0 record;
    record.version = 0;
    record.height = status.height + 1;
    record.previous_record_hash = status.latest_record_hash;
    record.integer = proof.m;
    record.proof.g = proof.m;
    record.proof.d = proof.d;
    record.proof.e = proof.e;
    record.proof.provider_address = "pcdev1_composite_miner";
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sequential-node-tests <store-path>\n";
        return 1;
    }

    std::string error;
    primechain::node::SequentialNode node(argv[1]);
    if (!expect(node.load(error), "load empty node")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(!node.status().has_genesis, "empty node has no genesis")) {
        return 1;
    }
    error.clear();
    if (!expect(node.initializeGenesis(error), "initialize genesis")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(node.status().frontier_integer == 2, "genesis frontier is 2")) {
        return 1;
    }

    MapProofIndex proofs;

    const auto proof3 = primechain::math::makePrattProof(3, proofs);
    if (!expect(proof3.has_value(), "make Pratt proof for 3")) {
        return 1;
    }
    error.clear();
    if (!expect(node.appendPrime(makePrimeRecord(node.status(), 3, *proof3), error), "append prime 3")) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto proof4 = primechain::math::makeCompositeProof(4, "pcdev1_composite_miner");
    if (!expect(proof4.has_value(), "make composite proof for 4")) {
        return 1;
    }
    error.clear();
    if (!expect(node.appendComposite(makeCompositeRecord(node.status(), *proof4), error), "append composite 4")) {
        std::cerr << error << "\n";
        return 1;
    }
    proofs.add(*proof4);

    const auto proof5 = primechain::math::makePrattProof(5, proofs);
    if (!expect(proof5.has_value(), "make Pratt proof for 5")) {
        return 1;
    }
    error.clear();
    if (!expect(node.appendPrime(makePrimeRecord(node.status(), 5, *proof5), error), "append prime 5")) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto proof6 = primechain::math::makeCompositeProof(6, "pcdev1_composite_miner");
    if (!expect(proof6.has_value(), "make composite proof for 6")) {
        return 1;
    }
    auto skipped = makeCompositeRecord(node.status(), *proof6);
    skipped.integer = 7;
    skipped.proof.g = 7;
    error.clear();
    if (!expect(!node.appendComposite(skipped, error), "reject skipped integer")) {
        return 1;
    }

    primechain::node::SequentialNode reloaded(argv[1]);
    error.clear();
    if (!expect(reloaded.load(error), "reload node")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(reloaded.status().has_genesis, "reloaded has genesis")) {
        return 1;
    }
    if (!expect(reloaded.status().height == 3, "reloaded height 3")) {
        return 1;
    }
    if (!expect(reloaded.status().frontier_integer == 5, "reloaded frontier 5")) {
        return 1;
    }
    if (!expect(reloaded.totalSupplyMicroUnits(3) == primechain::node::kAssetMicroUnits, "prime 3 supply conserved")) {
        return 1;
    }
    if (!expect(reloaded.balanceMicroUnits("pcdev1_prime_miner", 3) == primechain::node::kAssetMicroUnits, "prime 3 reward to prime miner")) {
        return 1;
    }
    if (!expect(reloaded.totalSupplyMicroUnits(5) == primechain::node::kAssetMicroUnits, "prime 5 supply conserved")) {
        return 1;
    }
    if (!expect(reloaded.balanceMicroUnits("pcdev1_prime_miner", 5) == 500000, "prime 5 reward to prime miner")) {
        return 1;
    }
    if (!expect(reloaded.balanceMicroUnits("pcdev1_composite_miner", 5) == 500000, "prime 5 reward to composite miner")) {
        return 1;
    }

    auto tx_record = makeCompositeRecord(node.status(), *proof6);
    tx_record.tx_batch.transaction_count = 1;
    primechain::protocol::applyDevelopmentFinalization(tx_record);
    error.clear();
    if (!expect(!node.appendComposite(tx_record, error), "reject non-empty tx batch until transaction application exists")) {
        return 1;
    }
    primechain::node::SequentialNode after_rejected_tx(argv[1]);
    error.clear();
    if (!expect(after_rejected_tx.load(error), "reload after rejected tx batch")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(after_rejected_tx.status().frontier_integer == 5, "rejected tx batch was not stored")) {
        return 1;
    }

    const std::string bad_path = std::string(argv[1]) + ".bad-composite";
    std::remove(bad_path.c_str());
    primechain::node::SequentialNode bad_node(bad_path);
    error.clear();
    if (!expect(bad_node.load(error), "load empty bad-node store")) {
        std::cerr << error << "\n";
        return 1;
    }
    error.clear();
    if (!expect(bad_node.initializeGenesis(error), "initialize bad-node genesis")) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto bad_proof3 = primechain::math::makePrattProof(3, proofs);
    if (!expect(bad_proof3.has_value(), "make bad-node Pratt proof for 3")) {
        return 1;
    }
    error.clear();
    if (!expect(bad_node.appendPrime(makePrimeRecord(bad_node.status(), 3, *bad_proof3), error), "append bad-node prime 3")) {
        std::cerr << error << "\n";
        return 1;
    }
    primechain::protocol::CompositeRecordV0 bad_composite;
    bad_composite.version = 0;
    bad_composite.height = bad_node.status().height + 1;
    bad_composite.previous_record_hash = bad_node.status().latest_record_hash;
    bad_composite.integer = 4;
    bad_composite.proof.g = 4;
    bad_composite.proof.d = 2;
    bad_composite.proof.e = 3;
    bad_composite.proof.provider_address = "pcdev1_composite_miner";
    primechain::protocol::applyDevelopmentFinalization(bad_composite);
    primechain::storage::RecordStore bad_store(bad_path);
    error.clear();
    if (!expect(bad_store.append(primechain::storage::makeStoredRecord(bad_composite), error), "append hashed bad composite directly to store")) {
        std::cerr << error << "\n";
        return 1;
    }
    primechain::node::SequentialNode bad_reload(bad_path);
    error.clear();
    if (!expect(!bad_reload.load(error), "reject bad composite on replay")) {
        return 1;
    }

    const std::string bad_prime_path = std::string(argv[1]) + ".bad-prime";
    std::remove(bad_prime_path.c_str());
    primechain::node::SequentialNode bad_prime_node(bad_prime_path);
    error.clear();
    if (!expect(bad_prime_node.load(error), "load empty bad-prime store")) {
        std::cerr << error << "\n";
        return 1;
    }
    error.clear();
    if (!expect(bad_prime_node.initializeGenesis(error), "initialize bad-prime genesis")) {
        std::cerr << error << "\n";
        return 1;
    }
    primechain::protocol::PrimeRecordV0 bad_prime;
    bad_prime.version = 0;
    bad_prime.height = bad_prime_node.status().height + 1;
    bad_prime.previous_record_hash = bad_prime_node.status().latest_record_hash;
    bad_prime.integer = 3;
    bad_prime.proof.p = 3;
    bad_prime.proof.witness = 2;
    bad_prime.proof.factors_of_p_minus_1.push_back({2, 2});
    bad_prime.proof.provider_address = "pcdev1_prime_miner";
    primechain::protocol::applyDevelopmentFinalization(bad_prime);
    primechain::storage::RecordStore bad_prime_store(bad_prime_path);
    error.clear();
    if (!expect(bad_prime_store.append(primechain::storage::makeStoredRecord(bad_prime), error), "append hashed bad prime directly to store")) {
        std::cerr << error << "\n";
        return 1;
    }
    primechain::node::SequentialNode bad_prime_reload(bad_prime_path);
    error.clear();
    if (!expect(!bad_prime_reload.load(error), "reject bad prime on replay")) {
        return 1;
    }

    std::cout << "sequential node tests passed\n";
    return 0;
}
