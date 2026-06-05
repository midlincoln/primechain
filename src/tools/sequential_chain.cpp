#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include <sys/stat.h>

#include "primechain/math/number_theory.hpp"
#include "primechain/types.hpp"

namespace {

constexpr const char* kDefaultOutputPath = "data/sequential-chain.log";

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

bool ensureParentDataDir(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }
    const std::string dir = path.substr(0, slash);
    if (dir.empty()) {
        return true;
    }
    if (mkdir(dir.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

std::string factorizationString(const primechain::math::Factorization& factorization) {
    std::ostringstream out;
    for (std::size_t i = 0; i < factorization.factors.size(); ++i) {
        if (i > 0) {
            out << "*";
        }
        out << factorization.factors[i].prime << "^" << factorization.factors[i].exponent;
    }
    return out.str();
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [limit] [output_path]\n"
              << "example:\n"
              << "  " << argv0 << " 500 ./data/sequential-500.log\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const primechain::PrimeValue limit = argc > 1 ? std::stoull(argv[1]) : 500;
    const std::string output_path = argc > 2 ? argv[2] : kDefaultOutputPath;
    if (limit < 2) {
        printUsage(argv[0]);
        return 1;
    }
    if (!ensureParentDataDir(output_path)) {
        std::cerr << "could not create parent directory for " << output_path << "\n";
        return 1;
    }

    std::ofstream out(output_path, std::ios::trunc);
    if (!out) {
        std::cerr << "could not open " << output_path << " for writing\n";
        return 1;
    }

    MapProofIndex proofs;
    std::uint64_t prime_count = 0;
    std::uint64_t composite_count = 0;

    for (primechain::PrimeValue n = 2; n <= limit; ++n) {
        if (primechain::math::isPrime(n)) {
            const auto proof = primechain::math::makePrattProof(n, proofs);
            if (!proof.has_value() || !primechain::math::verifyPrattProof(*proof)) {
                std::cerr << "could not create valid Pratt proof for " << n << "\n";
                return 1;
            }

            out << n << " PRIME witness=" << proof->witness
                << " factors_p_minus_1=" << factorizationString(proof->factors_of_p_minus_1)
                << "\n";
            ++prime_count;
            continue;
        }

        const auto proof = primechain::math::makeCompositeProof(n, "sequential-composite-miner");
        if (!proof.has_value() || !primechain::math::verifyCompositeProof(*proof)) {
            std::cerr << "could not create valid composite proof for " << n << "\n";
            return 1;
        }
        proofs.add(*proof);

        const auto full_factorization = primechain::math::factorizeFromProofIndex(n, proofs);
        if (!full_factorization.has_value()) {
            std::cerr << "could not reconstruct full factorization for " << n << "\n";
            return 1;
        }

        out << n << " COMPOSITE d=" << proof->d
            << " e=" << proof->e
            << " full_factorization=" << factorizationString(*full_factorization)
            << "\n";
        ++composite_count;
    }

    out.close();
    if (!out) {
        std::cerr << "failed while writing " << output_path << "\n";
        return 1;
    }

    std::cout << "sequential chain complete\n";
    std::cout << "output_path: " << output_path << "\n";
    std::cout << "limit: " << limit << "\n";
    std::cout << "prime_records: " << prime_count << "\n";
    std::cout << "composite_records: " << composite_count << "\n";
    return 0;
}
