#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include <sys/stat.h>

#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/types.hpp"

namespace {

constexpr const char* kDefaultOutputPath = "data/sequential-chain.log";
constexpr const char* kDefaultRecordStorePath = "data/sequential-chain.dat";

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
    std::cerr << "usage: " << argv0 << " [limit] [text_log_path] [record_store_path]\n"
              << "example:\n"
              << "  " << argv0 << " 500 ./data/sequential-500.log ./data/sequential-500.dat\n";
}

primechain::protocol::PrimeRecordV0 makePrimeRecord(
    const primechain::node::SequentialNodeStatus& status,
    primechain::PrimeValue p,
    const primechain::math::PrattProof& proof) {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = status.has_genesis ? status.height + 1 : 0;
    record.previous_record_hash = status.latest_record_hash;
    record.integer = p;
    record.proof.p = proof.p;
    record.proof.witness = proof.witness;
    for (const auto& factor : proof.factors_of_p_minus_1.factors) {
        record.proof.factors_of_p_minus_1.push_back({factor.prime, factor.exponent});
    }
    record.proof.provider_address = p == 2 ? "pcdev1_genesis" : "pcdev1_prime_miner";
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
    return record;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const primechain::PrimeValue limit = argc > 1 ? std::stoull(argv[1]) : 500;
    const std::string output_path = argc > 2 ? argv[2] : kDefaultOutputPath;
    const std::string record_store_path = argc > 3 ? argv[3] : kDefaultRecordStorePath;
    if (limit < 2) {
        printUsage(argv[0]);
        return 1;
    }
    if (!ensureParentDataDir(output_path) || !ensureParentDataDir(record_store_path)) {
        std::cerr << "could not create parent data directory\n";
        return 1;
    }

    std::ofstream out(output_path, std::ios::trunc);
    if (!out) {
        std::cerr << "could not open " << output_path << " for writing\n";
        return 1;
    }

    MapProofIndex proofs;
    std::string error;
    primechain::node::SequentialNode node(record_store_path);
    if (!node.load(error)) {
        std::cerr << "could not load record store: " << error << "\n";
        return 1;
    }
    if (node.status().has_genesis) {
        std::cerr << "record store already contains records; use a fresh store path for this generator\n";
        return 1;
    }
    if (!node.initializeGenesis(error)) {
        std::cerr << "could not initialize genesis: " << error << "\n";
        return 1;
    }

    std::uint64_t prime_count = 0;
    std::uint64_t composite_count = 0;

    for (primechain::PrimeValue n = 2; n <= limit; ++n) {
        if (primechain::math::isPrime(n)) {
            const auto proof = primechain::math::makePrattProof(n, proofs);
            if (!proof.has_value() || !primechain::math::verifyPrattProof(*proof)) {
                std::cerr << "could not create valid Pratt proof for " << n << "\n";
                return 1;
            }

            if (n != 2) {
                auto record = makePrimeRecord(node.status(), n, *proof);
                error.clear();
                if (!node.appendPrime(record, error)) {
                    std::cerr << "could not append prime record for " << n << ": " << error << "\n";
                    return 1;
                }
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

        auto record = makeCompositeRecord(node.status(), *proof);
        error.clear();
        if (!node.appendComposite(record, error)) {
            std::cerr << "could not append composite record for " << n << ": " << error << "\n";
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

    primechain::node::SequentialNode reloaded(record_store_path);
    error.clear();
    if (!reloaded.load(error)) {
        std::cerr << "could not reload record store: " << error << "\n";
        return 1;
    }
    if (!reloaded.status().has_genesis || reloaded.status().frontier_integer != limit) {
        std::cerr << "reloaded frontier mismatch\n";
        return 1;
    }

    std::cout << "sequential chain complete\n";
    std::cout << "output_path: " << output_path << "\n";
    std::cout << "record_store_path: " << record_store_path << "\n";
    std::cout << "limit: " << limit << "\n";
    std::cout << "prime_records: " << prime_count << "\n";
    std::cout << "composite_records: " << composite_count << "\n";
    return 0;
}
