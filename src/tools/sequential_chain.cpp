#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/types.hpp"
#include "primechain/wallet/miner_identity.hpp"

namespace {

constexpr const char* kDefaultOutputPath = "data/sequential-chain.log";
constexpr const char* kDefaultRecordStorePath = "data/sequential-chain.dat";
constexpr const char* kDefaultPrimeMinerAddress = "pcdev1_prime_miner";
constexpr const char* kDefaultCompositeMinerAddress = "pcdev1_composite_miner";

struct Options {
    primechain::PrimeValue limit{500};
    std::string output_path{kDefaultOutputPath};
    std::string record_store_path{kDefaultRecordStorePath};
    std::string prime_miner_address{kDefaultPrimeMinerAddress};
    std::string composite_miner_address{kDefaultCompositeMinerAddress};
    std::vector<primechain::Address> validator_set;
    std::vector<std::string> validator_identity_paths;
    bool has_transfer{false};
    std::string transfer_sender_wallet;
    std::string transfer_receiver_address;
    primechain::PrimeValue transfer_prime{0};
    std::uint64_t transfer_amount{0};
    primechain::PrimeValue transfer_target_integer{0};
    bool has_mempool{false};
    std::string mempool_host;
    int mempool_port{0};
    primechain::PrimeValue mempool_target_integer{0};
};

struct DevWallet {
    std::string address;
    std::vector<std::uint8_t> public_key;
};

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

class Socket {
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int fd() const { return fd_; }

private:
    int fd_{-1};
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

std::vector<std::uint8_t> hexToBytes(const std::string& hex) {
    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + ch - 'a';
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + ch - 'A';
        }
        return -1;
    };

    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) {
        return out;
    }
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = value(hex[i]);
        const int low = value(hex[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

bool loadDevWallet(const std::string& path, DevWallet& wallet) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "address") {
            wallet.address = value;
        } else if (key == "public_key") {
            wallet.public_key = hexToBytes(value);
        }
    }
    return !wallet.address.empty() &&
           !wallet.public_key.empty() &&
           wallet.address == primechain::protocol::developmentAddressFromPublicKey(wallet.public_key);
}

std::optional<Socket> connectToServer(const std::string& host, int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << "\n";
        return std::nullopt;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid IPv4 address: " << host << "\n";
        close(fd);
        return std::nullopt;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << "\n";
        close(fd);
        return std::nullopt;
    }
    return Socket(fd);
}

bool writeAll(int fd, const std::string& message) {
    const char* cursor = message.data();
    std::size_t remaining = message.size();
    while (remaining > 0) {
        const ssize_t sent = send(fd, cursor, remaining, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::optional<std::string> readLine(int fd) {
    std::string line;
    char ch = '\0';
    while (true) {
        const ssize_t received = recv(fd, &ch, 1, 0);
        if (received == 0) {
            return std::nullopt;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (ch == '\n') {
            return line;
        }
        if (line.size() > 1'000'000) {
            return std::nullopt;
        }
        line.push_back(ch);
    }
}

std::optional<std::vector<primechain::protocol::TransactionV0>> fetchMempool(
    const std::string& host,
    int port) {
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        return std::nullopt;
    }
    if (!writeAll(socket->fd(), "GET_MEMPOOL\n")) {
        std::cerr << "could not request mempool\n";
        return std::nullopt;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto header = readLine(socket->fd());
    if (!header.has_value()) {
        std::cerr << "missing mempool header\n";
        return std::nullopt;
    }
    std::istringstream header_in(*header);
    std::string tag;
    std::uint64_t expected_count = 0;
    header_in >> tag >> expected_count;
    if (!header_in || tag != "MEMPOOL") {
        std::cerr << "invalid mempool header: " << *header << "\n";
        return std::nullopt;
    }

    std::vector<primechain::protocol::TransactionV0> transactions;
    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_MEMPOOL") {
            break;
        }
        std::istringstream in(*line);
        std::string tx_tag;
        std::string tx_hash_hex;
        std::uint64_t payload_size = 0;
        std::string payload_hex;
        in >> tx_tag >> tx_hash_hex >> payload_size >> payload_hex;
        if (!in || tx_tag != "TX") {
            std::cerr << "invalid mempool tx line\n";
            return std::nullopt;
        }
        const auto payload = hexToBytes(payload_hex);
        if (payload.size() != payload_size) {
            std::cerr << "mempool tx payload size mismatch\n";
            return std::nullopt;
        }
        std::string error;
        const auto tx = primechain::protocol::deserializeTransaction(payload, error);
        if (!tx.has_value()) {
            std::cerr << "could not deserialize mempool tx: " << error << "\n";
            return std::nullopt;
        }
        std::string signature_error;
        if (!primechain::protocol::verifyDevelopmentTransactionSignature(*tx) &&
            !primechain::protocol::verifyAuthenticatedTransactionSignature(*tx, signature_error)) {
            std::cerr << "invalid mempool tx signature: " << signature_error << "\n";
            return std::nullopt;
        }
        transactions.push_back(*tx);
    }
    if (transactions.size() != expected_count) {
        std::cerr << "mempool count mismatch\n";
        return std::nullopt;
    }
    return transactions;
}

bool acknowledgeMempool(
    const std::string& host,
    int port,
    const std::vector<primechain::protocol::TransactionV0>& transactions) {
    if (transactions.empty()) {
        return true;
    }
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        return false;
    }

    std::ostringstream command;
    command << "ACK_MEMPOOL";
    for (const auto& tx : transactions) {
        command << " " << primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
    }
    command << "\n";

    if (!writeAll(socket->fd(), command.str())) {
        std::cerr << "could not acknowledge mempool transactions\n";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto response = readLine(socket->fd());
    if (!response.has_value() || response->rfind("MEMPOOL_ACKED ", 0) != 0) {
        std::cerr << "invalid mempool acknowledgement response\n";
        return false;
    }
    return true;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [limit] [text_log_path] [record_store_path] [--prime-miner address] [--composite-miner address]\n"
              << "       [--transfer sender.wallet receiver_address prime amount target_integer]\n"
              << "       [--mempool host port target_integer] [--validator-set addr1 addr2 addr3]\n"
              << "       [--validator-identities validator1.wallet validator2.wallet]\n"
              << "example:\n"
              << "  " << argv0 << " 500 ./data/sequential-500.log ./data/sequential-500.dat --prime-miner pcdev1_...\n";
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    int index = 1;
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.limit = std::stoull(argv[index++]);
    }
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.output_path = argv[index++];
    }
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.record_store_path = argv[index++];
    }
    while (index < argc) {
        const std::string flag = argv[index++];
        if (flag == "--validator-set") {
            if (index + 2 >= argc) return std::nullopt;
            options.validator_set = {argv[index], argv[index + 1], argv[index + 2]};
            index += 3;
            continue;
        }
        if (flag == "--validator-identities") {
            if (index + 1 >= argc) return std::nullopt;
            options.validator_identity_paths = {argv[index], argv[index + 1]};
            index += 2;
            continue;
        }
        if (index >= argc) {
            return std::nullopt;
        }
        const std::string value = argv[index++];
        if (flag == "--prime-miner") {
            options.prime_miner_address = value;
        } else if (flag == "--composite-miner") {
            options.composite_miner_address = value;
        } else if (flag == "--transfer") {
            if (index + 3 >= argc) {
                return std::nullopt;
            }
            options.has_transfer = true;
            options.transfer_sender_wallet = value;
            options.transfer_receiver_address = argv[index++];
            options.transfer_prime = std::stoull(argv[index++]);
            options.transfer_amount = std::stoull(argv[index++]);
            options.transfer_target_integer = std::stoull(argv[index++]);
        } else if (flag == "--mempool") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            options.has_mempool = true;
            options.mempool_host = value;
            options.mempool_port = std::stoi(argv[index++]);
            options.mempool_target_integer = std::stoull(argv[index++]);
        } else {
            return std::nullopt;
        }
    }
    return options;
}

primechain::protocol::TransactionV0 makeTransferTransaction(
    const DevWallet& sender,
    const std::string& receiver_address,
    primechain::PrimeValue prime,
    std::uint64_t amount) {
    primechain::protocol::TransactionV0 tx;
    tx.version = 0;
    tx.inputs.push_back({prime, {amount, 1}});
    tx.outputs.push_back({prime, {amount, 1}, receiver_address});
    tx.fee = {prime, {0, 1}};
    tx.nonce = 1;
    tx.sender_address = sender.address;
    tx.sender_public_key = sender.public_key;
    tx.signature = primechain::protocol::developmentTransactionSignature(tx);
    return tx;
}

primechain::protocol::PrimeRecordV0 makePrimeRecord(
    const primechain::node::SequentialNodeStatus& status,
    primechain::PrimeValue p,
    const primechain::math::PrattProof& proof,
    const std::string& prime_miner_address,
    const primechain::wallet::MinerIdentity* signing_identity = nullptr) {
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
    record.proof.provider_address = p == 2 ? "pcdev1_genesis" :
        (signing_identity == nullptr ? prime_miner_address : signing_identity->address);
    if (p != 2 && signing_identity != nullptr) {
        std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors;
        for (const auto& factor : proof.factors_of_p_minus_1.factors) {
            factors.push_back({factor.prime, factor.exponent});
        }
        std::string signing_error;
        const auto signature = primechain::crypto::signProtocolMessage(
            signing_identity->private_key,
            primechain::crypto::primeProofSigningPayload(
                record.previous_record_hash, proof.p, proof.witness, factors,
                signing_identity->address),
            signing_error);
        if (signature.has_value()) {
            record.proof.signature = primechain::crypto::packPrimeProofAuthentication(
                signing_identity->public_key, *signature);
        }
    }
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

primechain::protocol::CompositeRecordV0 makeCompositeRecord(
    const primechain::node::SequentialNodeStatus& status,
    const primechain::CompositeProof& proof,
    const std::string& composite_miner_address) {
    primechain::protocol::CompositeRecordV0 record;
    record.version = 0;
    record.height = status.height + 1;
    record.previous_record_hash = status.latest_record_hash;
    record.integer = proof.m;
    record.proof.g = proof.m;
    record.proof.d = proof.d;
    record.proof.e = proof.e;
    record.proof.provider_address = composite_miner_address;
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

template <typename Record>
bool applySignedFinalization(
    Record& record,
    const std::vector<primechain::wallet::MinerIdentity>& validators,
    std::string& error) {
    primechain::protocol::updateTransactionBatch(record);
    record.finalized_by.rule = "fixed-2-of-3-mldsa65-v2";
    record.finalized_by.votes.clear();
    const auto candidate_hash = primechain::protocol::candidateRecordHash(record);
    for (const auto& validator : validators) {
        auto vote = primechain::protocol::makeSignedValidatorVote(
            validator.address, validator.public_key, validator.private_key,
            candidate_hash, 1, error);
        if (vote.signature.empty()) return false;
        record.finalized_by.votes.push_back(std::move(vote));
    }
    std::sort(record.finalized_by.votes.begin(), record.finalized_by.votes.end(),
        [](const auto& left, const auto& right) {
            return left.validator_address < right.validator_address;
        });
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const auto parsed = parseOptions(argc, argv);
    if (!parsed.has_value()) {
        printUsage(argv[0]);
        return 1;
    }
    const Options options = *parsed;
    if (options.limit < 2 ||
        (!options.validator_set.empty() &&
            (options.validator_set.size() != 3 ||
             !std::all_of(options.validator_set.begin(), options.validator_set.end(),
                 [](const primechain::Address& address) {
                     return primechain::crypto::isProtocolSignatureAddress(address);
                 }))) ||
        !primechain::protocol::isProtocolAddress(options.prime_miner_address) ||
        !primechain::protocol::isProtocolAddress(options.composite_miner_address) ||
        (options.has_transfer &&
            (!primechain::protocol::isProtocolAddress(options.transfer_receiver_address) ||
             options.transfer_amount == 0 ||
             options.transfer_prime < 2 ||
             options.transfer_target_integer < 3 ||
             options.transfer_target_integer > options.limit)) ||
        (options.has_mempool &&
            (options.mempool_port <= 0 ||
             options.mempool_target_integer < 3 ||
             options.mempool_target_integer > options.limit))) {
        printUsage(argv[0]);
        return 1;
    }
    if (options.validator_set.empty() != options.validator_identity_paths.empty()) {
        printUsage(argv[0]);
        return 1;
    }
    if (!options.validator_set.empty() && options.validator_identity_paths.size() != 2) {
        printUsage(argv[0]);
        return 1;
    }
    if (!options.validator_set.empty()) {
        std::set<primechain::Address> unique(
            options.validator_set.begin(), options.validator_set.end());
        if (unique.size() != 3) {
            printUsage(argv[0]);
            return 1;
        }
    }
    std::vector<primechain::wallet::MinerIdentity> finalization_validators;
    for (const auto& path : options.validator_identity_paths) {
        primechain::wallet::MinerIdentity identity;
        std::string identity_error;
        if (!primechain::wallet::loadMinerIdentity(path, identity, identity_error) ||
            std::find(options.validator_set.begin(), options.validator_set.end(), identity.address) ==
                options.validator_set.end()) {
            std::cerr << "could not load configured validator identity: " << identity_error << "\n";
            return 1;
        }
        finalization_validators.push_back(std::move(identity));
    }
    DevWallet transfer_sender;
    if (options.has_transfer && !loadDevWallet(options.transfer_sender_wallet, transfer_sender)) {
        std::cerr << "could not load transfer sender wallet\n";
        return 1;
    }
    std::vector<primechain::protocol::TransactionV0> mempool_transactions;
    if (options.has_mempool) {
        const auto fetched = fetchMempool(options.mempool_host, options.mempool_port);
        if (!fetched.has_value()) {
            return 1;
        }
        mempool_transactions = *fetched;
    }
    if (!ensureParentDataDir(options.output_path) || !ensureParentDataDir(options.record_store_path)) {
        std::cerr << "could not create parent data directory\n";
        return 1;
    }

    std::ofstream out(options.output_path, std::ios::trunc);
    if (!out) {
        std::cerr << "could not open " << options.output_path << " for writing\n";
        return 1;
    }

    MapProofIndex proofs;
    std::string error;
    primechain::node::SequentialNode node(options.record_store_path);
    if (!node.load(error)) {
        std::cerr << "could not load record store: " << error << "\n";
        return 1;
    }
    if (node.status().has_genesis) {
        std::cerr << "record store already contains records; use a fresh store path for this generator\n";
        return 1;
    }
    if (!node.initializeGenesis(options.validator_set, error)) {
        std::cerr << "could not initialize genesis: " << error << "\n";
        return 1;
    }

    std::uint64_t prime_count = 0;
    std::uint64_t composite_count = 0;

    for (primechain::PrimeValue n = 2; n <= options.limit; ++n) {
        if (primechain::math::isPrime(n)) {
            const auto proof = primechain::math::makePrattProof(n, proofs);
            if (!proof.has_value() || !primechain::math::verifyPrattProof(*proof)) {
                std::cerr << "could not create valid Pratt proof for " << n << "\n";
                return 1;
            }

            if (n != 2) {
                auto record = makePrimeRecord(
                    node.status(), n, *proof, options.prime_miner_address,
                    finalization_validators.empty() ? nullptr : &finalization_validators.front());
                if (options.has_transfer && n == options.transfer_target_integer) {
                    record.transactions.push_back(makeTransferTransaction(
                        transfer_sender,
                        options.transfer_receiver_address,
                        options.transfer_prime,
                        options.transfer_amount));
                    primechain::protocol::applyDevelopmentFinalization(record);
                }
                if (options.has_mempool && n == options.mempool_target_integer) {
                    record.transactions.insert(
                        record.transactions.end(),
                        mempool_transactions.begin(),
                        mempool_transactions.end());
                    primechain::protocol::applyDevelopmentFinalization(record);
                }
                if (!finalization_validators.empty() &&
                    !applySignedFinalization(record, finalization_validators, error)) {
                    std::cerr << "could not finalize prime record for " << n << ": " << error << "\n";
                    return 1;
                }
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

        const auto proof = primechain::math::makeCompositeProof(n, options.composite_miner_address);
        if (!proof.has_value() || !primechain::math::verifyCompositeProof(*proof)) {
            std::cerr << "could not create valid composite proof for " << n << "\n";
            return 1;
        }

        auto record = makeCompositeRecord(node.status(), *proof, options.composite_miner_address);
        if (options.has_transfer && n == options.transfer_target_integer) {
            record.transactions.push_back(makeTransferTransaction(
                transfer_sender,
                options.transfer_receiver_address,
                options.transfer_prime,
                options.transfer_amount));
            primechain::protocol::applyDevelopmentFinalization(record);
        }
        if (options.has_mempool && n == options.mempool_target_integer) {
            record.transactions.insert(
                record.transactions.end(),
                mempool_transactions.begin(),
                mempool_transactions.end());
            primechain::protocol::applyDevelopmentFinalization(record);
        }
        if (!finalization_validators.empty() &&
            !applySignedFinalization(record, finalization_validators, error)) {
            std::cerr << "could not finalize composite record for " << n << ": " << error << "\n";
            return 1;
        }
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
        std::cerr << "failed while writing " << options.output_path << "\n";
        return 1;
    }

    primechain::node::SequentialNode reloaded(options.record_store_path);
    error.clear();
    if (!reloaded.load(error)) {
        std::cerr << "could not reload record store: " << error << "\n";
        return 1;
    }
    if (!reloaded.status().has_genesis || reloaded.status().frontier_integer != options.limit) {
        std::cerr << "reloaded frontier mismatch\n";
        return 1;
    }
    if (options.has_mempool && !acknowledgeMempool(options.mempool_host, options.mempool_port, mempool_transactions)) {
        return 1;
    }

    std::cout << "sequential chain complete\n";
    std::cout << "output_path: " << options.output_path << "\n";
    std::cout << "record_store_path: " << options.record_store_path << "\n";
    std::cout << "limit: " << options.limit << "\n";
    std::cout << "prime_miner_address: " << options.prime_miner_address << "\n";
    std::cout << "composite_miner_address: " << options.composite_miner_address << "\n";
    if (options.has_transfer) {
        std::cout << "transfer_sender: " << transfer_sender.address << "\n";
        std::cout << "transfer_receiver: " << options.transfer_receiver_address << "\n";
        std::cout << "transfer_prime: " << options.transfer_prime << "\n";
        std::cout << "transfer_amount: " << options.transfer_amount << "\n";
        std::cout << "transfer_target_integer: " << options.transfer_target_integer << "\n";
    }
    if (options.has_mempool) {
        std::cout << "mempool_source: " << options.mempool_host << ":" << options.mempool_port << "\n";
        std::cout << "mempool_transactions: " << mempool_transactions.size() << "\n";
        std::cout << "mempool_target_integer: " << options.mempool_target_integer << "\n";
    }
    std::cout << "prime_records: " << prime_count << "\n";
    std::cout << "composite_records: " << composite_count << "\n";
    return 0;
}
