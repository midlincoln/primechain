#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

struct PeerConfig {
    std::string host;
    int port{0};
};

struct StatusLine {
    std::uint64_t records{0};
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    bool has_genesis{false};
    std::uint64_t height{0};
    primechain::PrimeValue frontier{0};
    std::string latest_hash;
};

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

std::string joinPath(const std::string& left, const std::string& right) {
    if (left.empty() || left == ".") return right;
    if (left.back() == '/') return left + right;
    return left + "/" + right;
}

bool pathExists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool ensureDirectory(const std::string& path) {
    if (path.empty() || path == "." || path == "/") return true;
    if (pathExists(path)) return true;
    if (!ensureDirectory(directoryName(path))) return false;
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    std::cerr << "could not create directory " << path << ": " << std::strerror(errno) << "\n";
    return false;
}

std::string configPath(const std::string& workdir) { return joinPath(workdir, "client.conf"); }
std::string dataDir(const std::string& workdir) { return joinPath(workdir, "data"); }
std::string walletsDir(const std::string& workdir) { return joinPath(workdir, "wallets"); }
std::string jobsDir(const std::string& workdir) { return joinPath(workdir, "jobs"); }
std::string logsDir(const std::string& workdir) { return joinPath(workdir, "logs"); }
std::string chainPath(const std::string& workdir) { return joinPath(dataDir(workdir), "chain.dat"); }
std::string primeWalletPath(const std::string& workdir) { return joinPath(walletsDir(workdir), "prime.wallet"); }
std::string compositeWalletPath(const std::string& workdir) { return joinPath(walletsDir(workdir), "composite.wallet"); }
std::string mineStatePath(const std::string& workdir) { return joinPath(jobsDir(workdir), "mine.state"); }

std::map<std::string, std::string> readKeyValueFile(const std::string& path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

bool ensureWorkdirLayout(const std::string& workdir) {
    return ensureDirectory(workdir) && ensureDirectory(dataDir(workdir)) && ensureDirectory(walletsDir(workdir))
        && ensureDirectory(jobsDir(workdir)) && ensureDirectory(logsDir(workdir));
}

bool writeConfig(const std::string& workdir, const std::optional<PeerConfig>& peer) {
    if (!ensureWorkdirLayout(workdir)) return false;
    std::ofstream out(configPath(workdir), std::ios::trunc);
    if (!out) {
        std::cerr << "could not write " << configPath(workdir) << "\n";
        return false;
    }
    out << "version=primechain-client-v1\n";
    if (peer.has_value()) {
        out << "peer_host=" << peer->host << "\n";
        out << "peer_port=" << peer->port << "\n";
    }
    return true;
}

std::optional<PeerConfig> readPeerConfig(const std::string& workdir) {
    const auto values = readKeyValueFile(configPath(workdir));
    const auto host = values.find("peer_host");
    const auto port = values.find("peer_port");
    if (host == values.end() || port == values.end()) return std::nullopt;
    return PeerConfig{host->second, std::stoi(port->second)};
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

bool captureTool(const std::string& argv0, const std::string& tool, std::vector<std::string> args, std::string& output) {
    int fds[2];
    if (pipe(fds) != 0) {
        std::cerr << "pipe failed: " << std::strerror(errno) << "\n";
        return false;
    }

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
        close(fds[0]);
        close(fds[1]);
        std::cerr << "could not start " << tool << ": " << std::strerror(errno) << "\n";
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execv(path.c_str(), raw.data());
        _exit(127);
    }

    close(fds[1]);
    output.clear();
    char buffer[4096];
    while (true) {
        const ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::optional<StatusLine> parseStatus(const std::string& text) {
    std::istringstream in(text);
    std::string tag;
    StatusLine status;
    int has_genesis = 0;
    in >> tag >> status.records >> status.prime_records >> status.composite_records >> has_genesis
       >> status.height >> status.frontier >> status.latest_hash;
    if (!in || tag != "STATUS") return std::nullopt;
    status.has_genesis = has_genesis != 0;
    return status;
}

std::optional<StatusLine> queryPeerStatus(const std::string& argv0, const PeerConfig& peer) {
    std::string output;
    if (!captureTool(argv0, "primechain-sync-query", {peer.host, std::to_string(peer.port), "GET_STATUS"}, output)) {
        std::cerr << "could not query peer status\n";
        return std::nullopt;
    }
    auto status = parseStatus(output);
    if (!status.has_value()) {
        std::cerr << "unexpected peer status: " << output;
        return std::nullopt;
    }
    return status;
}

StatusLine loadLocalStatus(const std::string& path) {
    StatusLine out;
    primechain::node::SequentialNode node(path);
    std::string error;
    if (!node.load(error)) return out;
    const auto& status = node.status();
    out.has_genesis = status.has_genesis;
    out.height = status.height;
    out.frontier = status.frontier_integer;
    out.records = status.has_genesis ? status.height + 1 : 0;
    return out;
}

bool ensureWallet(const std::string& argv0, const std::string& path) {
    if (pathExists(path)) return true;
    return runTool(argv0, "primechain-wallet", {"new-miner", path}) == 0;
}

int initWorkdir(const char* argv0, int argc, char** argv) {
    if (argc != 3 && argc != 5) return 1;
    const std::string workdir = argv[2];
    std::optional<PeerConfig> peer;
    if (argc == 5) peer = PeerConfig{argv[3], std::stoi(argv[4])};
    if (!writeConfig(workdir, peer)) return 1;
    if (!ensureWallet(argv0, primeWalletPath(workdir)) || !ensureWallet(argv0, compositeWalletPath(workdir))) return 1;
    std::cout << "WORKDIR_INITIALIZED " << workdir << "\n";
    if (peer.has_value()) std::cout << "PEER " << peer->host << " " << peer->port << "\n";
    std::cout << "CHAIN " << chainPath(workdir) << "\n";
    return 0;
}

int syncWorkdir(const char* argv0, const std::string& workdir, const PeerConfig& peer) {
    if (!ensureWorkdirLayout(workdir)) return 1;
    const auto remote = queryPeerStatus(argv0, peer);
    if (!remote.has_value()) return 1;
    const auto local = loadLocalStatus(chainPath(workdir));
    const primechain::PrimeValue start = local.has_genesis ? local.frontier + 1 : 2;
    if (remote->frontier < start) {
        std::cout << "SYNC_UP_TO_DATE " << local.frontier << "\n";
        return 0;
    }
    const int rc = runTool(argv0, "primechain-sync-download", {
        peer.host,
        std::to_string(peer.port),
        std::to_string(start),
        std::to_string(remote->frontier),
        chainPath(workdir),
    });
    if (rc != 0) return rc;
    std::cout << "SYNCED " << start << " " << remote->frontier << "\n";
    return 0;
}

int syncPeer(const char* argv0, int argc, char** argv) {
    if (argc != 3 && argc != 5) return 1;
    const std::string workdir = argv[2];
    std::optional<PeerConfig> peer;
    if (argc == 5) {
        peer = PeerConfig{argv[3], std::stoi(argv[4])};
        if (!writeConfig(workdir, peer)) return 1;
    } else {
        peer = readPeerConfig(workdir);
    }
    if (!peer.has_value()) {
        std::cerr << "no peer configured; pass host and port\n";
        return 1;
    }
    return syncWorkdir(argv0, workdir, *peer);
}

int jobStatus(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    const auto peer = readPeerConfig(workdir);
    const auto local = loadLocalStatus(chainPath(workdir));
    std::cout << "WORKDIR " << workdir << "\n";
    if (peer.has_value()) {
        std::cout << "PEER " << peer->host << " " << peer->port << "\n";
    } else {
        std::cout << "PEER none\n";
    }
    std::cout << "LOCAL_FRONTIER " << local.frontier << "\n";
    const auto state = readKeyValueFile(mineStatePath(workdir));
    const auto target = state.find("target");
    if (target != state.end()) std::cout << "MINE_TARGET " << target->second << "\n";
    return 0;
}

int mineJob(const char* argv0, int argc, char** argv) {
    if (argc != 5 || std::string(argv[3]) != "--target") return 1;
    const std::string workdir = argv[2];
    const std::string target = argv[4];
    const auto peer = readPeerConfig(workdir);
    if (!peer.has_value()) {
        std::cerr << "no peer configured; run init-workdir with host and port first\n";
        return 1;
    }
    if (!ensureWorkdirLayout(workdir)) return 1;
    if (!ensureWallet(argv0, primeWalletPath(workdir)) || !ensureWallet(argv0, compositeWalletPath(workdir))) return 1;
    {
        std::ofstream out(mineStatePath(workdir), std::ios::trunc);
        if (!out) {
            std::cerr << "could not write " << mineStatePath(workdir) << "\n";
            return 1;
        }
        out << "target=" << target << "\n";
    }
    const int mined = runTool(argv0, "primechain-frontier-miner", {
        peer->host,
        std::to_string(peer->port),
        target,
        "--prime-identity",
        primeWalletPath(workdir),
        "--composite-identity",
        compositeWalletPath(workdir),
    });
    if (mined != 0) return mined;
    return syncWorkdir(argv0, workdir, *peer);
}

std::vector<std::string> tail(int argc, char** argv, int first) {
    std::vector<std::string> out;
    for (int i = first; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " init-workdir <workdir> [host port]\n"
              << "  " << argv0 << " sync-peer <workdir> [host port]\n"
              << "  " << argv0 << " job-status <workdir>\n"
              << "  " << argv0 << " mine-job <workdir> --target <integer>\n"
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
    if (command == "init-workdir") {
        if (argc != 3 && argc != 5) { printUsage(argv[0]); return 1; }
        return initWorkdir(argv[0], argc, argv);
    }
    if (command == "sync-peer") {
        if (argc != 3 && argc != 5) { printUsage(argv[0]); return 1; }
        return syncPeer(argv[0], argc, argv);
    }
    if (command == "job-status") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return jobStatus(argc, argv);
    }
    if (command == "mine-job") {
        if (argc != 5) { printUsage(argv[0]); return 1; }
        return mineJob(argv[0], argc, argv);
    }
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
