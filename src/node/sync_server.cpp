#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

constexpr int kDefaultPort = 18889;
constexpr const char* kDefaultStorePath = "data/sequential-chain.dat";
volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

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

std::optional<Socket> listenOnPort(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << "\n";
        return std::nullopt;
    }

    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << "\n";
        close(fd);
        return std::nullopt;
    }
    if (listen(fd, 16) != 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << "\n";
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
        if (line.size() > 8192) {
            return std::nullopt;
        }
        line.push_back(ch);
    }
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

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHex[(byte >> 4) & 0x0f]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
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
        return {};
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

std::string recordLine(const primechain::storage::StoredRecord& record) {
    std::ostringstream out;
    out << "RECORD " << record.integer << " "
        << record.height << " "
        << kindName(record.kind) << " "
        << primechain::crypto::toHex(record.record_hash) << " "
        << record.payload.size() << " "
        << bytesToHex(record.payload)
        << "\n";
    return out.str();
}

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

primechain::CompositeProof toLegacyCompositeProof(const primechain::protocol::CompositeProofV0& proof) {
    primechain::CompositeProof out;
    out.m = proof.g;
    out.d = proof.d;
    out.e = proof.e;
    out.provider_address = proof.provider_address;
    out.signature = proof.signature;
    return out;
}

bool loadCompositeProofIndex(
    const primechain::storage::RecordStore& store,
    MapProofIndex& proofs,
    std::string& error) {
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        return false;
    }
    for (const auto& stored : records) {
        if (stored.kind != primechain::storage::StoredRecordKind::Composite) {
            continue;
        }
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) {
            return false;
        }
        const auto proof = toLegacyCompositeProof(decoded->proof);
        if (!primechain::math::verifyCompositeProof(proof)) {
            error = "stored composite proof is invalid";
            return false;
        }
        proofs.add(proof);
    }
    return true;
}

primechain::protocol::PrimeRecordV0 makePrimeRecord(
    const primechain::node::SequentialNodeStatus& status,
    primechain::PrimeValue p,
    const primechain::math::PrattProof& proof,
    const std::string& prime_miner_address) {
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
    record.proof.provider_address = prime_miner_address;
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

class SyncServer {
public:
    explicit SyncServer(std::string store_path)
        : store_path_(std::move(store_path)),
          store_(store_path_) {}

    void handleClient(int fd) {
        while (const auto line = readLine(fd)) {
            if (*line == "GET_STATUS") {
                sendStatus(fd);
                continue;
            }
            if (line->rfind("GET_RECORD ", 0) == 0) {
                sendRecord(fd, *line);
                continue;
            }
            if (line->rfind("GET_RECORD_RANGE ", 0) == 0) {
                sendRecordRange(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_TX ", 0) == 0) {
                submitTx(fd, *line);
                continue;
            }
            if (*line == "GET_MEMPOOL") {
                sendMempool(fd);
                continue;
            }
            if (line->rfind("ACK_MEMPOOL ", 0) == 0) {
                ackMempool(fd, *line);
                continue;
            }
            if (line->rfind("ADVANCE_TO ", 0) == 0) {
                advanceTo(fd, *line);
                continue;
            }
            if (line->rfind("GET_BALANCE ", 0) == 0) {
                sendBalance(fd, *line);
                continue;
            }
            writeAll(fd, "ERROR unknown command\n");
        }
    }

private:
    void sendStatus(int fd) const {
        std::string error;
        const auto records = store_.loadAll(error);
        if (!error.empty()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        std::uint64_t prime_records = 0;
        std::uint64_t composite_records = 0;
        for (const auto& record : records) {
            if (record.kind == primechain::storage::StoredRecordKind::Prime) {
                ++prime_records;
            } else {
                ++composite_records;
            }
        }

        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        const auto& status = node.status();
        std::ostringstream out;
        out << "STATUS "
            << records.size() << " "
            << prime_records << " "
            << composite_records << " "
            << (status.has_genesis ? 1 : 0) << " "
            << status.height << " "
            << status.frontier_integer << " "
            << primechain::crypto::toHex(status.latest_record_hash) << "\n";
        writeAll(fd, out.str());
    }

    void sendRecord(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue integer = 0;
        in >> command >> integer;
        if (!in) {
            writeAll(fd, "ERROR invalid GET_RECORD\n");
            return;
        }

        std::string error;
        const auto record = store_.findByInteger(integer, error);
        if (!error.empty()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (!record.has_value()) {
            writeAll(fd, "NOT_FOUND " + std::to_string(integer) + "\n");
            return;
        }
        writeAll(fd, recordLine(*record));
    }

    void sendRecordRange(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue start = 0;
        primechain::PrimeValue end = 0;
        in >> command >> start >> end;
        if (!in) {
            writeAll(fd, "ERROR invalid GET_RECORD_RANGE\n");
            return;
        }

        std::string error;
        const auto records = store_.findRange(start, end, error);
        if (!error.empty()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        std::ostringstream header;
        header << "RECORD_RANGE " << start << " " << end << " " << records.size() << "\n";
        writeAll(fd, header.str());
        for (const auto& record : records) {
            writeAll(fd, recordLine(record));
        }
        writeAll(fd, "END_RECORD_RANGE\n");
    }

    void submitTx(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        std::string tx_hex;
        in >> command >> tx_hex;
        if (!in || tx_hex.empty()) {
            writeAll(fd, "ERROR invalid SUBMIT_TX\n");
            return;
        }

        const auto tx_bytes = hexToBytes(tx_hex);
        if (tx_bytes.empty()) {
            writeAll(fd, "ERROR invalid tx hex\n");
            return;
        }

        std::string error;
        const auto tx = primechain::protocol::deserializeTransaction(tx_bytes, error);
        if (!tx.has_value()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (!primechain::protocol::verifyDevelopmentTransactionSignature(*tx)) {
            writeAll(fd, "ERROR invalid transaction signature\n");
            return;
        }

        const auto hash = primechain::protocol::transactionHash(*tx);
        for (const auto& existing : mempool_) {
            if (primechain::protocol::transactionHash(existing) == hash) {
                writeAll(fd, "TX_DUPLICATE " + primechain::crypto::toHex(hash) + "\n");
                return;
            }
        }

        mempool_.push_back(*tx);
        writeAll(fd, "TX_ACCEPTED " + primechain::crypto::toHex(hash) + "\n");
    }

    void sendMempool(int fd) const {
        std::ostringstream header;
        header << "MEMPOOL " << mempool_.size() << "\n";
        writeAll(fd, header.str());
        for (const auto& tx : mempool_) {
            const auto bytes = primechain::protocol::serializeTransaction(tx, true);
            writeAll(fd, "TX "
                + primechain::crypto::toHex(primechain::protocol::transactionHash(tx))
                + " "
                + std::to_string(bytes.size())
                + " "
                + bytesToHex(bytes)
                + "\n");
        }
        writeAll(fd, "END_MEMPOOL\n");
    }

    void ackMempool(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        in >> command;

        std::vector<std::string> hashes;
        std::string hash;
        while (in >> hash) {
            hashes.push_back(hash);
        }
        if (hashes.empty()) {
            writeAll(fd, "ERROR ACK_MEMPOOL requires at least one hash\n");
            return;
        }

        std::uint64_t removed = 0;
        std::vector<primechain::protocol::TransactionV0> retained;
        retained.reserve(mempool_.size());
        for (const auto& tx : mempool_) {
            const std::string tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
            bool acknowledged = false;
            for (const auto& requested_hash : hashes) {
                if (requested_hash == tx_hash) {
                    acknowledged = true;
                    break;
                }
            }
            if (acknowledged) {
                ++removed;
            } else {
                retained.push_back(tx);
            }
        }
        mempool_ = std::move(retained);

        std::ostringstream out;
        out << "MEMPOOL_ACKED " << removed << " " << mempool_.size() << "\n";
        writeAll(fd, out.str());
    }

    void removeMempoolTransactions(const std::vector<primechain::protocol::TransactionV0>& included) {
        if (included.empty()) {
            return;
        }

        std::vector<primechain::protocol::TransactionV0> retained;
        retained.reserve(mempool_.size());
        for (const auto& tx : mempool_) {
            const auto tx_hash = primechain::protocol::transactionHash(tx);
            bool acknowledged = false;
            for (const auto& included_tx : included) {
                if (primechain::protocol::transactionHash(included_tx) == tx_hash) {
                    acknowledged = true;
                    break;
                }
            }
            if (!acknowledged) {
                retained.push_back(tx);
            }
        }
        mempool_ = std::move(retained);
    }

    void advanceTo(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue limit = 0;
        std::string prime_miner_address;
        std::string composite_miner_address;
        primechain::PrimeValue mempool_target_integer = 0;
        in >> command >> limit >> prime_miner_address >> composite_miner_address >> mempool_target_integer;
        if (!in ||
            limit < 2 ||
            mempool_target_integer < 3 ||
            mempool_target_integer > limit ||
            !primechain::protocol::isDevelopmentAddress(prime_miner_address) ||
            !primechain::protocol::isDevelopmentAddress(composite_miner_address)) {
            writeAll(fd, "ERROR invalid ADVANCE_TO; expected ADVANCE_TO limit prime_miner composite_miner mempool_target_integer\n");
            return;
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (!node.status().has_genesis) {
            error.clear();
            if (!node.initializeGenesis(error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
        }

        MapProofIndex proofs;
        error.clear();
        if (!loadCompositeProofIndex(store_, proofs, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        if (limit <= node.status().frontier_integer) {
            std::ostringstream out;
            out << "ADVANCED 0 included_txs=0 frontier=" << node.status().frontier_integer << "\n";
            writeAll(fd, out.str());
            return;
        }

        std::uint64_t appended = 0;
        std::vector<primechain::protocol::TransactionV0> included_transactions;
        for (primechain::PrimeValue n = node.status().frontier_integer + 1; n <= limit; ++n) {
            const bool include_mempool_here = (n == mempool_target_integer);
            if (primechain::math::isPrime(n)) {
                const auto proof = primechain::math::makePrattProof(n, proofs);
                if (!proof.has_value() || !primechain::math::verifyPrattProof(*proof)) {
                    writeAll(fd, "ERROR could not create valid Pratt proof for " + std::to_string(n) + "\n");
                    return;
                }
                auto record = makePrimeRecord(node.status(), n, *proof, prime_miner_address);
                if (include_mempool_here) {
                    record.transactions = mempool_;
                    included_transactions = mempool_;
                    primechain::protocol::applyDevelopmentFinalization(record);
                }
                error.clear();
                if (!node.appendPrime(record, error)) {
                    writeAll(fd, "ERROR could not append prime record for " + std::to_string(n) + ": " + error + "\n");
                    return;
                }
                ++appended;
                continue;
            }

            const auto proof = primechain::math::makeCompositeProof(n, composite_miner_address);
            if (!proof.has_value() || !primechain::math::verifyCompositeProof(*proof)) {
                writeAll(fd, "ERROR could not create valid composite proof for " + std::to_string(n) + "\n");
                return;
            }
            auto record = makeCompositeRecord(node.status(), *proof, composite_miner_address);
            if (include_mempool_here) {
                record.transactions = mempool_;
                included_transactions = mempool_;
                primechain::protocol::applyDevelopmentFinalization(record);
            }
            error.clear();
            if (!node.appendComposite(record, error)) {
                writeAll(fd, "ERROR could not append composite record for " + std::to_string(n) + ": " + error + "\n");
                return;
            }
            proofs.add(*proof);
            ++appended;
        }

        primechain::node::SequentialNode reloaded(store_path_);
        error.clear();
        if (!reloaded.load(error)) {
            writeAll(fd, "ERROR reload failed after advance: " + error + "\n");
            return;
        }
        if (!reloaded.status().has_genesis || reloaded.status().frontier_integer != limit) {
            writeAll(fd, "ERROR reload frontier mismatch after advance\n");
            return;
        }

        removeMempoolTransactions(included_transactions);

        std::ostringstream out;
        out << "ADVANCED " << appended
            << " included_txs=" << included_transactions.size()
            << " frontier=" << reloaded.status().frontier_integer
            << "\n";
        writeAll(fd, out.str());
    }

    void sendBalance(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command;
        std::string address;
        in >> command >> address;
        if (!in || !primechain::protocol::isDevelopmentAddress(address)) {
            writeAll(fd, "ERROR invalid GET_BALANCE\n");
            return;
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        const auto holdings = node.holdingsForAddress(address);
        std::ostringstream out;
        out << "BALANCE " << address << " " << holdings.size() << "\n";
        for (const auto& holding : holdings) {
            out << "HOLDING " << holding.first << " " << holding.second << "\n";
        }
        out << "END_BALANCE\n";
        writeAll(fd, out.str());
    }

    std::string store_path_;
    primechain::storage::RecordStore store_;
    std::vector<primechain::protocol::TransactionV0> mempool_;
};

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [port] [record_store_path]\n"
              << "example:\n"
              << "  " << argv0 << " 18889 ./data/sequential-500.dat\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const int port = argc > 1 ? std::stoi(argv[1]) : kDefaultPort;
    const std::string store_path = argc > 2 ? argv[2] : kDefaultStorePath;

    auto server = listenOnPort(port);
    if (!server.has_value()) {
        return 1;
    }

    SyncServer sync_server(store_path);
    std::cout << "Primechain sync server listening on 127.0.0.1:" << port << "\n";
    std::cout << "record store: " << store_path << "\n";

    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server->fd(), &read_fds);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        const int ready = select(server->fd() + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "select failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (ready == 0) {
            continue;
        }

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = accept(server->fd(), reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            break;
        }

        Socket client(client_fd);
        sync_server.handleClient(client.fd());
    }

    std::cout << "sync server stopped\n";
    return 0;
}
