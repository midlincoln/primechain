#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "primechain/math/number_theory.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

class StoreProofIndex final : public primechain::math::CompositeProofIndex {
public:
    void add(const primechain::CompositeProof& proof) {
        proofs_[proof.m] = proof;
    }

    std::optional<primechain::CompositeProof> findCompositeProof(primechain::PrimeValue n) const override {
        const auto found = proofs_.find(n);
        if (found == proofs_.end()) return std::nullopt;
        return found->second;
    }

private:
    std::map<primechain::PrimeValue, primechain::CompositeProof> proofs_;
};

primechain::CompositeProof toCompositeProof(const primechain::protocol::CompositeProofV0& proof) {
    primechain::CompositeProof out;
    out.m = proof.g;
    out.d = proof.d;
    out.e = proof.e;
    out.provider_address = proof.provider_address;
    out.signature = proof.signature;
    return out;
}

bool loadProofIndex(const std::string& store_path, StoreProofIndex& index, std::string& error) {
    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) return false;
    for (const auto& stored : records) {
        if (stored.kind != primechain::storage::StoredRecordKind::Composite) continue;
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) return false;
        const auto proof = toCompositeProof(decoded->proof);
        if (!primechain::math::verifyCompositeProof(proof)) {
            error = "stored composite proof is invalid";
            return false;
        }
        index.add(proof);
    }
    return true;
}

void printFactorization(const primechain::math::Factorization& factorization) {
    std::cout << "FACTOR " << factorization.factors.size();
    for (const auto& factor : factorization.factors) {
        std::cout << " " << factor.prime << "^" << factor.exponent;
    }
    std::cout << "\n";
}

void printPrattProof(const primechain::math::PrattProof& proof) {
    std::cout << "PRATT " << proof.p << " witness=" << proof.witness << " factors=";
    for (std::size_t i = 0; i < proof.factors_of_p_minus_1.factors.size(); ++i) {
        if (i != 0) std::cout << ",";
        const auto& factor = proof.factors_of_p_minus_1.factors[i];
        std::cout << factor.prime << "^" << factor.exponent;
    }
    std::cout << "\n";
}

std::string directoryName(const std::string& path) {
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) return ".";
    if (separator == 0) return "/";
    return path.substr(0, separator);
}

int runTool(const std::string& argv0, const std::string& tool, std::vector<std::string> args) {
    const std::string path = directoryName(argv0) + "/" + tool;
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(path);
    for (auto& arg : args) owned.push_back(std::move(arg));

    std::vector<char*> raw;
    raw.reserve(owned.size() + 1);
    for (auto& arg : owned) raw.push_back(&arg[0]);
    raw.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "could not start " << tool << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0) {
        execv(path.c_str(), raw.data());
        std::cerr << "could not execute " << path << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        std::cerr << "could not wait for " << tool << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        std::cerr << tool << " terminated by signal " << WTERMSIG(status) << "\n";
    }
    return 1;
}

std::vector<std::string> tail(int argc, char** argv, int first) {
    std::vector<std::string> out;
    for (int i = first; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " status <host> <port>\n"
              << "  " << argv0 << " query <host> <port> <command...>\n"
              << "  " << argv0 << " sync <host> <port> <start> <end> <output-store>\n"
              << "  " << argv0 << " inspect <record-store> [integer]\n"
              << "  " << argv0 << " inspect <record-store> --range <start> <end>\n"
              << "  " << argv0 << " new-miner <wallet-file>\n"
              << "  " << argv0 << " address <wallet-file>\n"
              << "  " << argv0 << " balance <record-store> <wallet-file>\n"
              << "  " << argv0 << " mine <host> <port> <limit> --prime-identity <file> --composite-identity <file>\n"
              << "  " << argv0 << " is-prime <n>\n"
              << "  " << argv0 << " divisor <n>\n"
              << "  " << argv0 << " factor <record-store> <n>\n"
              << "  " << argv0 << " pratt <record-store> <prime>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    const std::string command = argv[1];
    if (command == "status") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-sync-query", {argv[2], argv[3], "GET_STATUS"});
    }
    if (command == "query") {
        if (argc < 5) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-sync-query", tail(argc, argv, 2));
    }
    if (command == "sync") {
        if (argc != 7) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-sync-download", tail(argc, argv, 2));
    }
    if (command == "inspect") {
        if (argc < 3) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-store-inspect", tail(argc, argv, 2));
    }
    if (command == "new-miner") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-wallet", {"new-miner", argv[2]});
    }
    if (command == "address") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-wallet", {"address", argv[2]});
    }
    if (command == "balance") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-wallet", {"balance", argv[2], argv[3]});
    }
    if (command == "mine") {
        if (argc != 9) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-frontier-miner", tail(argc, argv, 2));
    }
    if (command == "is-prime") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        const primechain::PrimeValue n = std::stoull(argv[2]);
        std::cout << (primechain::math::isPrime(n) ? "PRIME " : "COMPOSITE ") << n << "\n";
        return 0;
    }
    if (command == "divisor") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        const primechain::PrimeValue n = std::stoull(argv[2]);
        const auto proof = primechain::math::makeCompositeProof(n, "pcdev1_client_workbench");
        if (!proof.has_value()) {
            std::cerr << "no nontrivial divisor found for " << n << "\n";
            return 1;
        }
        std::cout << "DIVISOR " << n << " " << proof->d << " " << proof->e << "\n";
        return 0;
    }
    if (command == "factor") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        const primechain::PrimeValue n = std::stoull(argv[3]);
        StoreProofIndex index;
        std::string error;
        if (!loadProofIndex(argv[2], index, error)) {
            std::cerr << "could not load proof index: " << error << "\n";
            return 1;
        }
        const auto factorization = primechain::math::factorizeFromProofIndex(n, index);
        if (!factorization.has_value()) {
            std::cerr << "factorization unavailable for " << n << "\n";
            return 1;
        }
        printFactorization(*factorization);
        return 0;
    }
    if (command == "pratt") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        const primechain::PrimeValue p = std::stoull(argv[3]);
        StoreProofIndex index;
        std::string error;
        if (!loadProofIndex(argv[2], index, error)) {
            std::cerr << "could not load proof index: " << error << "\n";
            return 1;
        }
        const auto proof = primechain::math::makePrattProof(p, index);
        if (!proof.has_value()) {
            std::cerr << "Pratt proof unavailable for " << p << "\n";
            return 1;
        }
        printPrattProof(*proof);
        return 0;
    }

    std::cerr << "unknown client command: " << command << "\n";
    printUsage(argv[0]);
    return 1;
}
