#include <iostream>
#include <string>

#include "primechain/math/number_theory.hpp"

namespace {

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

primechain::math::PrattProof proofFor11() {
    primechain::math::PrattProof proof;
    proof.p = 11;
    proof.witness = 2;
    proof.factors_of_p_minus_1.factors = {{2, 1}, {5, 1}};
    return proof;
}

primechain::math::PrattProof proofFor97() {
    primechain::math::PrattProof proof;
    proof.p = 97;
    proof.witness = 5;
    proof.factors_of_p_minus_1.factors = {{2, 5}, {3, 1}};
    return proof;
}

} // namespace

int main() {
    primechain::math::PrattProof proof2;
    proof2.p = 2;
    proof2.witness = 0;
    if (!expect(primechain::math::verifyPrattProof(proof2), "genesis Pratt proof for 2")) {
        return 1;
    }

    const auto proof11 = proofFor11();
    if (!expect(primechain::math::verifyPrattProof(proof11), "valid Pratt proof for 11")) {
        return 1;
    }

    auto bad_product = proof11;
    bad_product.factors_of_p_minus_1.factors = {{2, 1}, {3, 1}};
    if (!expect(!primechain::math::verifyPrattProof(bad_product), "reject wrong p-1 factorization product")) {
        return 1;
    }

    auto bad_witness = proof11;
    bad_witness.witness = 10;
    if (!expect(!primechain::math::verifyPrattProof(bad_witness), "reject invalid witness for 11")) {
        return 1;
    }

    auto composite = proof11;
    composite.p = 9;
    composite.witness = 2;
    composite.factors_of_p_minus_1.factors = {{2, 3}};
    if (!expect(!primechain::math::verifyPrattProof(composite), "reject composite candidate 9")) {
        return 1;
    }

    const auto proof97 = proofFor97();
    if (!expect(primechain::math::verifyPrattProof(proof97), "valid Pratt proof for 97")) {
        return 1;
    }

    auto unsorted = proof97;
    unsorted.factors_of_p_minus_1.factors = {{3, 1}, {2, 5}};
    if (!expect(!primechain::math::verifyPrattProof(unsorted), "reject noncanonical factorization")) {
        return 1;
    }

    std::cout << "Pratt tests passed\n";
    return 0;
}
