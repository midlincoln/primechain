#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
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
#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"
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

std::optional<primechain::Address> loadMinerAddress(const std::string& path) {
    primechain::wallet::MinerIdentity identity;
    std::string error;
    if (!primechain::wallet::loadMinerIdentity(path, identity, error)) {
        std::cerr << "could not load miner wallet " << path << ": " << error << "\n";
        return std::nullopt;
    }
    return identity.address;
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
            if (record->proof.provider_address == *composite_address) summary.fee_micro_units += transactionFees(record->transactions);
            pending_composite_providers.push_back(record->proof.provider_address);
            continue;
        }

        const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!record.has_value()) {
            std::cerr << "could not decode prime record: " << error << "\n";
            return 1;
        }
        if (record->proof.provider_address == *prime_address) summary.fee_micro_units += transactionFees(record->transactions);
        if (record->proof.provider_address == *prime_address) ++summary.prime_records;

        if (pending_composite_providers.empty()) {
            if (record->proof.provider_address == *prime_address) {
                summary.prime_micro_units += primechain::node::kAssetMicroUnits;
            }
        } else {
            constexpr std::uint64_t prime_reward = primechain::node::kAssetMicroUnits / 2;
            const std::uint64_t composite_pool = primechain::node::kAssetMicroUnits - prime_reward;
            const std::uint64_t per_composite = composite_pool / pending_composite_providers.size();
            const std::uint64_t remainder = composite_pool % pending_composite_providers.size();
            if (record->proof.provider_address == *prime_address) {
                summary.prime_micro_units += prime_reward + remainder;
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
        if (pending.empty()) {
            if (record->proof.provider_address == *prime_address) {
                std::ostringstream event;
                event << "REWARD prime integer=" << record->integer
                      << " amount=" << primechain::node::kAssetMicroUnits
                      << " role=prime-miner record_height=" << record->height;
                events.push_back(event.str());
            }
        } else {
            constexpr std::uint64_t prime_reward = primechain::node::kAssetMicroUnits / 2;
            const std::uint64_t composite_pool = primechain::node::kAssetMicroUnits - prime_reward;
            const std::uint64_t per_composite = composite_pool / pending.size();
            const std::uint64_t remainder = composite_pool % pending.size();
            if (record->proof.provider_address == *prime_address) {
                std::ostringstream event;
                event << "REWARD prime integer=" << record->integer
                      << " amount=" << (prime_reward + remainder)
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

std::optional<StatusLine> waitForFrontierAdvance(
    const char* argv0,
    const std::string& workdir,
    const PeerConfig& peer,
    primechain::PrimeValue previous_frontier,
    primechain::PrimeValue target) {
    for (int attempt = 0; attempt < 8; ++attempt) {
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

    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto before_mine = loadLocalStatus(chainPath(workdir));
        rc = runTool(argv0, "primechain-frontier-miner", {
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
        });
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
            const auto advanced = waitForFrontierAdvance(
                argv0, workdir, *peer, before_mine.frontier, *target);
            if (advanced.has_value()) {
                local = *advanced;
                state["last_synced_frontier"] = std::to_string(local.frontier);
            }
        }
        if (sync_rc != 0 || local.frontier <= before_mine.frontier) {
            state["status"] = "failed";
            state["updated_at"] = nowSeconds();
            state["last_result"] = sync_rc != 0 ? "sync-after-miner-failed" : "miner-failed";
            writeMineState(workdir, state);
            return rc;
        }
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
        state["last_result"] = "retrying-after-stale-miner";
        if (!writeMineState(workdir, state)) return 1;
    }
    if (rc != 0) {
        state["status"] = "failed";
        state["updated_at"] = nowSeconds();
        state["last_result"] = "miner-failed";
        writeMineState(workdir, state);
        return rc;
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

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
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
              << "  " << argv0 << " update-indexes <workdir>\n"
              << "  " << argv0 << " index-status <workdir>\n"
              << "  " << argv0 << " factor-workdir <workdir> <n>\n"
              << "  " << argv0 << " pratt-workdir <workdir> <prime>\n"
              << "  " << argv0 << " status <host> <port>\n"
              << "  " << argv0 << " query <host> <port> <command...>\n"
              << "  " << argv0 << " sync <host> <port> <start> <end> <output-store>\n"
              << "  " << argv0 << " inspect <record-store> [integer]\n"
              << "  " << argv0 << " inspect <record-store> --range <start> <end>\n"
              << "  " << argv0 << " decode-record <record-store> <integer>\n"
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
    if (command == "update-indexes") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return updateIndexes(argc, argv);
    }
    if (command == "index-status") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return indexStatus(argc, argv);
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
