#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "primechain/math/number_theory.hpp"

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

MapProofIndex makeIndexUpTo(primechain::PrimeValue n) {
    MapProofIndex index;
    for (primechain::PrimeValue m = 4; m <= n; ++m) {
        const auto proof = primechain::math::makeCompositeProof(m, "test-factor-miner");
        if (proof.has_value()) {
            index.add(*proof);
        }
    }
    return index;
}

bool equals(
    const std::vector<primechain::math::PrimePowerFactor>& actual,
    const std::vector<primechain::math::PrimePowerFactor>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].prime != expected[i].prime || actual[i].exponent != expected[i].exponent) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const auto index = makeIndexUpTo(600);

    const auto factor_30 = primechain::math::factorizeFromProofIndex(30, index);
    if (!expect(factor_30.has_value(), "factorize 30")) {
        return 1;
    }
    if (!expect(equals(*factor_30, {{2, 1}, {3, 1}, {5, 1}}), "30 = 2 * 3 * 5")) {
        return 1;
    }

    const auto factor_504 = primechain::math::factorizeFromProofIndex(504, index);
    if (!expect(factor_504.has_value(), "factorize 504")) {
        return 1;
    }
    if (!expect(equals(*factor_504, {{2, 3}, {3, 2}, {7, 1}}), "504 = 2^3 * 3^2 * 7")) {
        return 1;
    }

    const auto factor_prime = primechain::math::factorizeFromProofIndex(97, index);
    if (!expect(factor_prime.has_value(), "factorize prime 97")) {
        return 1;
    }
    if (!expect(equals(*factor_prime, {{97, 1}}), "97 = 97")) {
        return 1;
    }

    MapProofIndex incomplete;
    const auto proof_30 = primechain::math::makeCompositeProof(30, "test-factor-miner");
    if (proof_30.has_value()) {
        incomplete.add(*proof_30);
    }
    if (!expect(!primechain::math::factorizeFromProofIndex(30, incomplete).has_value(), "reject incomplete recursive proof index")) {
        return 1;
    }

    if (!expect(!primechain::math::factorizeFromProofIndex(1, index).has_value(), "reject n < 2")) {
        return 1;
    }

    std::cout << "factorization tests passed\n";
    return 0;
}
