#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "primechain/crypto/hash.hpp"
#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/node/validator_registry.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/protocol/validator_governance.hpp"
#include "primechain/storage/record_store.hpp"
#include "primechain/version.hpp"
#include "primechain/wallet/miner_identity.hpp"

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

std::string directoryName(const std::string& path);
bool ensureDirectory(const std::string& path);

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

    const std::map<primechain::PrimeValue, primechain::CompositeProof>& proofs() const { return proofs_; }
    std::size_t size() const { return proofs_.size(); }

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

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

std::vector<std::uint8_t> hexToBytes(const std::string& hex) {
    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    };
    if (hex.size() % 2 != 0) return {};
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = value(hex[i]);
        const int low = value(hex[i + 1]);
        if (high < 0 || low < 0) return {};
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

bool loadProofIndexFile(const std::string& path, StoreProofIndex& index, std::map<std::string, std::string>& metadata, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "proof index is missing; run update-indexes";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.rfind("PROOF ", 0) != 0) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) metadata[line.substr(0, eq)] = line.substr(eq + 1);
            continue;
        }
        std::istringstream proof_in(line);
        std::string tag;
        std::string signature_hex;
        primechain::CompositeProof proof;
        proof_in >> tag >> proof.m >> proof.d >> proof.e >> proof.provider_address >> signature_hex;
        if (!proof_in || tag != "PROOF") {
            error = "malformed proof index line";
            return false;
        }
        if (signature_hex == "-") {
            proof.signature.clear();
        } else {
            proof.signature = hexToBytes(signature_hex);
        }
        if (!primechain::math::verifyCompositeProof(proof)) {
            error = "proof index contains invalid composite proof";
            return false;
        }
        index.add(proof);
    }
    if (metadata["version"] != "primechain-composite-proof-index-v1") {
        error = "unsupported proof index version";
        return false;
    }
    return true;
}

bool writeProofIndexFile(
    const std::string& path,
    const StoreProofIndex& index,
    const StatusLine& status,
    std::uint64_t prime_records,
    std::uint64_t composite_records,
    std::string& error) {
    if (!ensureDirectory(directoryName(path))) {
        error = "could not create index directory";
        return false;
    }
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            error = "could not write proof index temp file";
            return false;
        }
        out << "version=primechain-composite-proof-index-v1\n";
        out << "chain_frontier=" << status.frontier << "\n";
        out << "chain_height=" << status.height << "\n";
        out << "prime_records=" << prime_records << "\n";
        out << "composite_records=" << composite_records << "\n";
        out << "proof_count=" << index.size() << "\n";
        for (const auto& entry : index.proofs()) {
            const auto& proof = entry.second;
            out << "PROOF " << proof.m << " " << proof.d << " " << proof.e << " "
                << proof.provider_address << " "
                << (proof.signature.empty() ? std::string("-") : bytesToHex(proof.signature)) << "\n";
        }
        if (!out) {
            error = "could not finish proof index temp file";
            return false;
        }
    }
    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        error = std::string("could not install proof index: ") + std::strerror(errno);
        unlink(tmp_path.c_str());
        return false;
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


const char* kindName(primechain::storage::StoredRecordKind kind) {
    switch (kind) {
        case primechain::storage::StoredRecordKind::Composite:
            return "COMPOSITE";
        case primechain::storage::StoredRecordKind::Prime:
            return "PRIME";
    }
    return "UNKNOWN";
}

void printHashField(const char* name, const primechain::Hash256& hash) {
    std::cout << name << ": " << primechain::crypto::toHex(hash) << "\n";
}

void printTransactionSummary(const primechain::protocol::TransactionBatchV0& batch, std::size_t actual_count) {
    std::cout << "transactions: " << actual_count << "\n";
    std::cout << "tx_batch_count: " << batch.transaction_count << "\n";
    printHashField("tx_merkle_root", batch.transaction_merkle_root);
}

void printFinalizationSummary(const primechain::protocol::FinalizationProofV0& proof) {
    std::cout << "finalization_rule: " << proof.rule << "\n";
    std::cout << "finalization_votes: " << proof.votes.size() << "\n";
    std::cout << "round_changes: " << proof.round_changes.size() << "\n";
    if (!proof.votes.empty()) {
        std::cout << "finalization_round: " << proof.votes.front().round << "\n";
    }
}

void printValidatorEpochSummary(const primechain::protocol::ValidatorEpochTransitionV1& transition) {
    if (transition.epoch == 0 && transition.activation_integer == 0 &&
        transition.next_validator_set.empty() && transition.votes.empty()) {
        std::cout << "validator_epoch_transition: none\n";
        return;
    }
    std::cout << "validator_epoch_transition: present\n";
    std::cout << "validator_epoch: " << transition.epoch << "\n";
    std::cout << "activation_integer: " << transition.activation_integer << "\n";
    std::cout << "next_validators: " << transition.next_validator_set.size() << "\n";
    std::cout << "epoch_votes: " << transition.votes.size() << "\n";
}

int decodeRecord(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string store_path = argv[2];
    const primechain::PrimeValue integer = std::stoull(argv[3]);
    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto stored = store.findByInteger(integer, error);
    if (!error.empty()) {
        std::cerr << "record_store_error: " << error << "\n";
        return 1;
    }
    if (!stored.has_value()) {
        std::cerr << "record_not_found: " << integer << "\n";
        return 1;
    }

    std::cout << "DECODE_RECORD " << stored->integer << "\n";
    std::cout << "store_path: " << store_path << "\n";
    std::cout << "kind: " << kindName(stored->kind) << "\n";
    std::cout << "height: " << stored->height << "\n";
    std::cout << "integer: " << stored->integer << "\n";
    printHashField("record_hash", stored->record_hash);
    std::cout << "payload_bytes: " << stored->payload.size() << "\n";

    if (stored->kind == primechain::storage::StoredRecordKind::Composite) {
        const auto record = primechain::protocol::deserializeCompositeRecord(stored->payload, error);
        if (!record.has_value()) {
            std::cerr << "decode_error: " << error << "\n";
            return 1;
        }
        std::cout << "version: " << record->version << "\n";
        printHashField("previous_hash", record->previous_record_hash);
        printHashField("candidate_hash", primechain::protocol::candidateRecordHash(*record));
        std::cout << "provider: " << record->proof.provider_address << "\n";
        std::cout << "composite_integer: " << record->proof.g << "\n";
        std::cout << "divisor: " << record->proof.d << "\n";
        std::cout << "cofactor: " << record->proof.e << "\n";
        std::cout << "proof_signature_bytes: " << record->proof.signature.size() << "\n";
        std::cout << "commit_phase_integer: " << record->commit_phase.integer << "\n";
        std::cout << "commitments: " << record->commit_phase.commitments.size() << "\n";
        std::cout << "commit_phase_votes: " << record->commit_phase.votes.size() << "\n";
        std::cout << "commit_phase_validators: " << record->commit_phase.validator_set.size() << "\n";
        printHashField("state_root", record->state_root);
        printTransactionSummary(record->tx_batch, record->transactions.size());
        printValidatorEpochSummary(record->validator_epoch);
        printFinalizationSummary(record->finalized_by);
        return 0;
    }

    const auto record = primechain::protocol::deserializePrimeRecord(stored->payload, error);
    if (!record.has_value()) {
        std::cerr << "decode_error: " << error << "\n";
        return 1;
    }
    std::cout << "version: " << record->version << "\n";
    printHashField("previous_hash", record->previous_record_hash);
    printHashField("candidate_hash", primechain::protocol::candidateRecordHash(*record));
    std::cout << "provider: " << record->proof.provider_address << "\n";
    std::cout << "prime: " << record->proof.p << "\n";
    std::cout << "pratt_witness: " << record->proof.witness << "\n";
    std::cout << "factors_of_p_minus_1: " << record->proof.factors_of_p_minus_1.size();
    for (const auto& factor : record->proof.factors_of_p_minus_1) {
        std::cout << " " << factor.prime << "^" << factor.exponent;
    }
    std::cout << "\n";
    std::cout << "proof_signature_bytes: " << record->proof.signature.size() << "\n";
    std::cout << "genesis_validators: " << record->genesis_config.validator_set.size() << "\n";
    printHashField("state_root", record->state_root);
    printTransactionSummary(record->tx_batch, record->transactions.size());
    printValidatorEpochSummary(record->validator_epoch);
    printFinalizationSummary(record->finalized_by);
    return 0;
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
std::string indexesDir(const std::string& workdir) { return joinPath(workdir, "indexes"); }
std::string chainPath(const std::string& workdir) { return joinPath(dataDir(workdir), "chain.dat"); }
std::string primeWalletPath(const std::string& workdir) { return joinPath(walletsDir(workdir), "prime.wallet"); }
std::string compositeWalletPath(const std::string& workdir) { return joinPath(walletsDir(workdir), "composite.wallet"); }
std::string mineStatePath(const std::string& workdir) { return joinPath(jobsDir(workdir), "mine.state"); }
std::string pendingCompositePath(const std::string& workdir) { return joinPath(jobsDir(workdir), "pending-composite.state"); }
std::string compositeProofIndexPath(const std::string& workdir) { return joinPath(indexesDir(workdir), "composite-proofs.idx"); }
std::string addressIndexEventsPath(const std::string& workdir) { return joinPath(indexesDir(workdir), "address-index.dat"); }
std::string addressIndexMetaPath(const std::string& workdir) { return joinPath(indexesDir(workdir), "address-index.meta"); }

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

bool writeKeyValueFile(const std::string& path, const std::map<std::string, std::string>& values) {
    if (!ensureDirectory(directoryName(path))) return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::cerr << "could not write " << path << "\n";
        return false;
    }
    for (const auto& entry : values) {
        out << entry.first << "=" << entry.second << "\n";
    }
    return static_cast<bool>(out);
}

std::string nowSeconds() {
    return std::to_string(static_cast<unsigned long long>(std::time(nullptr)));
}

std::optional<primechain::PrimeValue> stateTarget(const std::map<std::string, std::string>& state) {
    const auto found = state.find("target");
    if (found == state.end() || found->second.empty()) return std::nullopt;
    return static_cast<primechain::PrimeValue>(std::stoull(found->second));
}

bool writeMineState(const std::string& workdir, std::map<std::string, std::string> state) {
    state["version"] = "primechain-mine-job-v1";
    return writeKeyValueFile(mineStatePath(workdir), state);
}


bool ensureWorkdirLayout(const std::string& workdir) {
    return ensureDirectory(workdir) && ensureDirectory(dataDir(workdir)) && ensureDirectory(walletsDir(workdir))
        && ensureDirectory(jobsDir(workdir)) && ensureDirectory(logsDir(workdir)) && ensureDirectory(indexesDir(workdir));
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
        std::cerr << "could not query peer status " << peer.host << ":" << peer.port << "\n";
        return std::nullopt;
    }
    auto status = parseStatus(output);
    if (!status.has_value()) {
        std::cerr << "unexpected peer status from " << peer.host << ":" << peer.port << ": " << output;
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

std::optional<primechain::Address> loadMinerAddress(const std::string& path) {
    primechain::Address address;
    std::string error;
    if (!primechain::wallet::loadMinerIdentityAddress(path, address, error)) {
        std::cerr << "could not load miner wallet " << path << ": " << error << "\n";
        return std::nullopt;
    }
    return address;
}

void printHoldings(
    const std::string& label,
    const primechain::Address& address,
    const std::vector<std::pair<primechain::PrimeValue, std::uint64_t>>& holdings) {
    std::uint64_t total_micro_units = 0;
    for (const auto& holding : holdings) total_micro_units += holding.second;
    std::cout << "WALLET " << label << " " << address << " holdings=" << holdings.size()
              << " total_micro_units=" << total_micro_units << "\n";
    for (const auto& holding : holdings) {
        std::cout << "HOLDING " << label << " " << holding.first << " " << holding.second << "\n";
    }
}

int balancesWorkdir(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    const auto prime_address = loadMinerAddress(primeWalletPath(workdir));
    const auto composite_address = loadMinerAddress(compositeWalletPath(workdir));
    if (!prime_address.has_value() || !composite_address.has_value()) return 1;

    primechain::node::SequentialNode node(chainPath(workdir));
    std::string error;
    if (!node.load(error)) {
        std::cerr << "could not load workdir chain: " << error << "\n";
        return 1;
    }

    std::cout << "BALANCES " << workdir << "\n";
    printHoldings("prime", *prime_address, node.holdingsForAddress(*prime_address));
    printHoldings("composite", *composite_address, node.holdingsForAddress(*composite_address));
    return 0;
}

std::uint64_t primeMinerRewardMicroUnits(
    bool validator_rewards_active,
    bool has_pending_composites,
    std::uint64_t composite_remainder) {
    if (!validator_rewards_active) {
        if (!has_pending_composites) return primechain::node::kAssetMicroUnits;
        return (primechain::node::kAssetMicroUnits / 2) + composite_remainder;
    }
    if (!has_pending_composites) {
        return primechain::node::kPrimeDiscoveryRewardMicroUnits +
               primechain::node::kCompositeDiscoveryRewardMicroUnits;
    }
    return primechain::node::kPrimeDiscoveryRewardMicroUnits + composite_remainder;
}

std::uint64_t compositeRewardPoolMicroUnits(bool validator_rewards_active) {
    return validator_rewards_active
        ? primechain::node::kCompositeDiscoveryRewardMicroUnits
        : primechain::node::kAssetMicroUnits - (primechain::node::kAssetMicroUnits / 2);
}

struct RewardSummary {
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    std::uint64_t prime_micro_units{0};
    std::uint64_t composite_micro_units{0};
    std::uint64_t fee_micro_units{0};
};

std::uint64_t transactionFees(const std::vector<primechain::protocol::TransactionV0>& transactions) {
    std::uint64_t total = 0;
    for (const auto& tx : transactions) {
        if (tx.fee.amount.denominator == 1) total += tx.fee.amount.numerator;
    }
    return total;
}

std::optional<std::vector<primechain::protocol::TransactionV0>> storedTransactions(
    const primechain::storage::StoredRecord& stored,
    std::string& error) {
    if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
        const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!record.has_value()) return std::nullopt;
        return record->transactions;
    }

    const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
    if (!record.has_value()) return std::nullopt;
    return record->transactions;
}

bool hasAmount(const primechain::protocol::Amount& amount) {
    return amount.numerator != 0;
}

void appendWalletTransactionEvent(
    std::vector<std::string>& events,
    const primechain::storage::StoredRecord& stored,
    const primechain::protocol::TransactionV0& tx,
    const std::string& direction,
    std::uint64_t confirmations,
    primechain::PrimeValue prime,
    const primechain::protocol::Amount& amount,
    const primechain::Address& sender,
    const primechain::Address& receiver) {
    std::ostringstream event;
    event << "TX_EVENT"
          << " integer=" << stored.integer
          << " height=" << stored.height
          << " kind=" << kindName(stored.kind)
          << " confirmations=" << confirmations
          << " direction=" << direction
          << " tx_hash=" << primechain::crypto::toHex(primechain::protocol::transactionHash(tx))
          << " version=" << tx.version
          << " nonce=" << tx.nonce
          << " prime=" << prime
          << " amount_micro_units=" << amount.numerator
          << " amount_denominator=" << amount.denominator
          << " sender=" << sender
          << " receiver=" << receiver;
    events.push_back(event.str());
}

// Secondary, derived address index: one EVENT line per (address, tx-side)
// pair, covering the same three cases wallet-history already computes
// (sent/received/self/fee-paid), extracted for every address a transaction
// touches rather than filtered to a single wallet. This lets a single
// incremental pass over new records serve lookups for any address, instead
// of every command re-decoding and re-verifying the whole chain from
// record 0. Deliberately a cache over data client.cpp already computes
// today (via storedTransactions/deserializePrimeRecord/
// deserializeCompositeRecord) -- it adds no new protocol logic and is
// fully disposable: delete indexes/address-index.* and the next
// update-address-index rebuilds it from the canonical record store.
struct AddressIndexEvent {
    primechain::Address address;
    primechain::PrimeValue integer{0};
    std::uint64_t height{0};
    std::string kind;
    std::string direction;
    std::string tx_hash;
    std::uint64_t version{0};
    std::uint64_t nonce{0};
    primechain::PrimeValue prime{0};
    std::uint64_t amount_micro_units{0};
    std::uint64_t amount_denominator{1};
    primechain::Address sender;
    primechain::Address receiver;
};

std::string formatAddressIndexEventLine(const AddressIndexEvent& event) {
    std::ostringstream out;
    out << "EVENT address=" << event.address
        << " integer=" << event.integer
        << " height=" << event.height
        << " kind=" << event.kind
        << " direction=" << event.direction
        << " tx_hash=" << event.tx_hash
        << " version=" << event.version
        << " nonce=" << event.nonce
        << " prime=" << event.prime
        << " amount_micro_units=" << event.amount_micro_units
        << " amount_denominator=" << event.amount_denominator
        << " sender=" << event.sender
        << " receiver=" << event.receiver;
    return out.str();
}

std::optional<AddressIndexEvent> parseAddressIndexEventLine(const std::string& line) {
    if (line.rfind("EVENT ", 0) != 0) return std::nullopt;
    std::istringstream in(line);
    std::string tag;
    in >> tag;
    AddressIndexEvent event;
    std::string token;
    try {
        while (in >> token) {
            const auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            const auto key = token.substr(0, eq);
            const auto value = token.substr(eq + 1);
            if (key == "address") event.address = value;
            else if (key == "integer") event.integer = std::stoull(value);
            else if (key == "height") event.height = std::stoull(value);
            else if (key == "kind") event.kind = value;
            else if (key == "direction") event.direction = value;
            else if (key == "tx_hash") event.tx_hash = value;
            else if (key == "version") event.version = std::stoull(value);
            else if (key == "nonce") event.nonce = std::stoull(value);
            else if (key == "prime") event.prime = std::stoull(value);
            else if (key == "amount_micro_units") event.amount_micro_units = std::stoull(value);
            else if (key == "amount_denominator") event.amount_denominator = std::stoull(value);
            else if (key == "sender") event.sender = value;
            else if (key == "receiver") event.receiver = value;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (event.address.empty() || event.kind.empty() || event.direction.empty() ||
        event.tx_hash.empty() || event.sender.empty() || event.receiver.empty()) {
        return std::nullopt;
    }
    return event;
}

void extractAddressIndexEvents(
    const primechain::storage::StoredRecord& stored,
    const std::vector<primechain::protocol::TransactionV0>& transactions,
    std::vector<AddressIndexEvent>& out) {
    for (const auto& tx : transactions) {
        const auto tx_hash_hex = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
        for (const auto& output : tx.outputs) {
            const bool self = tx.sender_address == output.receiver_address;
            AddressIndexEvent base;
            base.integer = stored.integer;
            base.height = stored.height;
            base.kind = kindName(stored.kind);
            base.tx_hash = tx_hash_hex;
            base.version = tx.version;
            base.nonce = tx.nonce;
            base.prime = output.prime;
            base.amount_micro_units = output.amount.numerator;
            base.amount_denominator = output.amount.denominator;
            base.sender = tx.sender_address;
            base.receiver = output.receiver_address;

            AddressIndexEvent sender_event = base;
            sender_event.address = tx.sender_address;
            sender_event.direction = self ? "self" : "sent";
            out.push_back(sender_event);

            if (!self) {
                AddressIndexEvent receiver_event = base;
                receiver_event.address = output.receiver_address;
                receiver_event.direction = "received";
                out.push_back(receiver_event);
            }
        }
        if (hasAmount(tx.fee.amount)) {
            AddressIndexEvent fee_event;
            fee_event.address = tx.sender_address;
            fee_event.integer = stored.integer;
            fee_event.height = stored.height;
            fee_event.kind = kindName(stored.kind);
            fee_event.direction = "fee-paid";
            fee_event.tx_hash = tx_hash_hex;
            fee_event.version = tx.version;
            fee_event.nonce = tx.nonce;
            fee_event.prime = tx.fee.prime;
            fee_event.amount_micro_units = tx.fee.amount.numerator;
            fee_event.amount_denominator = tx.fee.amount.denominator;
            fee_event.sender = tx.sender_address;
            fee_event.receiver = "validator-fee-pool";
            out.push_back(fee_event);
        }
    }
}

std::vector<primechain::protocol::TransactionV0> parseMempoolTransactions(
    const std::string& mempool_text,
    std::uint64_t& mempool_count,
    std::string& error) {
    std::istringstream in(mempool_text);
    std::string line;
    std::vector<primechain::protocol::TransactionV0> transactions;

    if (!std::getline(in, line)) {
        error = "empty mempool response";
        return {};
    }
    {
        std::istringstream header(line);
        std::string tag;
        header >> tag >> mempool_count;
        if (!header || tag != "MEMPOOL") {
            error = "unexpected mempool header: " + line;
            return {};
        }
    }

    while (std::getline(in, line)) {
        if (line == "END_MEMPOOL") break;
        std::istringstream tx_line(line);
        std::string tag;
        std::string tx_hash;
        std::uint64_t byte_count = 0;
        std::string tx_hex;
        tx_line >> tag >> tx_hash >> byte_count >> tx_hex;
        if (!tx_line || tag != "TX") {
            error = "unexpected mempool transaction line: " + line;
            return {};
        }
        const auto bytes = hexToBytes(tx_hex);
        if (bytes.size() != byte_count) {
            error = "mempool transaction byte count mismatch";
            return {};
        }
        auto tx = primechain::protocol::deserializeTransaction(bytes, error);
        if (!tx.has_value()) return {};
        transactions.push_back(*tx);
    }
    return transactions;
}

int walletPending(const char* argv0, int argc, char** argv) {
    if (argc != 5) return 1;
    const std::string host = argv[2];
    const std::string port = argv[3];
    const std::string wallet_path = argv[4];
    const auto address = loadMinerAddress(wallet_path);
    if (!address.has_value()) return 1;

    std::string mempool_text;
    if (!captureTool(argv0, "primechain-sync-query", {host, port, "GET_MEMPOOL"}, mempool_text)) {
        std::cerr << "wallet_pending_error: could not query mempool\n";
        return 1;
    }

    std::uint64_t mempool_count = 0;
    std::string error;
    const auto transactions = parseMempoolTransactions(mempool_text, mempool_count, error);
    if (!error.empty()) {
        std::cerr << "wallet_pending_error: " << error << "\n";
        return 1;
    }

    std::vector<std::string> events;
    std::set<std::string> matching_hashes;
    for (const auto& tx : transactions) {
        const auto tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
        const bool from_wallet = tx.sender_address == *address;
        for (const auto& output : tx.outputs) {
            const bool to_wallet = output.receiver_address == *address;
            if (!from_wallet && !to_wallet) continue;
            matching_hashes.insert(tx_hash);
            std::ostringstream event;
            event << "PENDING_TX"
                  << " direction=" << (from_wallet && to_wallet ? "self" : (from_wallet ? "sent" : "received"))
                  << " tx_hash=" << tx_hash
                  << " version=" << tx.version
                  << " nonce=" << tx.nonce
                  << " prime=" << output.prime
                  << " amount_micro_units=" << output.amount.numerator
                  << " amount_denominator=" << output.amount.denominator
                  << " sender=" << tx.sender_address
                  << " receiver=" << output.receiver_address;
            events.push_back(event.str());
        }
        if (from_wallet && hasAmount(tx.fee.amount)) {
            matching_hashes.insert(tx_hash);
            std::ostringstream event;
            event << "PENDING_TX"
                  << " direction=fee-paid"
                  << " tx_hash=" << tx_hash
                  << " version=" << tx.version
                  << " nonce=" << tx.nonce
                  << " prime=" << tx.fee.prime
                  << " amount_micro_units=" << tx.fee.amount.numerator
                  << " amount_denominator=" << tx.fee.amount.denominator
                  << " sender=" << tx.sender_address
                  << " receiver=validator-fee-pool";
            events.push_back(event.str());
        }
    }

    std::cout << "WALLET_PENDING " << host << ":" << port
              << " wallet=" << wallet_path
              << " address=" << *address
              << " mempool=" << mempool_count
              << " transactions=" << matching_hashes.size()
              << " events=" << events.size() << "\n";
    for (const auto& event : events) std::cout << event << "\n";
    return 0;
}

void printTransactionDetails(
    const primechain::protocol::TransactionV0& tx,
    const std::string& prefix) {
    std::cout << prefix << "_INPUTS count=" << tx.inputs.size() << "\n";
    for (const auto& input : tx.inputs) {
        std::cout << prefix << "_INPUT"
                  << " prime=" << input.prime
                  << " amount_micro_units=" << input.amount.numerator
                  << " amount_denominator=" << input.amount.denominator << "\n";
    }
    std::cout << prefix << "_OUTPUTS count=" << tx.outputs.size() << "\n";
    for (const auto& output : tx.outputs) {
        std::cout << prefix << "_OUTPUT"
                  << " prime=" << output.prime
                  << " amount_micro_units=" << output.amount.numerator
                  << " amount_denominator=" << output.amount.denominator
                  << " receiver=" << output.receiver_address << "\n";
    }
    std::cout << prefix << "_FEE"
              << " prime=" << tx.fee.prime
              << " amount_micro_units=" << tx.fee.amount.numerator
              << " amount_denominator=" << tx.fee.amount.denominator << "\n";
}

int transactionLookup(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string store_path = argv[2];
    const std::string wanted_hash = argv[3];

    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "tx_lookup_error: " << error << "\n";
        return 1;
    }

    primechain::PrimeValue frontier = 0;
    if (!records.empty()) frontier = records.back().integer;

    for (const auto& stored : records) {
        const auto transactions = storedTransactions(stored, error);
        if (!transactions.has_value()) {
            std::cerr << "tx_lookup_error: " << error << "\n";
            return 1;
        }
        for (const auto& tx : *transactions) {
            const auto tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
            if (tx_hash != wanted_hash) continue;

            const auto confirmations = frontier >= stored.integer ? frontier - stored.integer + 1 : 0;
            std::cout << "TX_FOUND " << wanted_hash
                      << " store=" << store_path
                      << " integer=" << stored.integer
                      << " height=" << stored.height
                      << " kind=" << kindName(stored.kind)
                      << " frontier=" << frontier
                      << " confirmations=" << confirmations
                      << " version=" << tx.version
                      << " nonce=" << tx.nonce
                      << " sender=" << tx.sender_address << "\n";
            printTransactionDetails(tx, "TX");
            return 0;
        }
    }

    std::cout << "TX_NOT_FOUND " << wanted_hash
              << " store=" << store_path
              << " frontier=" << frontier << "\n";
    return 1;
}


bool hasValidatorEpochTransition(const primechain::protocol::ValidatorEpochTransitionV1& transition) {
    return transition.epoch != 0 || transition.activation_integer != 0 ||
           !transition.next_validator_set.empty() || !transition.votes.empty();
}

bool hasEconomicPolicyUpdate(const primechain::protocol::EconomicPolicyUpdateV1& policy) {
    return policy.transfer_fee_micro_units != 0 ||
           policy.validator_min_reserve_micro_units != 0 ||
           policy.effective_integer != 0 || policy.sequence != 0 || !policy.votes.empty();
}

void printExplorerTransactions(const std::vector<primechain::protocol::TransactionV0>& transactions) {
    for (const auto& tx : transactions) {
        const auto tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
        std::cout << "RECORD_TX"
                  << " tx_hash=" << tx_hash
                  << " version=" << tx.version
                  << " nonce=" << tx.nonce
                  << " sender=" << tx.sender_address
                  << " inputs=" << tx.inputs.size()
                  << " outputs=" << tx.outputs.size()
                  << " fee_prime=" << tx.fee.prime
                  << " fee_micro_units=" << tx.fee.amount.numerator
                  << " fee_denominator=" << tx.fee.amount.denominator << "\n";
    }
}

void printExplorerCommonMetadata(
    const primechain::protocol::ValidatorEpochTransitionV1& validator_epoch,
    const std::vector<primechain::protocol::ValidatorEndpointUpdateV1>& validator_endpoints,
    const primechain::protocol::EconomicPolicyUpdateV1& economic_policy,
    const std::vector<primechain::protocol::ValidatorApplicationV1>& validator_applications,
    const std::vector<primechain::protocol::ValidatorWorkBindingV1>& validator_work_bindings) {
    if (hasValidatorEpochTransition(validator_epoch)) {
        std::cout << "VALIDATOR_EPOCH_TRANSITION"
                  << " epoch=" << validator_epoch.epoch
                  << " activation_integer=" << validator_epoch.activation_integer
                  << " validators=" << validator_epoch.next_validator_set.size()
                  << " votes=" << validator_epoch.votes.size() << "\n";
    }
    if (!validator_endpoints.empty()) {
        std::cout << "VALIDATOR_ENDPOINT_UPDATES count=" << validator_endpoints.size() << "\n";
    }
    if (hasEconomicPolicyUpdate(economic_policy)) {
        std::cout << "ECONOMIC_POLICY_UPDATE"
                  << " transfer_fee_micro_units=" << economic_policy.transfer_fee_micro_units
                  << " validator_min_reserve_micro_units=" << economic_policy.validator_min_reserve_micro_units
                  << " effective_integer=" << economic_policy.effective_integer
                  << " sequence=" << economic_policy.sequence
                  << " votes=" << economic_policy.votes.size() << "\n";
    }
    if (!validator_applications.empty()) {
        std::cout << "VALIDATOR_APPLICATIONS count=" << validator_applications.size() << "\n";
    }
    if (!validator_work_bindings.empty()) {
        std::cout << "VALIDATOR_WORK_BINDINGS count=" << validator_work_bindings.size() << "\n";
    }
}

bool printExplorerRecord(
    const primechain::storage::StoredRecord& stored,
    primechain::PrimeValue frontier,
    bool include_details,
    std::string& error) {
    const auto confirmations = frontier >= stored.integer ? frontier - stored.integer + 1 : 0;
    if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
        const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) return false;
        std::cout << "RECORD"
                  << " integer=" << stored.integer
                  << " height=" << stored.height
                  << " kind=" << kindName(stored.kind)
                  << " hash=" << primechain::crypto::toHex(stored.record_hash)
                  << " frontier=" << frontier
                  << " confirmations=" << confirmations
                  << " provider=" << record->proof.provider_address
                  << " txs=" << record->transactions.size()
                  << " finalization_votes=" << record->finalized_by.votes.size()
                  << " commit_phase_votes=" << record->commit_phase.votes.size()
                  << " round_changes=" << record->finalized_by.round_changes.size() << "\n";
        if (include_details) {
            std::cout << "COMPOSITE_PROOF"
                      << " integer=" << record->proof.g
                      << " divisor=" << record->proof.d
                      << " cofactor=" << record->proof.e << "\n";
            std::cout << "COMMIT_PHASE"
                      << " integer=" << record->commit_phase.integer
                      << " commitments=" << record->commit_phase.commitments.size()
                      << " votes=" << record->commit_phase.votes.size()
                      << " validators=" << record->commit_phase.validator_set.size() << "\n";
            printExplorerTransactions(record->transactions);
            printExplorerCommonMetadata(
                record->validator_epoch,
                record->validator_endpoints,
                record->economic_policy,
                record->validator_applications,
                record->validator_work_bindings);
        }
        return true;
    }

    const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
    if (!record.has_value()) return false;
    std::cout << "RECORD"
              << " integer=" << stored.integer
              << " height=" << stored.height
              << " kind=" << kindName(stored.kind)
              << " hash=" << primechain::crypto::toHex(stored.record_hash)
              << " frontier=" << frontier
              << " confirmations=" << confirmations
              << " provider=" << record->proof.provider_address
              << " txs=" << record->transactions.size()
              << " finalization_votes=" << record->finalized_by.votes.size()
              << " commit_phase_votes=0"
              << " round_changes=" << record->finalized_by.round_changes.size() << "\n";
    if (include_details) {
        std::cout << "PRIME_PROOF"
                  << " prime=" << record->proof.p
                  << " witness=" << record->proof.witness
                  << " factors=" << record->proof.factors_of_p_minus_1.size() << "\n";
        if (!record->genesis_config.validator_set.empty()) {
            std::cout << "GENESIS_CONFIG validators=" << record->genesis_config.validator_set.size() << "\n";
        }
        printExplorerTransactions(record->transactions);
        printExplorerCommonMetadata(
            record->validator_epoch,
            record->validator_endpoints,
            record->economic_policy,
            record->validator_applications,
            record->validator_work_bindings);
    }
    return true;
}

int recordExplorerLookup(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string store_path = argv[2];
    const primechain::PrimeValue integer = std::stoull(argv[3]);

    // Single-record lookup: findByInteger seeks straight to it via the
    // existing .idx offset index instead of decoding the whole chain just
    // to find one entry.
    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto latest = store.latest(error);
    if (!error.empty()) {
        std::cerr << "record_error: " << error << "\n";
        return 1;
    }
    const auto frontier = latest.has_value() ? latest->integer : primechain::PrimeValue{0};

    const auto stored = store.findByInteger(integer, error);
    if (!error.empty()) {
        std::cerr << "record_error: " << error << "\n";
        return 1;
    }
    if (stored.has_value()) {
        if (!printExplorerRecord(*stored, frontier, true, error)) {
            std::cerr << "record_error: " << error << "\n";
            return 1;
        }
        return 0;
    }

    std::cout << "RECORD_NOT_FOUND " << store_path
              << " integer=" << integer
              << " frontier=" << frontier << "\n";
    return 1;
}

int latestRecordsExplorer(int argc, char** argv) {
    if (argc != 3 && argc != 5) return 1;
    const std::string store_path = argv[2];
    std::uint64_t last = 20;
    if (argc == 5) {
        if (std::string(argv[3]) != "--last") return 1;
        last = static_cast<std::uint64_t>(std::stoull(argv[4]));
    }

    // Every integer from 2 (the first prime) to frontier has exactly one
    // record, so total record count is derivable from the frontier alone
    // without loading anything, and the tail we actually want to print can
    // be fetched with one findRange() seek instead of decoding the entire
    // chain to throw away everything but the last `last` records.
    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto latest = store.latest(error);
    if (!error.empty()) {
        std::cerr << "latest_records_error: " << error << "\n";
        return 1;
    }
    const auto frontier = latest.has_value() ? latest->integer : primechain::PrimeValue{0};
    const std::uint64_t total_records = frontier >= 2 ? static_cast<std::uint64_t>(frontier - 1) : 0;
    const auto showing = last >= total_records ? total_records : last;

    std::vector<primechain::storage::StoredRecord> records;
    if (showing > 0) {
        const primechain::PrimeValue range_start = frontier - static_cast<primechain::PrimeValue>(showing) + 1;
        records = store.findRange(range_start, frontier, error);
        if (!error.empty()) {
            std::cerr << "latest_records_error: " << error << "\n";
            return 1;
        }
    }

    std::cout << "LATEST_RECORDS " << store_path
              << " frontier=" << frontier
              << " records=" << total_records
              << " showing=" << showing << "\n";
    for (const auto& stored : records) {
        if (!printExplorerRecord(stored, frontier, false, error)) {
            std::cerr << "latest_records_error: " << error << "\n";
            return 1;
        }
    }
    return 0;
}

struct AddressReportTotals {
    std::uint64_t sent_micro_units{0};
    std::uint64_t received_micro_units{0};
    std::uint64_t fee_micro_units{0};
    std::uint64_t transactions{0};
};

void appendAddressEvents(
    std::vector<std::string>& events,
    std::set<std::string>& matching_hashes,
    AddressReportTotals& totals,
    const primechain::storage::StoredRecord& stored,
    primechain::PrimeValue frontier,
    const primechain::protocol::TransactionV0& tx,
    const primechain::Address& address,
    const std::string& prefix) {
    const auto tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
    const bool from_address = tx.sender_address == address;
    const auto confirmations = frontier >= stored.integer ? frontier - stored.integer + 1 : 0;
    for (const auto& output : tx.outputs) {
        const bool to_address = output.receiver_address == address;
        if (!from_address && !to_address) continue;
        matching_hashes.insert(tx_hash);
        const auto direction = from_address && to_address
            ? std::string("self")
            : (from_address ? std::string("sent") : std::string("received"));
        if (output.amount.denominator == 1) {
            if (direction == "sent") totals.sent_micro_units += output.amount.numerator;
            if (direction == "received") totals.received_micro_units += output.amount.numerator;
        }
        std::ostringstream event;
        event << prefix
              << " integer=" << stored.integer
              << " height=" << stored.height
              << " kind=" << kindName(stored.kind)
              << " confirmations=" << confirmations
              << " direction=" << direction
              << " tx_hash=" << tx_hash
              << " version=" << tx.version
              << " nonce=" << tx.nonce
              << " prime=" << output.prime
              << " amount_micro_units=" << output.amount.numerator
              << " amount_denominator=" << output.amount.denominator
              << " sender=" << tx.sender_address
              << " receiver=" << output.receiver_address;
        events.push_back(event.str());
    }
    if (from_address && hasAmount(tx.fee.amount)) {
        matching_hashes.insert(tx_hash);
        if (tx.fee.amount.denominator == 1) totals.fee_micro_units += tx.fee.amount.numerator;
        std::ostringstream event;
        event << prefix
              << " integer=" << stored.integer
              << " height=" << stored.height
              << " kind=" << kindName(stored.kind)
              << " confirmations=" << confirmations
              << " direction=fee-paid"
              << " tx_hash=" << tx_hash
              << " version=" << tx.version
              << " nonce=" << tx.nonce
              << " prime=" << tx.fee.prime
              << " amount_micro_units=" << tx.fee.amount.numerator
              << " amount_denominator=" << tx.fee.amount.denominator
              << " sender=" << tx.sender_address
              << " receiver=validator-fee-pool";
        events.push_back(event.str());
    }
}

int addressReport(int argc, char** argv) {
    if (argc != 4 && argc != 6) return 1;
    const std::string store_path = argv[2];
    const primechain::Address address = argv[3];
    std::uint64_t last = 0;
    if (argc == 6) {
        if (std::string(argv[4]) != "--last") return 1;
        last = static_cast<std::uint64_t>(std::stoull(argv[5]));
    }

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "address_report_error: " << error << "\n";
        return 1;
    }
    const auto status = node.status();
    const auto frontier = status.has_genesis ? status.frontier_integer : primechain::PrimeValue{0};

    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "address_report_error: " << error << "\n";
        return 1;
    }

    std::vector<std::string> events;
    std::set<std::string> matching_hashes;
    AddressReportTotals totals;
    for (const auto& stored : records) {
        const auto transactions = storedTransactions(stored, error);
        if (!transactions.has_value()) {
            std::cerr << "address_report_error: " << error << "\n";
            return 1;
        }
        for (const auto& tx : *transactions) {
            appendAddressEvents(events, matching_hashes, totals, stored, frontier, tx, address, "ADDRESS_TX");
        }
    }
    totals.transactions = matching_hashes.size();

    const auto holdings = node.holdingsForAddress(address);
    std::uint64_t balance = 0;
    for (const auto& holding : holdings) balance += holding.second;

    std::cout << "ADDRESS_REPORT " << store_path
              << " address=" << address
              << " frontier=" << frontier
              << " holdings=" << holdings.size()
              << " total_micro_units=" << balance
              << " transactions=" << totals.transactions
              << " events=" << events.size()
              << " sent_micro_units=" << totals.sent_micro_units
              << " received_micro_units=" << totals.received_micro_units
              << " fee_micro_units=" << totals.fee_micro_units << "\n";
    for (const auto& holding : holdings) {
        std::cout << "ADDRESS_HOLDING address=" << address
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }

    const auto start = last == 0 || last >= events.size()
        ? std::size_t{0}
        : events.size() - static_cast<std::size_t>(last);
    for (std::size_t i = start; i < events.size(); ++i) {
        std::cout << events[i] << "\n";
    }
    return 0;
}

int walletHistory(int argc, char** argv) {
    if (argc != 4 && argc != 6) return 1;
    const std::string store_path = argv[2];
    const std::string wallet_path = argv[3];
    std::uint64_t last = 0;
    if (argc == 6) {
        if (std::string(argv[4]) != "--last") return 1;
        last = static_cast<std::uint64_t>(std::stoull(argv[5]));
    }

    const auto address = loadMinerAddress(wallet_path);
    if (!address.has_value()) return 1;

    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "wallet_history_error: " << error << "\n";
        return 1;
    }

    const auto frontier = records.empty() ? primechain::PrimeValue{0} : records.back().integer;
    std::vector<std::string> events;
    for (const auto& stored : records) {
        const auto transactions = storedTransactions(stored, error);
        if (!transactions.has_value()) {
            std::cerr << "wallet_history_error: " << error << "\n";
            return 1;
        }

        for (const auto& tx : *transactions) {
            const bool from_wallet = tx.sender_address == *address;
            for (const auto& output : tx.outputs) {
                const bool to_wallet = output.receiver_address == *address;
                if (!from_wallet && !to_wallet) continue;
                const auto direction = from_wallet && to_wallet
                    ? std::string("self")
                    : (from_wallet ? std::string("sent") : std::string("received"));
                appendWalletTransactionEvent(
                    events,
                    stored,
                    tx,
                    direction,
                    frontier >= stored.integer ? frontier - stored.integer + 1 : 0,
                    output.prime,
                    output.amount,
                    tx.sender_address,
                    output.receiver_address);
            }

            if (from_wallet && hasAmount(tx.fee.amount)) {
                appendWalletTransactionEvent(
                    events,
                    stored,
                    tx,
                    "fee-paid",
                    frontier >= stored.integer ? frontier - stored.integer + 1 : 0,
                    tx.fee.prime,
                    tx.fee.amount,
                    tx.sender_address,
                    "validator-fee-pool");
            }
        }
    }

    std::cout << "WALLET_HISTORY " << store_path
              << " wallet=" << wallet_path
              << " address=" << *address
              << " events=" << events.size() << "\n";
    const auto start = last == 0 || last >= events.size()
        ? std::size_t{0}
        : events.size() - static_cast<std::size_t>(last);
    for (std::size_t i = start; i < events.size(); ++i) {
        std::cout << events[i] << "\n";
    }
    return 0;
}

int rewardsWorkdir(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    const auto prime_address = loadMinerAddress(primeWalletPath(workdir));
    const auto composite_address = loadMinerAddress(compositeWalletPath(workdir));
    if (!prime_address.has_value() || !composite_address.has_value()) return 1;

    primechain::storage::RecordStore store(chainPath(workdir));
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "could not load workdir chain: " << error << "\n";
        return 1;
    }

    RewardSummary summary;
    std::vector<primechain::Address> pending_composite_providers;
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "could not decode composite record: " << error << "\n";
                return 1;
            }
            if (record->proof.provider_address == *composite_address) ++summary.composite_records;
            pending_composite_providers.push_back(record->proof.provider_address);
            continue;
        }

        const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!record.has_value()) {
            std::cerr << "could not decode prime record: " << error << "\n";
            return 1;
        }
        if (record->proof.provider_address == *prime_address) ++summary.prime_records;

        const bool validator_rewards_active = record->version != 0 && record->height != 0;
        if (pending_composite_providers.empty()) {
            if (record->proof.provider_address == *prime_address) {
                summary.prime_micro_units += primeMinerRewardMicroUnits(
                    validator_rewards_active, false, 0);
            }
        } else {
            const std::uint64_t composite_pool =
                compositeRewardPoolMicroUnits(validator_rewards_active);
            const std::uint64_t per_composite = composite_pool / pending_composite_providers.size();
            const std::uint64_t remainder = composite_pool % pending_composite_providers.size();
            if (record->proof.provider_address == *prime_address) {
                summary.prime_micro_units += primeMinerRewardMicroUnits(
                    validator_rewards_active, true, remainder);
            }
            for (const auto& provider : pending_composite_providers) {
                if (provider == *composite_address) summary.composite_micro_units += per_composite;
            }
        }
        pending_composite_providers.clear();
    }

    std::cout << "REWARDS " << workdir << "\n";
    std::cout << "PRIME_WALLET " << *prime_address << " records=" << summary.prime_records
              << " reward_micro_units=" << summary.prime_micro_units << "\n";
    std::cout << "COMPOSITE_WALLET " << *composite_address << " records=" << summary.composite_records
              << " reward_micro_units=" << summary.composite_micro_units << "\n";
    std::cout << "FEE_REWARDS micro_units=" << summary.fee_micro_units << "\n";
    std::cout << "PENDING_COMPOSITE_RECORDS " << pending_composite_providers.size() << "\n";
    return 0;
}

struct PendingCompositeReward {
    primechain::Address provider;
    primechain::PrimeValue source_integer{0};
};

int rewardHistoryWorkdir(int argc, char** argv) {
    if (argc != 3 && argc != 5) return 1;
    const std::string workdir = argv[2];
    std::optional<std::size_t> last;
    if (argc == 5) {
        if (std::string(argv[3]) != "--last") return 1;
        last = static_cast<std::size_t>(std::stoull(argv[4]));
    }

    const auto prime_address = loadMinerAddress(primeWalletPath(workdir));
    const auto composite_address = loadMinerAddress(compositeWalletPath(workdir));
    if (!prime_address.has_value() || !composite_address.has_value()) return 1;

    primechain::storage::RecordStore store(chainPath(workdir));
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "could not load workdir chain: " << error << "\n";
        return 1;
    }

    std::vector<PendingCompositeReward> pending;
    std::vector<std::string> events;
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "could not decode composite record: " << error << "\n";
                return 1;
            }
            const auto fees = transactionFees(record->transactions);
            if (fees != 0 && record->proof.provider_address == *composite_address) {
                std::ostringstream event;
                event << "REWARD fee integer=" << record->integer << " amount=" << fees
                      << " role=record-provider record_height=" << record->height;
                events.push_back(event.str());
            }
            pending.push_back({record->proof.provider_address, record->integer});
            continue;
        }

        const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!record.has_value()) {
            std::cerr << "could not decode prime record: " << error << "\n";
            return 1;
        }
        const auto fees = transactionFees(record->transactions);
        if (fees != 0 && record->proof.provider_address == *prime_address) {
            std::ostringstream event;
            event << "REWARD fee integer=" << record->integer << " amount=" << fees
                  << " role=record-provider record_height=" << record->height;
            events.push_back(event.str());
        }
        const bool validator_rewards_active = record->version != 0 && record->height != 0;
        if (pending.empty()) {
            if (record->proof.provider_address == *prime_address) {
                std::ostringstream event;
                event << "REWARD prime integer=" << record->integer
                      << " amount=" << primeMinerRewardMicroUnits(
                             validator_rewards_active, false, 0)
                      << " role=prime-miner record_height=" << record->height;
                events.push_back(event.str());
            }
        } else {
            const std::uint64_t composite_pool =
                compositeRewardPoolMicroUnits(validator_rewards_active);
            const std::uint64_t per_composite = composite_pool / pending.size();
            const std::uint64_t remainder = composite_pool % pending.size();
            if (record->proof.provider_address == *prime_address) {
                std::ostringstream event;
                event << "REWARD prime integer=" << record->integer
                      << " amount=" << primeMinerRewardMicroUnits(
                             validator_rewards_active, true, remainder)
                      << " role=prime-miner record_height=" << record->height;
                events.push_back(event.str());
            }
            for (const auto& provider : pending) {
                if (provider.provider == *composite_address) {
                    std::ostringstream event;
                    event << "REWARD composite integer=" << record->integer
                          << " amount=" << per_composite
                          << " role=composite-provider source=" << provider.source_integer
                          << " record_height=" << record->height;
                    events.push_back(event.str());
                }
            }
        }
        pending.clear();
    }

    std::size_t start = 0;
    if (last.has_value() && *last < events.size()) start = events.size() - *last;
    std::cout << "REWARD_HISTORY " << workdir << " events=" << (events.size() - start) << "\n";
    for (std::size_t i = start; i < events.size(); ++i) {
        std::cout << events[i] << "\n";
    }
    std::cout << "PENDING_COMPOSITE_RECORDS " << pending.size() << "\n";
    return 0;
}


struct AddressBoardStats {
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    std::uint64_t discovery_micro_units{0};
    std::uint64_t fee_micro_units{0};
};

struct ValidatorBoardStats {
    std::uint64_t finalization_votes{0};
    std::uint64_t round_change_votes{0};
    std::uint64_t commit_phase_votes{0};
};

struct BoardReportStats {
    primechain::PrimeValue from{0};
    primechain::PrimeValue to{0};
    std::uint64_t records{0};
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    std::uint64_t transaction_count{0};
    std::uint64_t fee_micro_units{0};
    std::uint64_t pending_composites_after_range{0};
    std::map<primechain::Address, AddressBoardStats> miners;
    std::map<primechain::Address, ValidatorBoardStats> validators;
};

bool inRange(primechain::PrimeValue n, primechain::PrimeValue from, primechain::PrimeValue to) {
    return n >= from && n <= to;
}

void addDiscoveryReward(
    BoardReportStats& stats,
    const primechain::Address& address,
    std::uint64_t micro_units) {
    stats.miners[address].discovery_micro_units += micro_units;
}

void addFeeReward(
    BoardReportStats& stats,
    const primechain::Address& /*address*/,
    std::uint64_t micro_units) {
    stats.fee_micro_units += micro_units;
}

void addValidatorEvidence(
    BoardReportStats& stats,
    const primechain::protocol::CommitPhaseCertificateV1& phase,
    const primechain::protocol::FinalizationProofV0& finalization) {
    for (const auto& vote : phase.votes) {
        ++stats.validators[vote.validator_address].commit_phase_votes;
    }
    for (const auto& vote : finalization.votes) {
        ++stats.validators[vote.validator_address].finalization_votes;
    }
    for (const auto& vote : finalization.round_changes) {
        ++stats.validators[vote.validator_address].round_change_votes;
    }
}

bool collectBoardReportStats(
    const std::string& store_path,
    primechain::PrimeValue from,
    primechain::PrimeValue to,
    BoardReportStats& stats,
    std::string& error) {
    if (from < 2 || to < from) {
        error = "invalid report range";
        return false;
    }

    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) return false;

    stats = BoardReportStats{};
    stats.from = from;
    stats.to = to;

    std::vector<PendingCompositeReward> pending;
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) return false;
            if (inRange(record->integer, from, to)) {
                ++stats.records;
                ++stats.composite_records;
                ++stats.miners[record->proof.provider_address].composite_records;
                stats.transaction_count += record->transactions.size();
                const auto fees = transactionFees(record->transactions);
                if (fees != 0) addFeeReward(stats, record->proof.provider_address, fees);
                addValidatorEvidence(stats, record->commit_phase, record->finalized_by);
            }
            pending.push_back({record->proof.provider_address, record->integer});
            if (record->integer <= to) stats.pending_composites_after_range = pending.size();
            continue;
        }

        const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!record.has_value()) return false;
        const bool count_record = inRange(record->integer, from, to);
        if (count_record) {
            ++stats.records;
            ++stats.prime_records;
            ++stats.miners[record->proof.provider_address].prime_records;
            stats.transaction_count += record->transactions.size();
            const auto fees = transactionFees(record->transactions);
            if (fees != 0) addFeeReward(stats, record->proof.provider_address, fees);
            addValidatorEvidence(stats, primechain::protocol::CommitPhaseCertificateV1{}, record->finalized_by);
        }

        if (count_record) {
            const bool validator_rewards_active = record->version != 0 && record->height != 0;
            if (pending.empty()) {
                addDiscoveryReward(stats, record->proof.provider_address,
                    primeMinerRewardMicroUnits(validator_rewards_active, false, 0));
            } else {
                const std::uint64_t composite_pool =
                    compositeRewardPoolMicroUnits(validator_rewards_active);
                const std::uint64_t per_composite = composite_pool / pending.size();
                const std::uint64_t remainder = composite_pool % pending.size();
                addDiscoveryReward(stats, record->proof.provider_address,
                    primeMinerRewardMicroUnits(validator_rewards_active, true, remainder));
                for (const auto& provider : pending) {
                    addDiscoveryReward(stats, provider.provider, per_composite);
                }
            }
        }
        pending.clear();
        if (record->integer <= to) stats.pending_composites_after_range = pending.size();
    }

    return true;
}

std::vector<std::pair<primechain::Address, AddressBoardStats>> sortedMinerStats(
    const std::map<primechain::Address, AddressBoardStats>& miners) {
    std::vector<std::pair<primechain::Address, AddressBoardStats>> out(miners.begin(), miners.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        const auto total_a = a.second.discovery_micro_units + a.second.fee_micro_units;
        const auto total_b = b.second.discovery_micro_units + b.second.fee_micro_units;
        if (total_a != total_b) return total_a > total_b;
        const auto records_a = a.second.prime_records + a.second.composite_records;
        const auto records_b = b.second.prime_records + b.second.composite_records;
        if (records_a != records_b) return records_a > records_b;
        return a.first < b.first;
    });
    return out;
}

std::vector<std::pair<primechain::Address, ValidatorBoardStats>> sortedValidatorStats(
    const std::map<primechain::Address, ValidatorBoardStats>& validators) {
    std::vector<std::pair<primechain::Address, ValidatorBoardStats>> out(validators.begin(), validators.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        const auto total_a = a.second.finalization_votes + a.second.round_change_votes + a.second.commit_phase_votes;
        const auto total_b = b.second.finalization_votes + b.second.round_change_votes + b.second.commit_phase_votes;
        if (total_a != total_b) return total_a > total_b;
        return a.first < b.first;
    });
    return out;
}

bool containsAddress(const std::vector<primechain::Address>& addresses, const primechain::Address& address) {
    return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

const char* validatorEvidenceClass(
    const primechain::Address& address,
    const std::vector<primechain::Address>& active_validators) {
    if (primechain::protocol::isDevelopmentAddress(address)) return "bootstrap-dev";
    if (containsAddress(active_validators, address)) return "active";
    return "historical";
}

std::vector<primechain::Address> genesisValidators(const primechain::node::ValidatorRegistryState& registry) {
    if (!registry.events.empty() && registry.events.front().type == primechain::node::ValidatorRegistryEventType::Genesis) {
        return registry.events.front().validator_set;
    }
    return {};
}

struct ValidatorEvidenceSummary {
    std::uint64_t active{0};
    std::uint64_t historical{0};
    std::uint64_t bootstrap_dev{0};
};

ValidatorEvidenceSummary summarizeValidatorEvidence(
    const std::map<primechain::Address, ValidatorBoardStats>& validators,
    const std::vector<primechain::Address>& active_validators) {
    ValidatorEvidenceSummary summary;
    for (const auto& entry : validators) {
        const std::string classification = validatorEvidenceClass(entry.first, active_validators);
        if (classification == "active") {
            ++summary.active;
        } else if (classification == "bootstrap-dev") {
            ++summary.bootstrap_dev;
        } else {
            ++summary.historical;
        }
    }
    return summary;
}

int boardReport(int argc, char** argv) {
    if (argc != 7 || std::string(argv[3]) != "--from" || std::string(argv[5]) != "--to") return 1;
    const std::string store_path = argv[2];
    const auto from = static_cast<primechain::PrimeValue>(std::stoull(argv[4]));
    const auto to = static_cast<primechain::PrimeValue>(std::stoull(argv[6]));

    BoardReportStats stats;
    std::string error;
    if (!collectBoardReportStats(store_path, from, to, stats, error)) {
        std::cerr << "could not build board report: " << error << "\n";
        return 1;
    }

    std::uint64_t discovery_total = 0;
    std::uint64_t unique_miners = 0;
    for (const auto& entry : stats.miners) {
        const auto record_count = entry.second.prime_records + entry.second.composite_records;
        const auto reward_total = entry.second.discovery_micro_units + entry.second.fee_micro_units;
        if (record_count != 0 || reward_total != 0) ++unique_miners;
        discovery_total += entry.second.discovery_micro_units;
    }

    std::cout << "BOARD_REPORT " << store_path << " from=" << stats.from << " to=" << stats.to << "\n";
    std::cout << "RECORDS total=" << stats.records
              << " prime=" << stats.prime_records
              << " composite=" << stats.composite_records
              << " transactions=" << stats.transaction_count << "\n";
    std::cout << "REWARDS discovery_micro_units=" << discovery_total
              << " fee_micro_units=" << stats.fee_micro_units
              << " unique_miners=" << unique_miners << "\n";
    std::cout << "PENDING_COMPOSITES_AFTER_RANGE " << stats.pending_composites_after_range << "\n";

    for (const auto& entry : sortedMinerStats(stats.miners)) {
        const auto records = entry.second.prime_records + entry.second.composite_records;
        const auto rewards = entry.second.discovery_micro_units + entry.second.fee_micro_units;
        if (records == 0 && rewards == 0) continue;
        std::cout << "MINER " << entry.first
                  << " prime_records=" << entry.second.prime_records
                  << " composite_records=" << entry.second.composite_records
                  << " discovery_micro_units=" << entry.second.discovery_micro_units
                  << " fee_micro_units=" << entry.second.fee_micro_units << "\n";
    }

    primechain::node::ValidatorRegistryState registry;
    const std::vector<primechain::Address> active_validators =
        primechain::node::loadValidatorRegistry(store_path, registry, error)
            ? registry.active_validators
            : std::vector<primechain::Address>{};
    const auto validator_summary = summarizeValidatorEvidence(stats.validators, active_validators);
    std::cout << "VALIDATOR_EVIDENCE_SUMMARY active=" << validator_summary.active
              << " historical=" << validator_summary.historical
              << " bootstrap_dev=" << validator_summary.bootstrap_dev << "\n";
    for (const auto& entry : sortedValidatorStats(stats.validators)) {
        std::cout << "VALIDATOR_EVIDENCE " << entry.first
                  << " class=" << validatorEvidenceClass(entry.first, active_validators)
                  << " finalization_votes=" << entry.second.finalization_votes
                  << " commit_phase_votes=" << entry.second.commit_phase_votes
                  << " round_change_votes=" << entry.second.round_change_votes << "\n";
    }
    return 0;
}


primechain::protocol::ValidatorWorkStatsV0 validatorWorkStatsFromBoardStats(
    const AddressBoardStats& stats) {
    return primechain::protocol::ValidatorWorkStatsV0{
        stats.prime_records,
        stats.composite_records,
        stats.discovery_micro_units};
}

std::vector<primechain::Address> validatorWorkSponsorsFromStore(
    const std::string& store_path,
    const primechain::Address& candidate) {
    std::set<primechain::Address> sponsors;
    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) return {};
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) return {};
            for (const auto& binding : record->validator_work_bindings) {
                if (binding.candidate_address == candidate) sponsors.insert(binding.miner_address);
            }
            continue;
        }
        const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) return {};
        for (const auto& binding : record->validator_work_bindings) {
            if (binding.candidate_address == candidate) sponsors.insert(binding.miner_address);
        }
    }
    return {sponsors.begin(), sponsors.end()};
}

int validatorEligibility(int argc, char** argv) {
    if (argc != 10 || std::string(argv[4]) != "--reserve" ||
        std::string(argv[6]) != "--observed" || std::string(argv[8]) != "--total") {
        return 1;
    }
    const std::string store_path = argv[2];
    const primechain::Address address = argv[3];
    std::uint64_t reserve_micro_units = 0;
    std::string error;
    if (std::string(argv[5]) == "auto") {
        primechain::node::SequentialNode node(store_path);
        if (!node.load(error)) {
            std::cerr << "could not derive validator reserve: " << error << "\n";
            return 1;
        }
        reserve_micro_units = node.lockedValidatorReserveMicroUnits(address);
    } else {
        reserve_micro_units = static_cast<std::uint64_t>(std::stoull(argv[5]));
    }
    const auto successful_observations = static_cast<std::uint64_t>(std::stoull(argv[7]));
    const auto total_observations = static_cast<std::uint64_t>(std::stoull(argv[9]));

    primechain::storage::RecordStore store(store_path);
    const auto latest = store.latest(error);
    if (!error.empty()) {
        std::cerr << "could not load latest record: " << error << "\n";
        return 1;
    }
    if (!latest.has_value()) {
        std::cerr << "record store is empty\n";
        return 1;
    }

    BoardReportStats report;
    if (!collectBoardReportStats(store_path, 2, latest->integer, report, error)) {
        std::cerr << "could not build eligibility report: " << error << "\n";
        return 1;
    }

    AddressBoardStats address_stats;
    const auto found = report.miners.find(address);
    if (found != report.miners.end()) address_stats = found->second;
    const auto sponsors = validatorWorkSponsorsFromStore(store_path, address);
    for (const auto& sponsor : sponsors) {
        if (sponsor == address) continue;
        const auto sponsor_found = report.miners.find(sponsor);
        if (sponsor_found == report.miners.end()) continue;
        address_stats.prime_records += sponsor_found->second.prime_records;
        address_stats.composite_records += sponsor_found->second.composite_records;
        address_stats.discovery_micro_units += sponsor_found->second.discovery_micro_units;
        address_stats.fee_micro_units += sponsor_found->second.fee_micro_units;
    }
    const auto work_stats = validatorWorkStatsFromBoardStats(address_stats);
    primechain::protocol::ValidatorEligibilityPolicyV0 policy;
    primechain::node::SequentialNode policy_node(store_path);
    if (policy_node.load(error)) policy.min_reserve_micro_units = policy_node.validatorMinReserveMicroUnits();
    const auto work_score = primechain::protocol::validatorWorkScoreV0(work_stats);
    const bool work_ok = primechain::protocol::validatorMeetsWorkMinimumV0(work_stats, policy);
    const bool reserve_ok = primechain::protocol::validatorMeetsReserveMinimumV0(reserve_micro_units, policy);
    const bool endpoint_ok = primechain::protocol::validatorMeetsEndpointUptimeMinimumV0(
        successful_observations, total_observations, policy);
    const bool eligible = work_ok && reserve_ok && endpoint_ok;

    std::cout << "VALIDATOR_ELIGIBILITY " << address
              << " eligible=" << (eligible ? 1 : 0) << "\n";
    std::cout << "WORK_HISTORY prime_records=" << work_stats.prime_records_mined
              << " composite_records=" << work_stats.composite_records_mined
              << " discovery_micro_units=" << work_stats.discovery_micro_units << "\n";
    std::cout << "WORK_SCORE score=" << work_score
              << " min=" << policy.min_work_score
              << " pass=" << (work_ok ? 1 : 0) << "\n";
    std::cout << "WORK_SPONSORS count=" << sponsors.size();
    for (const auto& sponsor : sponsors) std::cout << " " << sponsor;
    std::cout << "\n";
    std::cout << "RESERVE locked_micro_units=" << reserve_micro_units
              << " min=" << policy.min_reserve_micro_units
              << " pass=" << (reserve_ok ? 1 : 0) << "\n";
    std::cout << "ENDPOINT_OBSERVATION successful=" << successful_observations
              << " total=" << total_observations
              << " required_bps=" << policy.endpoint_required_uptime_bps
              << " window=" << policy.endpoint_observation_window
              << " pass=" << (endpoint_ok ? 1 : 0) << "\n";
    std::cout << "ADMISSION_RULE quorum=" << policy.admission_quorum_numerator
              << "/" << policy.admission_quorum_denominator
              << " activation_delay_epochs=" << policy.activation_delay_epochs
              << " epoch_length=" << policy.epoch_length << "\n";
    return 0;
}


int validatorReserve(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string store_path = argv[2];
    const primechain::Address validator_address = argv[3];
    const auto reserve_address = primechain::protocol::validatorReserveAddress(validator_address);

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "validator_reserve_error: " << error << "\n";
        return 1;
    }
    const auto holdings = node.holdingsForAddress(reserve_address);
    const auto total_micro_units = node.lockedValidatorReserveMicroUnits(validator_address);

    std::cout << "VALIDATOR_RESERVE " << store_path
              << " validator=" << validator_address
              << " reserve_address=" << reserve_address
              << " holdings=" << holdings.size()
              << " total_micro_units=" << total_micro_units << "\n";
    for (const auto& holding : holdings) {
        std::cout << "RESERVE_HOLDING validator=" << validator_address
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    return 0;
}

int validatorRegistry(int argc, char** argv) {
    if (argc != 3) return 1;
    primechain::node::ValidatorRegistryState state;
    std::string error;
    if (!primechain::node::loadValidatorRegistry(argv[2], state, error)) {
        std::cerr << "validator_registry_error: " << error << "\n";
        return 1;
    }

    std::cout << "VALIDATOR_REGISTRY " << argv[2]
              << " has_genesis=" << (state.has_genesis ? 1 : 0)
              << " current_epoch=" << state.current_epoch
              << " active_validators=" << state.active_validators.size()
              << " events=" << state.events.size() << "\n";
    std::cout << "ACTIVE_VALIDATORS";
    for (const auto& validator : state.active_validators) {
        std::cout << " " << validator;
    }
    std::cout << "\n";
    for (const auto& event : state.events) {
        std::cout << "VALIDATOR_REGISTRY_EVENT "
                  << primechain::node::validatorRegistryEventTypeName(event.type)
                  << " height=" << event.height
                  << " integer=" << event.record_integer
                  << " epoch=" << event.epoch
                  << " activation_integer=" << event.activation_integer
                  << " validators=" << event.validator_set.size();
        for (const auto& validator : event.validator_set) {
            std::cout << " " << validator;
        }
        std::cout << "\n";
    }
    return 0;
}

std::vector<PeerConfig> loadValidatorEndpointsFromStore(const std::string& store_path) {
    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        return {};
    }

    std::map<primechain::Address, primechain::protocol::ValidatorEndpointUpdateV1> latest;
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) {
                return {};
            }
            for (const auto& update : record->validator_endpoints) {
                latest[update.validator_address] = update;
            }
            continue;
        }
        const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) {
            return {};
        }
        for (const auto& update : record->validator_endpoints) {
            latest[update.validator_address] = update;
        }
    }

    std::vector<PeerConfig> endpoints;
    endpoints.reserve(latest.size());
    for (const auto& entry : latest) {
        const auto& update = entry.second;
        if (!update.host.empty() && update.port > 0 && update.port <= 65535) {
            endpoints.push_back({update.host, static_cast<int>(update.port)});
        }
    }
    return endpoints;
}

struct ValidatorEndpointReport {
    std::map<primechain::Address, primechain::protocol::ValidatorEndpointUpdateV1> latest;
    std::uint64_t event_count{0};
};

bool collectValidatorEndpointReport(
    const std::vector<primechain::storage::StoredRecord>& records,
    ValidatorEndpointReport& report,
    std::string& error) {
    report = ValidatorEndpointReport{};
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) return false;
            for (const auto& update : record->validator_endpoints) {
                report.latest[update.validator_address] = update;
                ++report.event_count;
            }
            continue;
        }
        const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) return false;
        for (const auto& update : record->validator_endpoints) {
            report.latest[update.validator_address] = update;
            ++report.event_count;
        }
    }
    return true;
}

int validatorEndpoints(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string store_path = argv[2];
    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "validator_endpoint_registry_error: " << error << "\n";
        return 1;
    }

    ValidatorEndpointReport report;
    if (!collectValidatorEndpointReport(records, report, error)) {
        std::cerr << "could not decode validator endpoint record: " << error << "\n";
        return 1;
    }

    std::cout << "VALIDATOR_ENDPOINT_REGISTRY " << store_path
              << " active_endpoints=" << report.latest.size()
              << " events=" << report.event_count << "\n";
    for (const auto& entry : report.latest) {
        const auto& update = entry.second;
        std::cout << "VALIDATOR_ENDPOINT " << update.validator_address
                  << " host=" << update.host
                  << " port=" << update.port
                  << " effective_integer=" << update.effective_integer
                  << " sequence=" << update.sequence << "\n";
    }
    return 0;
}

struct EconomicPolicyReport {
    std::vector<std::pair<primechain::PrimeValue, primechain::protocol::EconomicPolicyUpdateV1>> events;
};

bool collectEconomicPolicyReport(
    const std::vector<primechain::storage::StoredRecord>& records,
    EconomicPolicyReport& report,
    std::string& error) {
    report = EconomicPolicyReport{};
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) return false;
            if (record->economic_policy.transfer_fee_micro_units != 0 ||
                record->economic_policy.validator_min_reserve_micro_units != 0) {
                report.events.push_back({record->integer, record->economic_policy});
            }
            continue;
        }
        const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) return false;
        if (record->economic_policy.transfer_fee_micro_units != 0 ||
            record->economic_policy.validator_min_reserve_micro_units != 0) {
            report.events.push_back({record->integer, record->economic_policy});
        }
    }
    return true;
}

int economicPolicy(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string store_path = argv[2];
    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "economic_policy_error: " << error << "\n";
        return 1;
    }

    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "economic_policy_error: " << error << "\n";
        return 1;
    }

    EconomicPolicyReport report;
    if (!collectEconomicPolicyReport(records, report, error)) {
        std::cerr << "could not decode economic policy record: " << error << "\n";
        return 1;
    }

    std::cout << "ECONOMIC_POLICY_REGISTRY " << store_path
              << " active_transfer_fee_micro_units=" << node.transferFeeMicroUnits()
              << " active_validator_min_reserve_micro_units=" << node.validatorMinReserveMicroUnits()
              << " events=" << report.events.size() << "\n";
    for (const auto& entry : report.events) {
        const auto& update = entry.second;
        std::cout << "ECONOMIC_POLICY_EVENT integer=" << entry.first
                  << " transfer_fee_micro_units=" << update.transfer_fee_micro_units
                  << " validator_min_reserve_micro_units=" << update.validator_min_reserve_micro_units
                  << " effective_integer=" << update.effective_integer
                  << " sequence=" << update.sequence
                  << " votes=" << update.votes.size() << "\n";
    }
    return 0;
}

int launchReport(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string store_path = argv[2];

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "launch_report_error: " << error << "\n";
        return 1;
    }
    const auto status = node.status();

    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "launch_report_error: " << error << "\n";
        return 1;
    }

    std::uint64_t prime_records = 0;
    std::uint64_t composite_records = 0;
    std::uint64_t transaction_count = 0;
    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            ++prime_records;
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "could not decode prime record: " << error << "\n";
                return 1;
            }
            transaction_count += record->transactions.size();
            continue;
        }
        ++composite_records;
        const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!record.has_value()) {
            std::cerr << "could not decode composite record: " << error << "\n";
            return 1;
        }
        transaction_count += record->transactions.size();
    }

    primechain::node::ValidatorRegistryState registry;
    if (!primechain::node::loadValidatorRegistry(store_path, registry, error)) {
        std::cerr << "validator_registry_error: " << error << "\n";
        return 1;
    }

    ValidatorEndpointReport endpoints;
    if (!collectValidatorEndpointReport(records, endpoints, error)) {
        std::cerr << "validator_endpoint_registry_error: " << error << "\n";
        return 1;
    }

    EconomicPolicyReport policy;
    if (!collectEconomicPolicyReport(records, policy, error)) {
        std::cerr << "economic_policy_error: " << error << "\n";
        return 1;
    }

    BoardReportStats board;
    if (!records.empty() && !collectBoardReportStats(store_path, 2, status.frontier_integer, board, error)) {
        std::cerr << "could not build board report: " << error << "\n";
        return 1;
    }

    std::uint64_t discovery_total = 0;
    std::uint64_t unique_miners = 0;
    for (const auto& entry : board.miners) {
        const auto record_count = entry.second.prime_records + entry.second.composite_records;
        const auto reward_total = entry.second.discovery_micro_units + entry.second.fee_micro_units;
        if (record_count != 0 || reward_total != 0) ++unique_miners;
        discovery_total += entry.second.discovery_micro_units;
    }

    const auto fee_pool_address = node.validatorFeePoolAddress();
    const auto fee_pool_holdings = node.holdingsForAddress(fee_pool_address);
    std::uint64_t fee_pool_total = 0;
    for (const auto& holding : fee_pool_holdings) fee_pool_total += holding.second;
    const auto reward_pool_address = node.validatorRewardPoolAddress();
    const auto reward_pool_holdings = node.holdingsForAddress(reward_pool_address);
    std::uint64_t reward_pool_total = 0;
    for (const auto& holding : reward_pool_holdings) reward_pool_total += holding.second;
    const auto validator_summary = summarizeValidatorEvidence(board.validators, registry.active_validators);
    const auto genesis_validators = genesisValidators(registry);

    std::cout << "LAUNCH_REPORT " << store_path << "\n";
    std::cout << "CHAIN has_genesis=" << (status.has_genesis ? 1 : 0)
              << " height=" << status.height
              << " frontier=" << status.frontier_integer
              << " latest_hash=" << primechain::crypto::toHex(status.latest_record_hash)
              << " records=" << records.size()
              << " prime_records=" << prime_records
              << " composite_records=" << composite_records
              << " transactions=" << transaction_count << "\n";
    std::cout << "VALIDATOR_STATE epoch=" << node.validatorEpoch()
              << " active_validators=" << registry.active_validators.size()
              << " registry_events=" << registry.events.size()
              << " endpoint_events=" << endpoints.event_count
              << " active_endpoints=" << endpoints.latest.size()
              << " transfer_fee_micro_units=" << node.transferFeeMicroUnits()
              << " validator_min_reserve_micro_units=" << node.validatorMinReserveMicroUnits() << "\n";
    std::cout << "VALIDATOR_EVIDENCE_SUMMARY active=" << validator_summary.active
              << " historical=" << validator_summary.historical
              << " bootstrap_dev=" << validator_summary.bootstrap_dev << "\n";
    std::cout << "ACTIVE_VALIDATORS";
    for (const auto& validator : registry.active_validators) std::cout << " " << validator;
    std::cout << "\n";
    for (const auto& event : registry.events) {
        std::cout << "VALIDATOR_REGISTRY_EVENT "
                  << primechain::node::validatorRegistryEventTypeName(event.type)
                  << " height=" << event.height
                  << " integer=" << event.record_integer
                  << " epoch=" << event.epoch
                  << " activation_integer=" << event.activation_integer
                  << " validators=" << event.validator_set.size();
        for (const auto& validator : event.validator_set) std::cout << " " << validator;
        std::cout << "\n";
    }
    for (const auto& entry : endpoints.latest) {
        const auto& update = entry.second;
        std::cout << "VALIDATOR_ENDPOINT " << update.validator_address
                  << " host=" << update.host
                  << " port=" << update.port
                  << " effective_integer=" << update.effective_integer
                  << " sequence=" << update.sequence << "\n";
    }
    for (const auto& validator : registry.active_validators) {
        const auto reserve_address = primechain::protocol::validatorReserveAddress(validator);
        const auto reserve_holdings = node.holdingsForAddress(reserve_address);
        const auto admission = containsAddress(genesis_validators, validator) ? "genesis" : "reserve";
        std::cout << "VALIDATOR_RESERVE_SUMMARY " << validator
                  << " admission=" << admission
                  << " holdings=" << reserve_holdings.size()
                  << " total_micro_units=" << node.lockedValidatorReserveMicroUnits(validator) << "\n";
    }
    std::cout << "ECONOMIC_POLICY active_transfer_fee_micro_units=" << node.transferFeeMicroUnits()
              << " active_validator_min_reserve_micro_units=" << node.validatorMinReserveMicroUnits()
              << " events=" << policy.events.size() << "\n";
    for (const auto& entry : policy.events) {
        const auto& update = entry.second;
        std::cout << "ECONOMIC_POLICY_EVENT integer=" << entry.first
                  << " transfer_fee_micro_units=" << update.transfer_fee_micro_units
                  << " validator_min_reserve_micro_units=" << update.validator_min_reserve_micro_units
                  << " effective_integer=" << update.effective_integer
                  << " sequence=" << update.sequence
                  << " votes=" << update.votes.size() << "\n";
    }
    std::cout << "VALIDATOR_FEE_POOL epoch=" << node.validatorEpoch()
              << " address=" << fee_pool_address
              << " holdings=" << fee_pool_holdings.size()
              << " total_micro_units=" << fee_pool_total << "\n";
    for (const auto& holding : fee_pool_holdings) {
        std::cout << "FEE_POOL_HOLDING epoch=" << node.validatorEpoch()
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    std::cout << "VALIDATOR_REWARD_POOL epoch=" << node.validatorEpoch()
              << " address=" << reward_pool_address
              << " holdings=" << reward_pool_holdings.size()
              << " total_micro_units=" << reward_pool_total << "\n";
    for (const auto& holding : reward_pool_holdings) {
        std::cout << "VALIDATOR_REWARD_HOLDING epoch=" << node.validatorEpoch()
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    std::cout << "BOARD records=" << board.records
              << " prime=" << board.prime_records
              << " composite=" << board.composite_records
              << " transactions=" << board.transaction_count
              << " discovery_micro_units=" << discovery_total
              << " fee_micro_units=" << board.fee_micro_units
              << " unique_miners=" << unique_miners
              << " pending_composites_after_range=" << board.pending_composites_after_range << "\n";
    for (const auto& entry : sortedValidatorStats(board.validators)) {
        std::cout << "VALIDATOR_EVIDENCE " << entry.first
                  << " class=" << validatorEvidenceClass(entry.first, registry.active_validators)
                  << " finalization_votes=" << entry.second.finalization_votes
                  << " commit_phase_votes=" << entry.second.commit_phase_votes
                  << " round_change_votes=" << entry.second.round_change_votes << "\n";
    }
    return 0;
}


int feeDistributionStatus(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 1;
    const std::string store_path = argv[2];
    const std::uint64_t interval_records = argc == 4
        ? static_cast<std::uint64_t>(std::stoull(argv[3]))
        : 1000;
    if (interval_records == 0) {
        std::cerr << "fee_distribution_status_error: interval must be positive\n";
        return 1;
    }

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "fee_distribution_status_error: " << error << "\n";
        return 1;
    }
    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "fee_distribution_status_error: " << error << "\n";
        return 1;
    }

    std::uint64_t distribution_count = 0;
    primechain::PrimeValue last_distribution_integer = 0;
    std::uint64_t last_distribution_epoch = 0;
    primechain::PrimeValue last_distribution_prime = 0;
    std::uint64_t last_distribution_micro_units = 0;
    std::vector<std::string> events;

    for (const auto& stored : records) {
        std::vector<primechain::protocol::TransactionV0> transactions;
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "fee_distribution_status_error: " << error << "\n";
                return 1;
            }
            transactions = record->transactions;
        } else {
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "fee_distribution_status_error: " << error << "\n";
                return 1;
            }
            transactions = record->transactions;
        }
        for (const auto& tx : transactions) {
            if (tx.version != 3 ||
                tx.sender_address.rfind("pcpool_validator_fees_epoch_", 0) != 0 ||
                tx.inputs.empty()) {
                continue;
            }
            const auto epoch_text = tx.sender_address.substr(std::string("pcpool_validator_fees_epoch_").size());
            std::uint64_t epoch = 0;
            try {
                epoch = static_cast<std::uint64_t>(std::stoull(epoch_text));
            } catch (...) {
                continue;
            }
            ++distribution_count;
            last_distribution_integer = stored.integer;
            last_distribution_epoch = epoch;
            last_distribution_prime = tx.inputs.front().prime;
            last_distribution_micro_units = tx.inputs.front().amount.denominator == 1
                ? tx.inputs.front().amount.numerator
                : 0;

            std::ostringstream event;
            event << "FEE_DISTRIBUTION_EVENT integer=" << stored.integer
                  << " epoch=" << epoch
                  << " prime=" << last_distribution_prime
                  << " micro_units=" << last_distribution_micro_units
                  << " recipients=" << tx.outputs.size();
            events.push_back(event.str());
        }
    }

    const auto status = node.status();
    const auto current_frontier = status.has_genesis ? status.frontier_integer : 0;
    const auto anchor = last_distribution_integer == 0 ? 2 : last_distribution_integer;
    const auto next_distribution_integer = anchor + interval_records;
    const bool due = current_frontier >= next_distribution_integer;

    const auto pool_address = node.validatorFeePoolAddress();
    const auto holdings = node.holdingsForAddress(pool_address);
    const auto eligible_recipients = node.feeDistributionRecipients();
    std::uint64_t pool_total = 0;
    for (const auto& holding : holdings) pool_total += holding.second;

    std::cout << "FEE_DISTRIBUTION_STATUS " << store_path
              << " interval_records=" << interval_records
              << " current_frontier=" << current_frontier
              << " last_distribution_integer=" << last_distribution_integer
              << " next_distribution_integer=" << next_distribution_integer
              << " due=" << (due ? 1 : 0)
              << " current_epoch=" << node.validatorEpoch()
              << " pool_address=" << pool_address
              << " pool_holdings=" << holdings.size()
              << " pool_total_micro_units=" << pool_total
              << " distributions=" << distribution_count
              << " eligible_recipients=" << eligible_recipients.size() << "\n";
    if (last_distribution_integer != 0) {
        std::cout << "LAST_FEE_DISTRIBUTION integer=" << last_distribution_integer
                  << " epoch=" << last_distribution_epoch
                  << " prime=" << last_distribution_prime
                  << " micro_units=" << last_distribution_micro_units << "\n";
    }
    for (const auto& recipient : eligible_recipients) {
        std::cout << "FEE_DISTRIBUTION_RECIPIENT address=" << recipient << "\n";
    }
    for (const auto& holding : holdings) {
        std::cout << "FEE_POOL_HOLDING epoch=" << node.validatorEpoch()
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    for (const auto& event : events) std::cout << event << "\n";
    return 0;
}

int feePool(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 1;
    const std::string store_path = argv[2];

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "fee_pool_error: " << error << "\n";
        return 1;
    }

    const std::uint64_t epoch = argc == 4 ? static_cast<std::uint64_t>(std::stoull(argv[3]))
                                         : node.validatorEpoch();
    const auto pool_address = primechain::protocol::validatorFeePoolAddress(epoch);
    const auto holdings = node.holdingsForAddress(pool_address);

    std::uint64_t total_micro_units = 0;
    for (const auto& holding : holdings) total_micro_units += holding.second;

    std::cout << "VALIDATOR_FEE_POOL " << store_path
              << " epoch=" << epoch
              << " address=" << pool_address
              << " holdings=" << holdings.size()
              << " total_micro_units=" << total_micro_units << "\n";
    for (const auto& holding : holdings) {
        std::cout << "FEE_POOL_HOLDING epoch=" << epoch
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    return 0;
}

int validatorReputation(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string store_path = argv[2];
    const primechain::Address address = argv[3];

    primechain::storage::RecordStore store(store_path);
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "could not load record store: " << error << "\n";
        return 1;
    }

    std::uint64_t prime_records = 0;
    std::uint64_t composite_records = 0;
    std::uint64_t discovery_micro_units = 0;
    std::uint64_t fee_micro_units = 0;
    std::uint64_t finalization_votes = 0;
    std::uint64_t round_change_votes = 0;
    std::uint64_t commit_phase_votes = 0;
    std::uint64_t policy_votes = 0;
    std::uint64_t epoch_votes = 0;
    std::vector<PendingCompositeReward> pending;

    for (const auto& stored : records) {
        if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "could not decode composite record: " << error << "\n";
                return 1;
            }
            if (record->proof.provider_address == address) {
                ++composite_records;
            }
            for (const auto& vote : record->commit_phase.votes) {
                if (vote.validator_address == address) ++commit_phase_votes;
            }
            for (const auto& vote : record->finalized_by.votes) {
                if (vote.validator_address == address) ++finalization_votes;
            }
            for (const auto& vote : record->finalized_by.round_changes) {
                if (vote.validator_address == address) ++round_change_votes;
            }
            for (const auto& vote : record->validator_epoch.votes) {
                if (vote.validator_address == address) ++epoch_votes;
            }
            pending.push_back({record->proof.provider_address, record->integer});
            continue;
        }

        const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!record.has_value()) {
            std::cerr << "could not decode prime record: " << error << "\n";
            return 1;
        }
        if (record->proof.provider_address == address) {
            ++prime_records;
        }
        for (const auto& vote : record->finalized_by.votes) {
            if (vote.validator_address == address) ++finalization_votes;
        }
        for (const auto& vote : record->finalized_by.round_changes) {
            if (vote.validator_address == address) ++round_change_votes;
        }
        for (const auto& vote : record->validator_epoch.votes) {
            if (vote.validator_address == address) ++epoch_votes;
        }

        const bool validator_rewards_active = record->version != 0 && record->height != 0;
        if (pending.empty()) {
            if (record->proof.provider_address == address) {
                discovery_micro_units += primeMinerRewardMicroUnits(
                    validator_rewards_active, false, 0);
            }
        } else {
            const std::uint64_t composite_pool =
                compositeRewardPoolMicroUnits(validator_rewards_active);
            const std::uint64_t per_composite = composite_pool / pending.size();
            const std::uint64_t remainder = composite_pool % pending.size();
            if (record->proof.provider_address == address) {
                discovery_micro_units += primeMinerRewardMicroUnits(
                    validator_rewards_active, true, remainder);
            }
            for (const auto& provider : pending) {
                if (provider.provider == address) discovery_micro_units += per_composite;
            }
        }
        pending.clear();
    }

    const auto work_score = primechain::protocol::validatorWorkScoreV0(
        {prime_records, composite_records, discovery_micro_units});
    const auto participation = finalization_votes + commit_phase_votes + round_change_votes;
    std::cout << "VALIDATOR_REPUTATION " << address << "\n";
    std::cout << "MINING_HISTORY prime_records=" << prime_records
              << " composite_records=" << composite_records
              << " work_score=" << work_score
              << " discovery_micro_units=" << discovery_micro_units
              << " fee_micro_units=" << fee_micro_units << "\n";
    std::cout << "VALIDATOR_PARTICIPATION finalization_votes=" << finalization_votes
              << " commit_phase_votes=" << commit_phase_votes
              << " round_change_votes=" << round_change_votes
              << " total_participation_events=" << participation << "\n";
    std::cout << "GOVERNANCE_PARTICIPATION epoch_votes=" << epoch_votes
              << " policy_votes=" << policy_votes << "\n";
    std::cout << "TRANSFERABILITY mining_history=non_transferable reserve_required=not_implemented\n";
    return 0;
}

int updateIndexes(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    if (!ensureWorkdirLayout(workdir)) return 1;

    primechain::storage::RecordStore store(chainPath(workdir));
    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "could not load workdir chain: " << error << "\n";
        return 1;
    }

    StoreProofIndex index;
    StatusLine status;
    std::uint64_t prime_records = 0;
    std::uint64_t composite_records = 0;
    for (const auto& stored : records) {
        status.has_genesis = true;
        status.height = stored.height;
        status.frontier = stored.integer;
        if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
            ++composite_records;
            const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!decoded.has_value()) {
                std::cerr << "could not decode composite record: " << error << "\n";
                return 1;
            }
            const auto proof = toCompositeProof(decoded->proof);
            if (!primechain::math::verifyCompositeProof(proof)) {
                std::cerr << "stored composite proof is invalid\n";
                return 1;
            }
            index.add(proof);
        } else {
            ++prime_records;
        }
    }

    if (!writeProofIndexFile(compositeProofIndexPath(workdir), index, status, prime_records, composite_records, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "INDEX_UPDATED " << workdir << " frontier=" << status.frontier
              << " proofs=" << index.size() << " path=" << compositeProofIndexPath(workdir) << "\n";
    return 0;
}


int validatorRewardPool(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 1;
    const std::string store_path = argv[2];

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "validator_reward_pool_error: " << error << "\n";
        return 1;
    }

    const std::uint64_t epoch = argc == 4 ? static_cast<std::uint64_t>(std::stoull(argv[3]))
                                         : node.validatorEpoch();
    const auto pool_address = primechain::protocol::validatorRewardPoolAddress(epoch);
    const auto holdings = node.holdingsForAddress(pool_address);

    std::uint64_t total_micro_units = 0;
    for (const auto& holding : holdings) total_micro_units += holding.second;

    std::cout << "VALIDATOR_REWARD_POOL " << store_path
              << " epoch=" << epoch
              << " address=" << pool_address
              << " holdings=" << holdings.size()
              << " total_micro_units=" << total_micro_units << "\n";
    for (const auto& holding : holdings) {
        std::cout << "VALIDATOR_REWARD_HOLDING epoch=" << epoch
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    return 0;
}

int validatorRewardDistributionStatus(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 1;
    const std::string store_path = argv[2];
    const std::uint64_t interval_primes = argc == 4
        ? static_cast<std::uint64_t>(std::stoull(argv[3]))
        : 1000;
    if (interval_primes == 0) {
        std::cerr << "validator_reward_distribution_status_error: interval must be positive\n";
        return 1;
    }

    std::string error;
    primechain::node::SequentialNode node(store_path);
    if (!node.load(error)) {
        std::cerr << "validator_reward_distribution_status_error: " << error << "\n";
        return 1;
    }
    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "validator_reward_distribution_status_error: " << error << "\n";
        return 1;
    }

    std::uint64_t prime_record_count = 0;
    std::uint64_t distribution_count = 0;
    std::uint64_t last_distribution_prime_count = 0;
    primechain::PrimeValue last_distribution_integer = 0;
    std::uint64_t last_distribution_epoch = 0;
    primechain::PrimeValue last_distribution_prime = 0;
    std::uint64_t last_distribution_micro_units = 0;
    std::vector<std::string> events;

    for (const auto& stored : records) {
        std::vector<primechain::protocol::TransactionV0> transactions;
        if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
            ++prime_record_count;
            const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "validator_reward_distribution_status_error: " << error << "\n";
                return 1;
            }
            transactions = record->transactions;
        } else {
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) {
                std::cerr << "validator_reward_distribution_status_error: " << error << "\n";
                return 1;
            }
            transactions = record->transactions;
        }
        for (const auto& tx : transactions) {
            if (tx.version != 5 ||
                tx.sender_address.rfind("pcpool_validator_rewards_epoch_", 0) != 0 ||
                tx.inputs.empty()) {
                continue;
            }
            const auto epoch_text = tx.sender_address.substr(std::string("pcpool_validator_rewards_epoch_").size());
            std::uint64_t epoch = 0;
            try {
                epoch = static_cast<std::uint64_t>(std::stoull(epoch_text));
            } catch (...) {
                continue;
            }
            ++distribution_count;
            last_distribution_prime_count = prime_record_count;
            last_distribution_integer = stored.integer;
            last_distribution_epoch = epoch;
            last_distribution_prime = tx.inputs.front().prime;
            last_distribution_micro_units = tx.inputs.front().amount.denominator == 1
                ? tx.inputs.front().amount.numerator
                : 0;

            std::ostringstream event;
            event << "VALIDATOR_REWARD_DISTRIBUTION_EVENT integer=" << stored.integer
                  << " prime_count=" << prime_record_count
                  << " epoch=" << epoch
                  << " prime=" << last_distribution_prime
                  << " micro_units=" << last_distribution_micro_units
                  << " recipients=" << tx.outputs.size();
            events.push_back(event.str());
        }
    }

    const auto next_distribution_prime_count = last_distribution_prime_count + interval_primes;
    const bool due = prime_record_count >= next_distribution_prime_count;
    const auto pool_address = node.validatorRewardPoolAddress();
    const auto holdings = node.holdingsForAddress(pool_address);
    const auto eligible_recipients = node.validatorRewardDistributionRecipients();
    std::uint64_t pool_total = 0;
    for (const auto& holding : holdings) pool_total += holding.second;

    std::cout << "VALIDATOR_REWARD_DISTRIBUTION_STATUS " << store_path
              << " interval_primes=" << interval_primes
              << " current_prime_records=" << prime_record_count
              << " last_distribution_prime_count=" << last_distribution_prime_count
              << " next_distribution_prime_count=" << next_distribution_prime_count
              << " due=" << (due ? 1 : 0)
              << " current_epoch=" << node.validatorEpoch()
              << " pool_address=" << pool_address
              << " pool_holdings=" << holdings.size()
              << " pool_total_micro_units=" << pool_total
              << " distributions=" << distribution_count
              << " eligible_recipients=" << eligible_recipients.size() << "\n";
    if (last_distribution_integer != 0) {
        std::cout << "LAST_VALIDATOR_REWARD_DISTRIBUTION integer=" << last_distribution_integer
                  << " prime_count=" << last_distribution_prime_count
                  << " epoch=" << last_distribution_epoch
                  << " prime=" << last_distribution_prime
                  << " micro_units=" << last_distribution_micro_units << "\n";
    }
    for (const auto& recipient : eligible_recipients) {
        std::cout << "VALIDATOR_REWARD_DISTRIBUTION_RECIPIENT address=" << recipient << "\n";
    }
    for (const auto& holding : holdings) {
        std::cout << "VALIDATOR_REWARD_HOLDING epoch=" << node.validatorEpoch()
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    for (const auto& event : events) std::cout << event << "\n";
    return 0;
}

bool readAddressIndexMeta(
    const std::string& workdir,
    primechain::PrimeValue& checkpoint_integer,
    std::string& checkpoint_hash_hex,
    std::uint64_t& event_count,
    bool& present) {
    const auto path = addressIndexMetaPath(workdir);
    checkpoint_integer = 0;
    checkpoint_hash_hex.clear();
    event_count = 0;
    present = false;
    if (!pathExists(path)) return true;
    const auto values = readKeyValueFile(path);
    const auto version = values.find("version");
    if (version == values.end() || version->second != "primechain-address-index-v1") {
        return false;
    }
    if (values.count("checkpoint_integer")) checkpoint_integer = std::stoull(values.at("checkpoint_integer"));
    if (values.count("checkpoint_hash")) checkpoint_hash_hex = values.at("checkpoint_hash");
    if (values.count("event_count")) event_count = std::stoull(values.at("event_count"));
    present = true;
    return true;
}

bool writeAddressIndexMeta(
    const std::string& workdir,
    primechain::PrimeValue checkpoint_integer,
    const std::string& checkpoint_hash_hex,
    std::uint64_t event_count) {
    std::map<std::string, std::string> values;
    values["version"] = "primechain-address-index-v1";
    values["checkpoint_integer"] = std::to_string(checkpoint_integer);
    values["checkpoint_hash"] = checkpoint_hash_hex;
    values["event_count"] = std::to_string(event_count);
    return writeKeyValueFile(addressIndexMetaPath(workdir), values);
}

// Incrementally extends the address index up to the workdir's current
// frontier. Anchored on (checkpoint_integer, checkpoint_hash): before
// trusting the checkpoint as a starting point, re-reads that exact record
// from the canonical store and compares hashes. Because every record's
// hash is chained through previous_record_hash, a match at the checkpoint
// integer cryptographically guarantees nothing before it changed either --
// so this single-point check is sufficient to detect any divergence
// (a tip replacement, a resync onto a different canonical history, a
// corrupted local file) up to and including the checkpoint. On a mismatch
// (or a missing/corrupt meta file) the index is wiped and rebuilt from the
// local store in full, which is always safe since it carries no state that
// isn't re-derivable from chain.dat.
int updateAddressIndex(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    if (!ensureWorkdirLayout(workdir)) return 1;

    primechain::storage::RecordStore store(chainPath(workdir));
    std::string error;
    const auto latest = store.latest(error);
    if (!error.empty()) {
        std::cerr << "could not read workdir chain: " << error << "\n";
        return 1;
    }
    if (!latest.has_value()) {
        std::cout << "ADDRESS_INDEX_UPDATED " << workdir << " from=0 to=0 new_events=0 rebuilt=0\n";
        return 0;
    }
    const auto frontier = latest->integer;

    primechain::PrimeValue checkpoint_integer = 0;
    std::string checkpoint_hash_hex;
    std::uint64_t event_count = 0;
    bool meta_present = false;
    bool diverged = !readAddressIndexMeta(workdir, checkpoint_integer, checkpoint_hash_hex, event_count, meta_present);
    if (meta_present && !diverged && checkpoint_integer > 0) {
        const auto checkpoint_record = store.findByInteger(checkpoint_integer, error);
        if (!error.empty()) {
            std::cerr << "could not verify address index checkpoint: " << error << "\n";
            return 1;
        }
        if (!checkpoint_record.has_value() ||
            primechain::crypto::toHex(checkpoint_record->record_hash) != checkpoint_hash_hex) {
            diverged = true;
        }
    }

    primechain::PrimeValue start = checkpoint_integer + 1;
    const auto reported_from = checkpoint_integer;
    if (!meta_present || diverged) {
        unlink(addressIndexEventsPath(workdir).c_str());
        unlink(addressIndexMetaPath(workdir).c_str());
        event_count = 0;
        start = 0;
    }

    if (meta_present && !diverged && start > frontier) {
        std::cout << "ADDRESS_INDEX_UPDATED " << workdir
                  << " from=" << reported_from << " to=" << frontier
                  << " new_events=0 rebuilt=0\n";
        return 0;
    }

    const auto new_records = start == 0 ? store.loadAll(error) : store.findRange(start, frontier, error);
    if (!error.empty()) {
        std::cerr << "could not load new records: " << error << "\n";
        return 1;
    }

    std::vector<AddressIndexEvent> new_events;
    for (const auto& stored : new_records) {
        const auto transactions = storedTransactions(stored, error);
        if (!transactions.has_value()) {
            std::cerr << "could not decode record " << stored.integer << ": " << error << "\n";
            return 1;
        }
        extractAddressIndexEvents(stored, *transactions, new_events);
    }

    if (!ensureDirectory(indexesDir(workdir))) return 1;

    const auto events_path = addressIndexEventsPath(workdir);
    const auto temp_events_path = events_path + ".tmp";
    {
        std::ofstream out(temp_events_path, std::ios::trunc);
        if (!out) {
            std::cerr << "could not open temporary address index for write\n";
            return 1;
        }

        std::uint64_t copied_events = 0;
        if (meta_present && !diverged && event_count > 0) {
            std::ifstream existing(events_path);
            if (!existing) {
                std::cerr << "address index metadata exists but events file is missing; rebuilding\n";
                unlink(addressIndexMetaPath(workdir).c_str());
                unlink(events_path.c_str());
                unlink(temp_events_path.c_str());
                return updateAddressIndex(argc, argv);
            }
            std::string existing_line;
            while (copied_events < event_count && std::getline(existing, existing_line)) {
                if (!parseAddressIndexEventLine(existing_line).has_value()) {
                    std::cerr << "address index contains a malformed event; rebuilding\n";
                    unlink(addressIndexMetaPath(workdir).c_str());
                    unlink(events_path.c_str());
                    unlink(temp_events_path.c_str());
                    return updateAddressIndex(argc, argv);
                }
                out << existing_line << "\n";
                ++copied_events;
            }
            if (copied_events != event_count) {
                std::cerr << "address index ended before metadata event count; rebuilding\n";
                unlink(addressIndexMetaPath(workdir).c_str());
                unlink(events_path.c_str());
                unlink(temp_events_path.c_str());
                return updateAddressIndex(argc, argv);
            }
        }

        for (const auto& event : new_events) {
            out << formatAddressIndexEventLine(event) << "\n";
        }
        if (!out) {
            std::cerr << "could not write address index events\n";
            unlink(temp_events_path.c_str());
            return 1;
        }
    }
    if (std::rename(temp_events_path.c_str(), events_path.c_str()) != 0) {
        std::cerr << "could not replace address index events file: " << std::strerror(errno) << "\n";
        unlink(temp_events_path.c_str());
        return 1;
    }

    event_count += new_events.size();
    if (!writeAddressIndexMeta(workdir, frontier, primechain::crypto::toHex(latest->record_hash), event_count)) {
        std::cerr << "could not write address index checkpoint\n";
        return 1;
    }

    std::cout << "ADDRESS_INDEX_UPDATED " << workdir
              << " from=" << (start == 0 ? primechain::PrimeValue{0} : reported_from) << " to=" << frontier
              << " new_events=" << new_events.size()
              << " rebuilt=" << (diverged ? 1 : 0) << "\n";
    return 0;
}

int addressIndexStatus(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    const auto events_path = addressIndexEventsPath(workdir);
    primechain::PrimeValue checkpoint_integer = 0;
    std::string checkpoint_hash_hex;
    std::uint64_t event_count = 0;
    bool present = false;
    if (!readAddressIndexMeta(workdir, checkpoint_integer, checkpoint_hash_hex, event_count, present)) {
        std::cout << "ADDRESS_INDEX_INVALID " << workdir << "\n";
        return 1;
    }
    if (!present) {
        std::cout << "ADDRESS_INDEX_MISSING " << workdir << " path=" << events_path << "\n";
        return 0;
    }
    std::cout << "ADDRESS_INDEX_STATUS " << workdir
              << " checkpoint_integer=" << checkpoint_integer
              << " checkpoint_hash=" << checkpoint_hash_hex
              << " events=" << event_count
              << " path=" << events_path << "\n";
    return 0;
}

// Fast wallet-history: reads events from the address index instead of
// decoding and signature-verifying every record in the chain. Requires the
// index to be present and caught up to the workdir's current frontier
// (same anchor check as updateAddressIndex) -- if it's missing or stale,
// fails with a message pointing at update-address-index rather than
// silently falling back to a full replay, so staleness is never masked.
// Returns the workdir's current frontier if the address index is present
// and caught up to it (same anchor check as updateAddressIndex);
// std::nullopt (with a message on stderr pointing at update-address-index)
// otherwise. Shared by every fast, index-backed workdir command so none of
// them can silently serve stale or missing data.
std::optional<primechain::PrimeValue> requireFreshAddressIndex(const std::string& workdir) {
    primechain::PrimeValue checkpoint_integer = 0;
    std::string checkpoint_hash_hex;
    std::uint64_t event_count = 0;
    bool present = false;
    if (!readAddressIndexMeta(workdir, checkpoint_integer, checkpoint_hash_hex, event_count, present) || !present) {
        std::cerr << "address index is missing; run update-address-index\n";
        return std::nullopt;
    }
    primechain::storage::RecordStore store(chainPath(workdir));
    std::string error;
    const auto latest = store.latest(error);
    if (!error.empty()) {
        std::cerr << "could not read workdir chain: " << error << "\n";
        return std::nullopt;
    }
    const auto frontier = latest.has_value() ? latest->integer : primechain::PrimeValue{0};
    if (!latest.has_value() || checkpoint_integer != frontier ||
        primechain::crypto::toHex(latest->record_hash) != checkpoint_hash_hex) {
        std::cerr << "address index is stale; run update-address-index\n";
        return std::nullopt;
    }
    return frontier;
}

int walletHistoryWorkdir(int argc, char** argv) {
    if (argc != 4 && argc != 6) return 1;
    const std::string workdir = argv[2];
    const std::string wallet_path = argv[3];
    std::uint64_t last = 0;
    if (argc == 6) {
        if (std::string(argv[4]) != "--last") return 1;
        last = static_cast<std::uint64_t>(std::stoull(argv[5]));
    }
    const auto address = loadMinerAddress(wallet_path);
    if (!address.has_value()) return 1;

    const auto frontier_opt = requireFreshAddressIndex(workdir);
    if (!frontier_opt.has_value()) return 1;
    const auto frontier = *frontier_opt;

    std::ifstream in(addressIndexEventsPath(workdir));
    if (!in) {
        std::cerr << "could not open address index events file\n";
        return 1;
    }
    std::vector<std::string> events;
    std::string line;
    while (std::getline(in, line)) {
        const auto parsed = parseAddressIndexEventLine(line);
        if (!parsed.has_value() || parsed->address != *address) continue;
        const auto confirmations = frontier >= parsed->integer ? frontier - parsed->integer + 1 : 0;
        std::ostringstream event;
        event << "TX_EVENT"
              << " integer=" << parsed->integer
              << " height=" << parsed->height
              << " kind=" << parsed->kind
              << " confirmations=" << confirmations
              << " direction=" << parsed->direction
              << " tx_hash=" << parsed->tx_hash
              << " version=" << parsed->version
              << " nonce=" << parsed->nonce
              << " prime=" << parsed->prime
              << " amount_micro_units=" << parsed->amount_micro_units
              << " amount_denominator=" << parsed->amount_denominator
              << " sender=" << parsed->sender
              << " receiver=" << parsed->receiver;
        events.push_back(event.str());
    }

    std::cout << "WALLET_HISTORY " << workdir
              << " wallet=" << wallet_path
              << " address=" << *address
              << " events=" << events.size() << "\n";
    const auto start = last == 0 || last >= events.size()
        ? std::size_t{0}
        : events.size() - static_cast<std::size_t>(last);
    for (std::size_t i = start; i < events.size(); ++i) {
        std::cout << events[i] << "\n";
    }
    return 0;
}

// Fast address-report: same idea as wallet-history-workdir, but for a raw
// address rather than a local wallet file. Only the transaction-event half
// of the report comes from the index -- current holdings/balance are
// ledger state, not a log of past events, so they still require
// SequentialNode::load() regardless. This still cuts the work in half:
// the index removes the second, otherwise-redundant full decode-and-verify
// pass over every record that the original implementation did purely to
// build the event list.
int addressReportWorkdir(int argc, char** argv) {
    if (argc != 4 && argc != 6) return 1;
    const std::string workdir = argv[2];
    const primechain::Address address = argv[3];
    std::uint64_t last = 0;
    if (argc == 6) {
        if (std::string(argv[4]) != "--last") return 1;
        last = static_cast<std::uint64_t>(std::stoull(argv[5]));
    }

    const auto frontier_opt = requireFreshAddressIndex(workdir);
    if (!frontier_opt.has_value()) return 1;
    const auto frontier = *frontier_opt;

    std::string error;
    primechain::node::SequentialNode node(chainPath(workdir));
    if (!node.load(error)) {
        std::cerr << "address_report_error: " << error << "\n";
        return 1;
    }
    const auto holdings = node.holdingsForAddress(address);
    std::uint64_t balance = 0;
    for (const auto& holding : holdings) balance += holding.second;

    std::ifstream in(addressIndexEventsPath(workdir));
    if (!in) {
        std::cerr << "could not open address index events file\n";
        return 1;
    }
    std::vector<std::string> events;
    std::set<std::string> matching_hashes;
    AddressReportTotals totals;
    std::string line;
    while (std::getline(in, line)) {
        const auto parsed = parseAddressIndexEventLine(line);
        if (!parsed.has_value() || parsed->address != address) continue;
        matching_hashes.insert(parsed->tx_hash);
        if (parsed->amount_denominator == 1) {
            if (parsed->direction == "sent") totals.sent_micro_units += parsed->amount_micro_units;
            if (parsed->direction == "received") totals.received_micro_units += parsed->amount_micro_units;
            if (parsed->direction == "fee-paid") totals.fee_micro_units += parsed->amount_micro_units;
        }
        const auto confirmations = frontier >= parsed->integer ? frontier - parsed->integer + 1 : 0;
        std::ostringstream event;
        event << "ADDRESS_TX"
              << " integer=" << parsed->integer
              << " height=" << parsed->height
              << " kind=" << parsed->kind
              << " confirmations=" << confirmations
              << " direction=" << parsed->direction
              << " tx_hash=" << parsed->tx_hash
              << " version=" << parsed->version
              << " nonce=" << parsed->nonce
              << " prime=" << parsed->prime
              << " amount_micro_units=" << parsed->amount_micro_units
              << " amount_denominator=" << parsed->amount_denominator
              << " sender=" << parsed->sender
              << " receiver=" << parsed->receiver;
        events.push_back(event.str());
    }
    totals.transactions = matching_hashes.size();

    std::cout << "ADDRESS_REPORT " << workdir
              << " address=" << address
              << " frontier=" << frontier
              << " holdings=" << holdings.size()
              << " total_micro_units=" << balance
              << " transactions=" << totals.transactions
              << " events=" << events.size()
              << " sent_micro_units=" << totals.sent_micro_units
              << " received_micro_units=" << totals.received_micro_units
              << " fee_micro_units=" << totals.fee_micro_units << "\n";
    for (const auto& holding : holdings) {
        std::cout << "ADDRESS_HOLDING address=" << address
                  << " prime=" << holding.first
                  << " micro_units=" << holding.second << "\n";
    }
    const auto start = last == 0 || last >= events.size()
        ? std::size_t{0}
        : events.size() - static_cast<std::size_t>(last);
    for (std::size_t i = start; i < events.size(); ++i) {
        std::cout << events[i] << "\n";
    }
    return 0;
}

// Fast tx lookup: a transaction's hash isn't derivable from its position,
// so finding which record holds it normally means a full linear scan.
// Every transaction has at least its sender indexed, so scanning the
// (much smaller, decode-free) address index for a matching tx_hash finds
// the record's integer directly; findByInteger then seeks straight to it
// via the existing .idx offset index to fetch the full record for
// display.
int transactionLookupWorkdir(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string workdir = argv[2];
    const std::string wanted_hash = argv[3];

    const auto frontier_opt = requireFreshAddressIndex(workdir);
    if (!frontier_opt.has_value()) return 1;
    const auto frontier = *frontier_opt;

    std::ifstream in(addressIndexEventsPath(workdir));
    if (!in) {
        std::cerr << "could not open address index events file\n";
        return 1;
    }
    std::optional<primechain::PrimeValue> found_integer;
    std::string line;
    while (std::getline(in, line)) {
        const auto parsed = parseAddressIndexEventLine(line);
        if (!parsed.has_value() || parsed->tx_hash != wanted_hash) continue;
        found_integer = parsed->integer;
        break;
    }
    if (!found_integer.has_value()) {
        std::cout << "TX_NOT_FOUND " << wanted_hash << " store=" << workdir << " frontier=" << frontier << "\n";
        return 1;
    }

    primechain::storage::RecordStore store(chainPath(workdir));
    std::string error;
    const auto stored = store.findByInteger(*found_integer, error);
    if (!error.empty() || !stored.has_value()) {
        std::cerr << "tx_lookup_error: could not reload indexed record " << *found_integer << "\n";
        return 1;
    }
    const auto transactions = storedTransactions(*stored, error);
    if (!transactions.has_value()) {
        std::cerr << "tx_lookup_error: " << error << "\n";
        return 1;
    }
    for (const auto& tx : *transactions) {
        const auto tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
        if (tx_hash != wanted_hash) continue;
        const auto confirmations = frontier >= stored->integer ? frontier - stored->integer + 1 : 0;
        std::cout << "TX_FOUND " << wanted_hash
                  << " store=" << workdir
                  << " integer=" << stored->integer
                  << " height=" << stored->height
                  << " kind=" << kindName(stored->kind)
                  << " frontier=" << frontier
                  << " confirmations=" << confirmations
                  << " version=" << tx.version
                  << " nonce=" << tx.nonce
                  << " sender=" << tx.sender_address << "\n";
        printTransactionDetails(tx, "TX");
        return 0;
    }
    std::cerr << "tx_lookup_error: indexed record " << *found_integer
              << " does not contain tx_hash " << wanted_hash << "\n";
    return 1;
}

int indexStatus(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    const std::string path = compositeProofIndexPath(workdir);
    if (!pathExists(path)) {
        std::cout << "INDEX_MISSING " << workdir << " path=" << path << "\n";
        return 0;
    }
    StoreProofIndex index;
    std::map<std::string, std::string> metadata;
    std::string error;
    if (!loadProofIndexFile(path, index, metadata, error)) {
        std::cout << "INDEX_INVALID " << workdir << " error=" << error << "\n";
        return 1;
    }
    std::cout << "INDEX_STATUS " << workdir
              << " frontier=" << metadata["chain_frontier"]
              << " height=" << metadata["chain_height"]
              << " proofs=" << index.size()
              << " path=" << path << "\n";
    return 0;
}

bool loadWorkdirProofIndex(const std::string& workdir, StoreProofIndex& index) {
    std::map<std::string, std::string> metadata;
    std::string error;
    if (!loadProofIndexFile(compositeProofIndexPath(workdir), index, metadata, error)) {
        std::cerr << "could not load proof index: " << error << "\n";
        return false;
    }
    return true;
}

int factorWorkdir(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string workdir = argv[2];
    const primechain::PrimeValue n = std::stoull(argv[3]);
    StoreProofIndex index;
    if (!loadWorkdirProofIndex(workdir, index)) return 1;
    const auto factorization = primechain::math::factorizeFromProofIndex(n, index);
    if (!factorization.has_value()) {
        std::cerr << "factorization unavailable for " << n << "\n";
        return 1;
    }
    printFactorization(*factorization);
    return 0;
}

int prattWorkdir(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string workdir = argv[2];
    const primechain::PrimeValue p = std::stoull(argv[3]);
    StoreProofIndex index;
    if (!loadWorkdirProofIndex(workdir, index)) return 1;
    const auto proof = primechain::math::makePrattProof(p, index);
    if (!proof.has_value()) {
        std::cerr << "Pratt proof unavailable for " << p << "\n";
        return 1;
    }
    printPrattProof(*proof);
    return 0;
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

std::optional<std::pair<PeerConfig, StatusLine>> chooseFreshestSyncPeer(
    const char* argv0,
    const std::string& workdir,
    const PeerConfig& configured_peer) {
    std::vector<PeerConfig> peers{configured_peer};
    auto known = loadValidatorEndpointsFromStore(chainPath(workdir));
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(known.begin(), known.end(), rng);
    auto already_added = [&peers](const PeerConfig& candidate) {
        return std::any_of(peers.begin(), peers.end(), [&candidate](const PeerConfig& peer) {
            return peer.host == candidate.host && peer.port == candidate.port;
        });
    };
    for (const auto& endpoint : known) {
        if (peers.size() >= 5) break;
        if (!already_added(endpoint)) peers.push_back(endpoint);
    }

    std::optional<std::pair<PeerConfig, StatusLine>> best;
    for (const auto& candidate : peers) {
        auto status = queryPeerStatus(argv0, candidate);
        if (!status.has_value()) continue;
        if (!best.has_value() || status->frontier > best->second.frontier) {
            best = std::make_pair(candidate, *status);
        }
    }
    return best;
}

int syncWorkdir(const char* argv0, const std::string& workdir, const PeerConfig& peer) {
    if (!ensureWorkdirLayout(workdir)) return 1;
    const auto local = loadLocalStatus(chainPath(workdir));
    const auto remote_peer = chooseFreshestSyncPeer(argv0, workdir, peer);
    if (!remote_peer.has_value()) return 1;
    const auto& sync_peer = remote_peer->first;
    const auto& remote = remote_peer->second;
    const primechain::PrimeValue start = local.has_genesis ? local.frontier + 1 : 2;
    if (remote.frontier < start) {
        std::cout << "SYNC_UP_TO_DATE " << local.frontier << "\n";
        return 0;
    }
    const int rc = runTool(argv0, "primechain-sync-download", {
        sync_peer.host,
        std::to_string(sync_peer.port),
        std::to_string(start),
        std::to_string(remote.frontier),
        chainPath(workdir),
    });
    if (rc != 0) return rc;
    std::cout << "SYNCED " << start << " " << remote.frontier << "\n";
    return 0;
}

std::optional<StatusLine> waitForFrontierAdvance(
    const char* argv0,
    const std::string& workdir,
    const PeerConfig& peer,
    primechain::PrimeValue previous_frontier,
    primechain::PrimeValue target) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (syncWorkdir(argv0, workdir, peer) != 0) continue;
        const auto local = loadLocalStatus(chainPath(workdir));
        if (local.frontier > previous_frontier || local.frontier >= target) {
            return local;
        }
    }
    return std::nullopt;
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
    const auto status = state.find("status");
    if (status != state.end()) std::cout << "JOB_STATUS " << status->second << "\n";
    const auto started_at = state.find("started_at");
    if (started_at != state.end()) std::cout << "JOB_STARTED_AT " << started_at->second << "\n";
    const auto updated_at = state.find("updated_at");
    if (updated_at != state.end()) std::cout << "JOB_UPDATED_AT " << updated_at->second << "\n";
    const auto last_synced = state.find("last_synced_frontier");
    if (last_synced != state.end()) std::cout << "JOB_LAST_SYNCED_FRONTIER " << last_synced->second << "\n";
    const auto result = state.find("last_result");
    if (result != state.end()) std::cout << "JOB_LAST_RESULT " << result->second << "\n";
    return 0;
}

int addMineJob(int argc, char** argv) {
    if (argc != 5 || std::string(argv[3]) != "--target") return 1;
    const std::string workdir = argv[2];
    const std::string target = argv[4];
    if (!ensureWorkdirLayout(workdir)) return 1;
    std::map<std::string, std::string> state;
    state["target"] = target;
    state["status"] = "pending";
    state["created_at"] = nowSeconds();
    state["updated_at"] = state["created_at"];
    state["last_result"] = "created";
    if (!writeMineState(workdir, state)) return 1;
    std::cout << "MINE_JOB_ADDED " << workdir << " target=" << target << "\n";
    return 0;
}

int clearJob(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string path = mineStatePath(argv[2]);
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        std::cerr << "could not remove " << path << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    std::cout << "JOB_CLEARED " << argv[2] << "\n";
    return 0;
}

int runJobs(const char* argv0, int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string workdir = argv[2];
    const auto peer = readPeerConfig(workdir);
    if (!peer.has_value()) {
        std::cerr << "no peer configured; run init-workdir with host and port first\n";
        return 1;
    }
    if (!ensureWorkdirLayout(workdir)) return 1;
    if (!ensureWallet(argv0, primeWalletPath(workdir)) || !ensureWallet(argv0, compositeWalletPath(workdir))) return 1;

    auto state = readKeyValueFile(mineStatePath(workdir));
    const auto target = stateTarget(state);
    if (!target.has_value()) {
        std::cerr << "no mine job configured; run add-mine-job first\n";
        return 1;
    }

    if (state.find("started_at") == state.end()) state["started_at"] = nowSeconds();
    state["status"] = "syncing";
    state["updated_at"] = nowSeconds();
    state["last_result"] = "syncing-before-mine";
    if (!writeMineState(workdir, state)) return 1;

    int rc = syncWorkdir(argv0, workdir, *peer);
    if (rc != 0) {
        state["status"] = "failed";
        state["updated_at"] = nowSeconds();
        state["last_result"] = "sync-before-mine-failed";
        writeMineState(workdir, state);
        return rc;
    }

    auto local = loadLocalStatus(chainPath(workdir));
    state["last_synced_frontier"] = std::to_string(local.frontier);
    if (local.frontier >= *target) {
        state["status"] = "complete";
        state["updated_at"] = nowSeconds();
        state["last_result"] = "already-at-target";
        if (!writeMineState(workdir, state)) return 1;
        std::cout << "JOB_COMPLETE target=" << *target << " frontier=" << local.frontier << "\n";
        return 0;
    }

    state["status"] = "running";
    state["updated_at"] = nowSeconds();
    state["last_result"] = "mining";
    if (!writeMineState(workdir, state)) return 1;

    int stagnant_attempts = 0;
    constexpr int kMaxStagnantAttempts = 30;
    while (true) {
        const auto before_mine = loadLocalStatus(chainPath(workdir));
        std::vector<std::string> miner_args{
            peer->host,
            std::to_string(peer->port),
            std::to_string(*target),
            "--prime-identity",
            primeWalletPath(workdir),
            "--composite-identity",
            compositeWalletPath(workdir),
            "--proof-store",
            chainPath(workdir),
            "--pending-composite",
            pendingCompositePath(workdir),
        };
        for (const auto& endpoint : loadValidatorEndpointsFromStore(chainPath(workdir))) {
            miner_args.push_back("--validator-endpoint");
            miner_args.push_back(endpoint.host);
            miner_args.push_back(std::to_string(endpoint.port));
        }
        rc = runTool(argv0, "primechain-frontier-miner", miner_args);
        if (rc == 0) break;

        state["status"] = "syncing";
        state["updated_at"] = nowSeconds();
        state["last_result"] = "syncing-after-stale-miner";
        if (!writeMineState(workdir, state)) return 1;
        const int sync_rc = syncWorkdir(argv0, workdir, *peer);
        local = loadLocalStatus(chainPath(workdir));
        state["last_synced_frontier"] = std::to_string(local.frontier);
        if (sync_rc == 0 && local.frontier <= before_mine.frontier) {
            state["status"] = "syncing";
            state["updated_at"] = nowSeconds();
            state["last_result"] = "waiting-for-race-winner";
            if (!writeMineState(workdir, state)) return 1;
            std::cout << "WAITING_FOR_RACE_WINNER frontier=" << before_mine.frontier
                      << " target=" << *target << "\n";
            auto advanced = waitForFrontierAdvance(
                argv0, workdir, *peer, before_mine.frontier, *target);
            if (!advanced.has_value()) {
                state["last_result"] = "retrying-local-miner-after-race-wait";
                state["updated_at"] = nowSeconds();
                if (!writeMineState(workdir, state)) return 1;
                std::cout << "RACE_WAIT_TIMEOUT_RETRY frontier=" << before_mine.frontier
                          << " target=" << *target << "\n";
            }
            if (advanced.has_value()) {
                local = *advanced;
                state["last_synced_frontier"] = std::to_string(local.frontier);
            }
        }
        if (sync_rc != 0 || local.frontier <= before_mine.frontier) {
            ++stagnant_attempts;
            if (stagnant_attempts >= kMaxStagnantAttempts) {
                state["status"] = "failed";
                state["updated_at"] = nowSeconds();
                state["last_result"] = sync_rc != 0 ? "sync-after-miner-failed" : "miner-failed";
                writeMineState(workdir, state);
                return rc;
            }
            state["status"] = "running";
            state["updated_at"] = nowSeconds();
            state["last_result"] = sync_rc != 0 ? "retrying-after-sync-failure" : "retrying-after-stalled-race";
            if (!writeMineState(workdir, state)) return 1;
            std::cout << "RETRYING_LOCAL_MINER attempt=" << stagnant_attempts
                      << " frontier=" << local.frontier
                      << " target=" << *target << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        stagnant_attempts = 0;
        if (local.frontier >= *target) {
            state["status"] = "complete";
            state["updated_at"] = nowSeconds();
            state["last_result"] = "complete-after-stale-miner";
            if (!writeMineState(workdir, state)) return 1;
            std::cout << "JOB_COMPLETE target=" << *target << " frontier=" << local.frontier << "\n";
            return 0;
        }
        state["status"] = "running";
        state["updated_at"] = nowSeconds();
        state["last_result"] = "continuing-after-race-progress";
        if (!writeMineState(workdir, state)) return 1;
    }

    state["status"] = "syncing";
    state["updated_at"] = nowSeconds();
    state["last_result"] = "syncing-after-mine";
    if (!writeMineState(workdir, state)) return 1;

    rc = syncWorkdir(argv0, workdir, *peer);
    local = loadLocalStatus(chainPath(workdir));
    state["last_synced_frontier"] = std::to_string(local.frontier);
    state["updated_at"] = nowSeconds();
    if (rc != 0) {
        state["status"] = "failed";
        state["last_result"] = "sync-after-mine-failed";
        writeMineState(workdir, state);
        return rc;
    }
    if (local.frontier >= *target) {
        state["status"] = "complete";
        state["last_result"] = "complete";
    } else {
        state["status"] = "pending";
        state["last_result"] = "frontier-below-target";
    }
    if (!writeMineState(workdir, state)) return 1;
    std::cout << (local.frontier >= *target ? "JOB_COMPLETE" : "JOB_PENDING")
              << " target=" << *target << " frontier=" << local.frontier << "\n";
    return local.frontier >= *target ? 0 : 1;
}

int mineJob(const char* argv0, int argc, char** argv) {
    if (argc != 5 || std::string(argv[3]) != "--target") return 1;
    const int added = addMineJob(argc, argv);
    if (added != 0) return added;
    return runJobs(argv0, 3, argv);
}

std::vector<std::string> tail(int argc, char** argv, int first) {
    std::vector<std::string> out;
    for (int i = first; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void printVersion() {
    std::cout << "VERSION"
              << " name=" << primechain::version::kName
              << " version=" << primechain::version::kVersion
              << " git_commit=" << primechain::version::kGitCommit
              << " build_time=" << primechain::version::kBuildTimestamp
              << " protocol=" << primechain::version::kProtocolVersion
              << " network=" << primechain::version::kNetworkVersion
              << "\n";
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " version\n"
              << "  " << argv0 << " init-workdir <workdir> [host port]\n"
              << "  " << argv0 << " sync-peer <workdir> [host port]\n"
              << "  " << argv0 << " job-status <workdir>\n"
              << "  " << argv0 << " add-mine-job <workdir> --target <integer>\n"
              << "  " << argv0 << " run-jobs <workdir>\n"
              << "  " << argv0 << " clear-job <workdir>\n"
              << "  " << argv0 << " mine-job <workdir> --target <integer>\n"
              << "  " << argv0 << " balances <workdir>\n"
              << "  " << argv0 << " rewards <workdir>\n"
              << "  " << argv0 << " reward-history <workdir> [--last count]\n"
              << "  " << argv0 << " board-report <record-store> --from <integer> --to <integer>\n"
              << "  " << argv0 << " launch-report <record-store>\n"
              << "  " << argv0 << " validator-reputation <record-store> <address>\n"
              << "  " << argv0 << " validator-eligibility <record-store> <address> --reserve <micro-units|auto> --observed <ok> --total <count>\n"
              << "  " << argv0 << " validator-reserve <record-store> <validator-address>\n"
              << "  " << argv0 << " validator-registry <record-store>\n"
              << "  " << argv0 << " validator-endpoints <record-store>\n"
              << "  " << argv0 << " economic-policy <record-store>\n"
              << "  " << argv0 << " fee-pool <record-store> [epoch]\n"
              << "  " << argv0 << " validator-reward-pool <record-store> [epoch]\n"
              << "  " << argv0 << " validator-reward-distribution-status <record-store> [interval-primes]\n"
              << "  " << argv0 << " fee-distribution-status <record-store> [interval-records]\n"
              << "  " << argv0 << " update-indexes <workdir>\n"
              << "  " << argv0 << " index-status <workdir>\n"
              << "  " << argv0 << " update-address-index <workdir>\n"
              << "  " << argv0 << " address-index-status <workdir>\n"
              << "  " << argv0 << " wallet-history-workdir <workdir> <wallet-file> [--last count]\n"
              << "  " << argv0 << " address-report-workdir <workdir> <address> [--last count]\n"
              << "  " << argv0 << " tx-workdir <workdir> <tx-hash>\n"
              << "  " << argv0 << " factor-workdir <workdir> <n>\n"
              << "  " << argv0 << " pratt-workdir <workdir> <prime>\n"
              << "  " << argv0 << " status <host> <port>\n"
              << "  " << argv0 << " query <host> <port> <command...>\n"
              << "  " << argv0 << " sync <host> <port> <start> <end> <output-store>\n"
              << "  " << argv0 << " inspect <record-store> [integer]\n"
              << "  " << argv0 << " inspect <record-store> --range <start> <end>\n"
              << "  " << argv0 << " decode-record <record-store> <integer>\n"
              << "  " << argv0 << " record <record-store> <integer>\n"
              << "  " << argv0 << " latest-records <record-store> [--last count]\n"
              << "  " << argv0 << " new-miner <wallet-file>\n"
              << "  " << argv0 << " address <wallet-file>\n"
              << "  " << argv0 << " balance <record-store> <wallet-file>\n"
              << "  " << argv0 << " wallet-history <record-store> <wallet-file> [--last count]\n"
              << "  " << argv0 << " wallet-pending <host> <port> <wallet-file>\n"
              << "  " << argv0 << " address-report <record-store> <address> [--last count]\n"
              << "  " << argv0 << " tx <record-store> <tx-hash>\n"
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
    if (command == "version") {
        if (argc != 2) { printUsage(argv[0]); return 1; }
        printVersion();
        return 0;
    }
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
    if (command == "add-mine-job") {
        if (argc != 5) { printUsage(argv[0]); return 1; }
        return addMineJob(argc, argv);
    }
    if (command == "run-jobs") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return runJobs(argv[0], argc, argv);
    }
    if (command == "clear-job") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return clearJob(argc, argv);
    }
    if (command == "mine-job") {
        if (argc != 5) { printUsage(argv[0]); return 1; }
        return mineJob(argv[0], argc, argv);
    }
    if (command == "balances") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return balancesWorkdir(argc, argv);
    }
    if (command == "rewards") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return rewardsWorkdir(argc, argv);
    }
    if (command == "reward-history") {
        if (argc != 3 && argc != 5) { printUsage(argv[0]); return 1; }
        return rewardHistoryWorkdir(argc, argv);
    }
    if (command == "board-report") {
        if (argc != 7) { printUsage(argv[0]); return 1; }
        return boardReport(argc, argv);
    }
    if (command == "launch-report") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return launchReport(argc, argv);
    }
    if (command == "validator-reputation") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return validatorReputation(argc, argv);
    }
    if (command == "validator-eligibility") {
        if (argc != 10) { printUsage(argv[0]); return 1; }
        return validatorEligibility(argc, argv);
    }
    if (command == "validator-reserve") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return validatorReserve(argc, argv);
    }
    if (command == "validator-registry") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return validatorRegistry(argc, argv);
    }
    if (command == "validator-endpoints") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return validatorEndpoints(argc, argv);
    }
    if (command == "economic-policy") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return economicPolicy(argc, argv);
    }
    if (command == "fee-pool") {
        if (argc != 3 && argc != 4) { printUsage(argv[0]); return 1; }
        return feePool(argc, argv);
    }
    if (command == "validator-reward-pool") {
        if (argc != 3 && argc != 4) { printUsage(argv[0]); return 1; }
        return validatorRewardPool(argc, argv);
    }
    if (command == "validator-reward-distribution-status") {
        if (argc != 3 && argc != 4) { printUsage(argv[0]); return 1; }
        return validatorRewardDistributionStatus(argc, argv);
    }
    if (command == "fee-distribution-status") {
        if (argc != 3 && argc != 4) { printUsage(argv[0]); return 1; }
        return feeDistributionStatus(argc, argv);
    }
    if (command == "update-indexes") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return updateIndexes(argc, argv);
    }
    if (command == "index-status") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return indexStatus(argc, argv);
    }
    if (command == "update-address-index") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return updateAddressIndex(argc, argv);
    }
    if (command == "address-index-status") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return addressIndexStatus(argc, argv);
    }
    if (command == "wallet-history-workdir") {
        if (argc != 4 && argc != 6) { printUsage(argv[0]); return 1; }
        return walletHistoryWorkdir(argc, argv);
    }
    if (command == "address-report-workdir") {
        if (argc != 4 && argc != 6) { printUsage(argv[0]); return 1; }
        return addressReportWorkdir(argc, argv);
    }
    if (command == "tx-workdir") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return transactionLookupWorkdir(argc, argv);
    }
    if (command == "factor-workdir") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return factorWorkdir(argc, argv);
    }
    if (command == "pratt-workdir") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return prattWorkdir(argc, argv);
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
    if (command == "decode-record") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return decodeRecord(argc, argv);
    }
    if (command == "record") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return recordExplorerLookup(argc, argv);
    }
    if (command == "latest-records") {
        if (argc != 3 && argc != 5) { printUsage(argv[0]); return 1; }
        return latestRecordsExplorer(argc, argv);
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
    if (command == "wallet-history") {
        if (argc != 4 && argc != 6) { printUsage(argv[0]); return 1; }
        return walletHistory(argc, argv);
    }
    if (command == "wallet-pending") {
        if (argc != 5) { printUsage(argv[0]); return 1; }
        return walletPending(argv[0], argc, argv);
    }
    if (command == "address-report") {
        if (argc != 4 && argc != 6) { printUsage(argv[0]); return 1; }
        return addressReport(argc, argv);
    }
    if (command == "tx") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return transactionLookup(argc, argv);
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
