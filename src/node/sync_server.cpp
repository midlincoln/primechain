#include <cerrno>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
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
constexpr std::size_t kMaxLineBytes = 8192;
constexpr std::uint64_t kMaxRecordRangeCount = 10000;
constexpr std::size_t kMaxMempoolTransactions = 1000;
constexpr std::size_t kMaxKnownPeers = 32;
constexpr int kPeerConnectTimeoutMs = 1500;
constexpr int kPeerReadTimeoutMs = 3000;
constexpr std::size_t kMaxCommandsPerConnection = 128;
constexpr std::size_t kMaxWriteCommandsPerConnection = 16;
volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

struct PeerEndpoint {
    std::string host;
    int port{0};
};

bool samePeer(const PeerEndpoint& left, const PeerEndpoint& right) {
    return left.host == right.host && left.port == right.port;
}

bool validPeerEndpoint(const PeerEndpoint& peer) {
    if (peer.port <= 0 || peer.port > 65535) {
        return false;
    }
    sockaddr_in addr{};
    return inet_pton(AF_INET, peer.host.c_str(), &addr.sin_addr) == 1;
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

std::optional<Socket> listenOnPort(const std::string& bind_address, int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << "\n";
        return std::nullopt;
    }

    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid bind IPv4 address: " << bind_address << "\n";
        close(fd);
        return std::nullopt;
    }
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

bool setSocketTimeouts(int fd, int timeout_ms) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
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

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        std::cerr << "fcntl failed: " << std::strerror(errno) << "\n";
        close(fd);
        return std::nullopt;
    }

    const int result = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result != 0 && errno != EINPROGRESS) {
        std::cerr << "connect failed: " << std::strerror(errno) << "\n";
        close(fd);
        return std::nullopt;
    }
    if (result != 0) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);

        timeval timeout{};
        timeout.tv_sec = kPeerConnectTimeoutMs / 1000;
        timeout.tv_usec = (kPeerConnectTimeoutMs % 1000) * 1000;
        const int ready = select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
        if (ready <= 0) {
            std::cerr << "connect timeout\n";
            close(fd);
            return std::nullopt;
        }

        int socket_error = 0;
        socklen_t socket_error_len = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) != 0 ||
            socket_error != 0) {
            std::cerr << "connect failed: "
                      << std::strerror(socket_error != 0 ? socket_error : errno) << "\n";
            close(fd);
            return std::nullopt;
        }
    }

    if (fcntl(fd, F_SETFL, flags) != 0) {
        std::cerr << "fcntl restore failed: " << std::strerror(errno) << "\n";
        close(fd);
        return std::nullopt;
    }
    setSocketTimeouts(fd, kPeerReadTimeoutMs);
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
        if (line.size() >= kMaxLineBytes) {
            return std::nullopt;
        }
        line.push_back(ch);
    }
}

bool isWriteCommand(const std::string& line) {
    return line.rfind("ADD_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_TX ", 0) == 0 ||
           line.rfind("SUBMIT_COMPOSITE ", 0) == 0 ||
           line.rfind("SUBMIT_RECORD ", 0) == 0 ||
           line.rfind("ACK_MEMPOOL ", 0) == 0 ||
           line.rfind("ADVANCE_TO ", 0) == 0;
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

std::optional<primechain::Hash256> parseHash(const std::string& hex) {
    const auto bytes = hexToBytes(hex);
    if (bytes.size() != 32) {
        return std::nullopt;
    }
    primechain::Hash256 hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = bytes[i];
    }
    return hash;
}

std::optional<primechain::storage::StoredRecordKind> parseKind(const std::string& value) {
    if (value == "COMPOSITE") {
        return primechain::storage::StoredRecordKind::Composite;
    }
    if (value == "PRIME") {
        return primechain::storage::StoredRecordKind::Prime;
    }
    return std::nullopt;
}

std::optional<primechain::storage::StoredRecord> parseRecordLine(const std::string& line) {
    std::istringstream in(line);
    std::string tag;
    std::string kind_text;
    std::string hash_hex;
    std::string payload_hex;
    std::uint64_t payload_size = 0;
    primechain::storage::StoredRecord record;

    in >> tag >> record.integer >> record.height >> kind_text >> hash_hex >> payload_size >> payload_hex;
    if (!in || tag != "RECORD") {
        return std::nullopt;
    }
    const auto kind = parseKind(kind_text);
    const auto hash = parseHash(hash_hex);
    const auto payload = hexToBytes(payload_hex);
    if (!kind.has_value() || !hash.has_value() || payload.empty()) {
        return std::nullopt;
    }
    if (payload.size() != payload_size || primechain::crypto::devHash256(payload) != *hash) {
        return std::nullopt;
    }

    record.kind = *kind;
    record.record_hash = *hash;
    record.payload = payload;
    return record;
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

std::string submitRecordLine(const primechain::storage::StoredRecord& record) {
    std::ostringstream out;
    out << "SUBMIT_RECORD " << record.integer << " "
        << record.height << " "
        << kindName(record.kind) << " "
        << primechain::crypto::toHex(record.record_hash) << " "
        << record.payload.size() << " "
        << bytesToHex(record.payload)
        << "\n";
    return out.str();
}

std::optional<primechain::storage::StoredRecord> parseSubmitRecordLine(const std::string& line) {
    if (line.rfind("SUBMIT_RECORD ", 0) != 0) {
        return std::nullopt;
    }
    return parseRecordLine("RECORD " + line.substr(std::string("SUBMIT_RECORD ").size()));
}

bool hashLess(const primechain::Hash256& left, const primechain::Hash256& right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

std::optional<primechain::Hash256> previousRecordHash(
    const primechain::storage::StoredRecord& stored,
    std::string& error) {
    if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        return decoded->previous_record_hash;
    }

    const auto decoded = primechain::protocol::deserializePrimeRecord(stored.payload, error);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return decoded->previous_record_hash;
}

bool appendStoredRecord(
    primechain::node::SequentialNode& node,
    const primechain::storage::StoredRecord& stored,
    std::string& error) {
    if (!node.status().has_genesis) {
        const auto expected_genesis =
            primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0());
        if (stored.height == expected_genesis.height &&
            stored.integer == expected_genesis.integer &&
            stored.kind == expected_genesis.kind &&
            stored.record_hash == expected_genesis.record_hash &&
            stored.payload == expected_genesis.payload) {
            return node.initializeGenesis(error);
        }
    }

    if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) {
            return false;
        }
        return node.appendComposite(*decoded, error);
    }

    const auto decoded = primechain::protocol::deserializePrimeRecord(stored.payload, error);
    if (!decoded.has_value()) {
        return false;
    }
    return node.appendPrime(*decoded, error);
}

bool copyFile(const std::string& source, const std::string& destination, std::string& error) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        error = "could not open source store for copy";
        return false;
    }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open temporary store for copy";
        return false;
    }
    out << in.rdbuf();
    if (!out) {
        error = "failed while copying temporary store";
        return false;
    }
    return true;
}

bool copyFileOrCreateEmpty(const std::string& source, const std::string& destination, std::string& error) {
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open temporary store for sync";
        return false;
    }
    if (!in) {
        return true;
    }
    out << in.rdbuf();
    if (!out) {
        error = "failed while copying temporary sync store";
        return false;
    }
    return true;
}

std::optional<primechain::storage::StoredRecord> validateTipReplacementCandidate(
    const std::string& store_path,
    const primechain::storage::StoredRecord& local_tip,
    const primechain::storage::StoredRecord& replacement,
    std::string& error) {
    const std::string temp_path = store_path + ".tipcheck." + std::to_string(getpid());
    if (!copyFile(store_path, temp_path, error)) {
        return std::nullopt;
    }

    primechain::storage::RecordStore temp_store(temp_path);
    if (!temp_store.replaceTip(local_tip.record_hash, replacement, error)) {
        std::remove(temp_path.c_str());
        return std::nullopt;
    }

    primechain::node::SequentialNode candidate(temp_path);
    if (!candidate.load(error)) {
        std::remove(temp_path.c_str());
        return std::nullopt;
    }
    if (!candidate.status().has_genesis ||
        candidate.status().frontier_integer != replacement.integer ||
        candidate.status().height != replacement.height ||
        candidate.status().latest_record_hash != replacement.record_hash) {
        error = "replacement replay did not converge to candidate tip";
        std::remove(temp_path.c_str());
        return std::nullopt;
    }

    std::remove(temp_path.c_str());
    return replacement;
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

struct PeerStatus {
    std::uint64_t record_count{0};
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    bool has_genesis{false};
    std::uint64_t height{0};
    primechain::PrimeValue frontier_integer{0};
    primechain::Hash256 latest_record_hash{};
};

std::optional<PeerStatus> requestPeerStatus(const std::string& host, int port, std::string& error) {
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return std::nullopt;
    }
    if (!writeAll(socket->fd(), "GET_STATUS\n")) {
        error = "could not request peer status";
        return std::nullopt;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto line = readLine(socket->fd());
    if (!line.has_value()) {
        error = "peer did not return status";
        return std::nullopt;
    }

    std::istringstream in(*line);
    std::string tag;
    std::uint64_t has_genesis = 0;
    std::string hash_hex;
    PeerStatus status;
    in >> tag
       >> status.record_count
       >> status.prime_records
       >> status.composite_records
       >> has_genesis
       >> status.height
       >> status.frontier_integer
       >> hash_hex;
    const auto hash = parseHash(hash_hex);
    if (!in || tag != "STATUS" || !hash.has_value()) {
        error = "invalid peer status response";
        return std::nullopt;
    }
    status.has_genesis = has_genesis != 0;
    status.latest_record_hash = *hash;
    return status;
}

std::vector<PeerEndpoint> requestPeerList(const std::string& host, int port, std::string& error) {
    std::vector<PeerEndpoint> peers;
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return {};
    }
    if (!writeAll(socket->fd(), "GET_PEERS\n")) {
        error = "could not request peer list";
        return {};
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto header = readLine(socket->fd());
    if (!header.has_value()) {
        error = "peer did not return peer list header";
        return {};
    }

    std::istringstream header_in(*header);
    std::string header_tag;
    std::uint64_t expected_count = 0;
    header_in >> header_tag >> expected_count;
    if (!header_in || header_tag != "PEERS") {
        error = "invalid peer list header";
        return {};
    }
    if (expected_count > kMaxKnownPeers) {
        error = "peer list too large";
        return {};
    }

    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_PEERS") {
            break;
        }
        std::istringstream in(*line);
        std::string tag;
        PeerEndpoint peer;
        in >> tag >> peer.host >> peer.port;
        if (!in || tag != "PEER" || !validPeerEndpoint(peer)) {
            error = "invalid peer list entry";
            return {};
        }
        peers.push_back(peer);
        if (peers.size() > expected_count || peers.size() > kMaxKnownPeers) {
            error = "peer list count mismatch";
            return {};
        }
    }
    if (peers.size() != expected_count) {
        error = "peer list count mismatch";
        return {};
    }
    return peers;
}

bool downloadRecordRange(
    const std::string& host,
    int port,
    primechain::PrimeValue start,
    primechain::PrimeValue end,
    const primechain::storage::RecordStore& output,
    std::string& error) {
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    std::ostringstream command;
    command << "GET_RECORD_RANGE " << start << " " << end << "\n";
    if (!writeAll(socket->fd(), command.str())) {
        error = "could not request peer record range";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto header = readLine(socket->fd());
    if (!header.has_value()) {
        error = "peer did not return range header";
        return false;
    }

    std::istringstream header_in(*header);
    std::string header_tag;
    primechain::PrimeValue response_start = 0;
    primechain::PrimeValue response_end = 0;
    std::uint64_t expected_count = 0;
    header_in >> header_tag >> response_start >> response_end >> expected_count;
    if (!header_in || header_tag != "RECORD_RANGE" || response_start != start || response_end != end) {
        error = "invalid peer range header";
        return false;
    }

    std::uint64_t downloaded = 0;
    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_RECORD_RANGE") {
            break;
        }
        const auto record = parseRecordLine(*line);
        if (!record.has_value()) {
            error = "invalid peer record line";
            return false;
        }
        const primechain::PrimeValue expected_integer = start + downloaded;
        const std::uint64_t expected_height = expected_integer - 2;
        if (record->integer != expected_integer || record->height != expected_height) {
            error = "peer record sequence mismatch";
            return false;
        }
        if (!output.append(*record, error)) {
            return false;
        }
        ++downloaded;
    }
    if (downloaded != expected_count) {
        error = "peer range count mismatch";
        return false;
    }
    return true;
}

bool submitTransactionToPeer(
    const PeerEndpoint& peer,
    const primechain::protocol::TransactionV0& tx,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    const auto bytes = primechain::protocol::serializeTransaction(tx, true);
    if (!writeAll(socket->fd(), "SUBMIT_TX " + bytesToHex(bytes) + "\n")) {
        error = "could not submit transaction to peer";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto response = readLine(socket->fd());
    if (!response.has_value()) {
        error = "peer did not return transaction response";
        return false;
    }
    if (response->rfind("TX_ACCEPTED ", 0) == 0 || response->rfind("TX_DUPLICATE ", 0) == 0) {
        return true;
    }
    error = "peer rejected transaction: " + *response;
    return false;
}

bool submitRecordToPeer(
    const PeerEndpoint& peer,
    const primechain::storage::StoredRecord& record,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    if (!writeAll(socket->fd(), submitRecordLine(record))) {
        error = "could not submit record to peer";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto response = readLine(socket->fd());
    if (!response.has_value()) {
        error = "peer did not return record response";
        return false;
    }
    if (response->rfind("RECORD_ACCEPTED ", 0) == 0 || response->rfind("RECORD_DUPLICATE ", 0) == 0) {
        return true;
    }
    error = "peer rejected record: " + *response;
    return false;
}

class SyncServer {
public:
    SyncServer(
        std::string store_path,
        std::vector<PeerEndpoint> peers,
        bool advance_enabled,
        bool ack_mempool_enabled)
        : store_path_(std::move(store_path)),
          advance_enabled_(advance_enabled),
          ack_mempool_enabled_(ack_mempool_enabled),
          store_(store_path_) {
        for (const auto& peer : peers) {
            addPeer(peer);
        }
    }

    void handleClient(int fd) {
        std::size_t command_count = 0;
        std::size_t write_command_count = 0;
        while (const auto line = readLine(fd)) {
            ++command_count;
            if (command_count > kMaxCommandsPerConnection) {
                writeAll(fd, "ERROR rate limit exceeded: too many commands on one connection\n");
                return;
            }
            if (isWriteCommand(*line)) {
                ++write_command_count;
                if (write_command_count > kMaxWriteCommandsPerConnection) {
                    writeAll(fd, "ERROR rate limit exceeded: too many write commands on one connection\n");
                    return;
                }
            }
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
            if (*line == "GET_PEERS") {
                sendPeers(fd);
                continue;
            }
            if (line->rfind("ADD_PEER ", 0) == 0) {
                addPeerCommand(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_TX ", 0) == 0) {
                submitTx(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_COMPOSITE ", 0) == 0) {
                submitComposite(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_RECORD ", 0) == 0) {
                submitRecord(fd, *line);
                continue;
            }
            if (*line == "GET_MEMPOOL") {
                sendMempool(fd);
                continue;
            }
            if (line->rfind("ACK_MEMPOOL ", 0) == 0) {
                if (!ack_mempool_enabled_) {
                    writeAll(fd, "ERROR ACK_MEMPOOL disabled; restart with --enable-ack-mempool\n");
                    continue;
                }
                ackMempool(fd, *line);
                continue;
            }
            if (line->rfind("ADVANCE_TO ", 0) == 0) {
                if (!advance_enabled_) {
                    writeAll(fd, "ERROR ADVANCE_TO disabled; restart with --enable-advance\n");
                    continue;
                }
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

    bool syncFromPeer(const std::string& host, int port, std::string& error) const {
        primechain::node::SequentialNode local(store_path_);
        if (!local.load(error)) {
            return false;
        }

        const auto peer_status = requestPeerStatus(host, port, error);
        if (!peer_status.has_value()) {
            return false;
        }
        if (!peer_status->has_genesis) {
            return true;
        }

        const primechain::PrimeValue local_frontier =
            local.status().has_genesis ? local.status().frontier_integer : 0;
        if (peer_status->frontier_integer <= local_frontier) {
            return true;
        }

        const primechain::PrimeValue start =
            local.status().has_genesis ? local.status().frontier_integer + 1 : 2;
        const std::string temp_path = store_path_ + ".sync." + std::to_string(getpid());
        std::remove(temp_path.c_str());
        if (!copyFileOrCreateEmpty(store_path_, temp_path, error)) {
            return false;
        }

        primechain::storage::RecordStore temp_store(temp_path);
        if (!downloadRecordRange(host, port, start, peer_status->frontier_integer, temp_store, error)) {
            std::remove(temp_path.c_str());
            return false;
        }

        primechain::node::SequentialNode reloaded(temp_path);
        if (!reloaded.load(error)) {
            std::remove(temp_path.c_str());
            return false;
        }
        if (!reloaded.status().has_genesis ||
            reloaded.status().frontier_integer != peer_status->frontier_integer) {
            error = "auto-sync replay frontier mismatch";
            std::remove(temp_path.c_str());
            return false;
        }

        if (!copyFile(temp_path, store_path_, error)) {
            std::remove(temp_path.c_str());
            return false;
        }
        std::remove(temp_path.c_str());
        return true;
    }

    bool syncFromKnownPeers(std::string& error) const {
        return syncFromPeers(peers_, error);
    }

    bool hasKnownPeers() const {
        return !peers_.empty();
    }

    bool syncFromPeers(const std::vector<PeerEndpoint>& peers, std::string& error) const {
        bool synced_any = false;
        for (const auto& peer : peers) {
            error.clear();
            if (syncFromPeer(peer.host, peer.port, error)) {
                synced_any = true;
                continue;
            }
            std::cerr << "peer sync warning from " << peer.host << ":" << peer.port
                      << ": " << error << "\n";
        }
        if (synced_any || peers.empty()) {
            error.clear();
            return true;
        }
        return false;
    }

    void discoverPeersFromKnown() {
        const auto snapshot = peers_;
        for (const auto& peer : snapshot) {
            std::string error;
            const auto discovered = requestPeerList(peer.host, peer.port, error);
            if (!error.empty()) {
                std::cerr << "peer discovery warning from " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
                continue;
            }
            for (const auto& discovered_peer : discovered) {
                addPeer(discovered_peer);
            }
        }
    }

private:
    bool addPeer(const PeerEndpoint& peer) {
        if (!validPeerEndpoint(peer)) {
            return false;
        }
        const auto found = std::find_if(peers_.begin(), peers_.end(), [&](const PeerEndpoint& existing) {
            return samePeer(existing, peer);
        });
        if (found != peers_.end()) {
            return true;
        }
        if (peers_.size() >= kMaxKnownPeers) {
            return false;
        }
        peers_.push_back(peer);
        return true;
    }

    void sendPeers(int fd) const {
        std::ostringstream out;
        out << "PEERS " << peers_.size() << "\n";
        for (const auto& peer : peers_) {
            out << "PEER " << peer.host << " " << peer.port << "\n";
        }
        out << "END_PEERS\n";
        writeAll(fd, out.str());
    }

    void addPeerCommand(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        PeerEndpoint peer;
        in >> command >> peer.host >> peer.port;
        if (!in || !validPeerEndpoint(peer)) {
            writeAll(fd, "ERROR invalid ADD_PEER\n");
            return;
        }

        const auto before = peers_.size();
        if (!addPeer(peer)) {
            writeAll(fd, "ERROR peer table full; max="
                + std::to_string(kMaxKnownPeers)
                + "\n");
            return;
        }
        if (peers_.size() == before) {
            writeAll(fd, "PEER_DUPLICATE " + peer.host + " " + std::to_string(peer.port) + "\n");
            return;
        }
        writeAll(fd, "PEER_ADDED " + peer.host + " " + std::to_string(peer.port) + "\n");
    }

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
        if (end < start) {
            writeAll(fd, "ERROR invalid GET_RECORD_RANGE: start greater than end\n");
            return;
        }
        if ((end - start + 1) > kMaxRecordRangeCount) {
            writeAll(fd, "ERROR GET_RECORD_RANGE too large; max="
                + std::to_string(kMaxRecordRangeCount)
                + "\n");
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
        if (mempool_.size() >= kMaxMempoolTransactions) {
            writeAll(fd, "ERROR mempool full; max="
                + std::to_string(kMaxMempoolTransactions)
                + "\n");
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
        propagateTransaction(*tx);
        writeAll(fd, "TX_ACCEPTED " + primechain::crypto::toHex(hash) + "\n");
    }

    void propagateTransaction(const primechain::protocol::TransactionV0& tx) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitTransactionToPeer(peer, tx, error)) {
                std::cerr << "mempool propagation warning to " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
            }
        }
    }

    void submitRecord(int fd, const std::string& line) {
        const auto submitted = parseSubmitRecordLine(line);
        if (!submitted.has_value()) {
            writeAll(fd, "ERROR invalid SUBMIT_RECORD\n");
            return;
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        if (submitted->integer <= node.status().frontier_integer) {
            handleExistingOrConflictingRecord(fd, *submitted, node.status().frontier_integer);
            return;
        }

        error.clear();
        if (!appendStoredRecord(node, *submitted, error)) {
            writeAll(fd, "ERROR could not append submitted record: " + error + "\n");
            return;
        }

        propagateRecord(*submitted);
        writeAll(fd, "RECORD_ACCEPTED " + primechain::crypto::toHex(submitted->record_hash) + "\n");
    }

    void handleExistingOrConflictingRecord(
        int fd,
        const primechain::storage::StoredRecord& submitted,
        primechain::PrimeValue frontier_integer) {
        std::string error;
        const auto existing = store_.findByInteger(submitted.integer, error);
        if (!error.empty()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (existing.has_value() && existing->record_hash == submitted.record_hash) {
            writeAll(fd, "RECORD_DUPLICATE " + primechain::crypto::toHex(submitted.record_hash) + "\n");
            return;
        }
        if (existing.has_value() && submitted.integer == frontier_integer) {
            error.clear();
            const auto submitted_previous = previousRecordHash(submitted, error);
            if (!submitted_previous.has_value()) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            error.clear();
            const auto existing_previous = previousRecordHash(*existing, error);
            if (!existing_previous.has_value()) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }

            if (*submitted_previous != *existing_previous) {
                writeAll(fd, "RECORD_CONFLICT_FORK "
                    + primechain::crypto::toHex(submitted.record_hash)
                    + " "
                    + primechain::crypto::toHex(existing->record_hash)
                    + "\n");
                return;
            }

            if (hashLess(submitted.record_hash, existing->record_hash)) {
                error.clear();
                const auto validated =
                    validateTipReplacementCandidate(store_path_, *existing, submitted, error);
                if (!validated.has_value()) {
                    writeAll(fd, "ERROR invalid better tip replacement: " + error + "\n");
                    return;
                }
                error.clear();
                if (!store_.replaceTip(existing->record_hash, *validated, error)) {
                    writeAll(fd, "ERROR could not replace tip: " + error + "\n");
                    return;
                }
                propagateRecord(*validated);
                writeAll(fd, "RECORD_REPLACED "
                    + primechain::crypto::toHex(validated->record_hash)
                    + " "
                    + primechain::crypto::toHex(existing->record_hash)
                    + "\n");
                return;
            }

            writeAll(fd, "RECORD_CONFLICT_WORSE "
                + primechain::crypto::toHex(submitted.record_hash)
                + " "
                + primechain::crypto::toHex(existing->record_hash)
                + "\n");
            return;
        }
        writeAll(fd, "ERROR conflicting historical record\n");
    }

    void submitComposite(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue g = 0;
        primechain::PrimeValue d = 0;
        primechain::PrimeValue e = 0;
        std::string provider_address;
        in >> command >> g >> d >> e >> provider_address;
        if (!in ||
            command != "SUBMIT_COMPOSITE" ||
            !primechain::protocol::isDevelopmentAddress(provider_address)) {
            writeAll(fd, "ERROR invalid SUBMIT_COMPOSITE; expected SUBMIT_COMPOSITE g d e provider_address\n");
            return;
        }

        primechain::CompositeProof proof;
        proof.m = g;
        proof.d = d;
        proof.e = e;
        proof.provider_address = provider_address;
        if (!primechain::math::verifyCompositeProof(proof)) {
            writeAll(fd, "ERROR invalid composite proof\n");
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
            propagateRecord(
                primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0()));
        }
        if (g == node.status().frontier_integer && node.status().frontier_integer > 2) {
            const auto existing = store_.findByInteger(g, error);
            if (!error.empty()) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            if (!existing.has_value()) {
                writeAll(fd, "ERROR current frontier record not found\n");
                return;
            }
            error.clear();
            const auto previous_hash = previousRecordHash(*existing, error);
            if (!previous_hash.has_value()) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }

            primechain::protocol::CompositeRecordV0 record;
            record.version = 0;
            record.height = node.status().height;
            record.previous_record_hash = *previous_hash;
            record.integer = proof.m;
            record.proof.g = proof.m;
            record.proof.d = proof.d;
            record.proof.e = proof.e;
            record.proof.provider_address = provider_address;
            primechain::protocol::applyDevelopmentFinalization(record);
            const auto stored = primechain::storage::makeStoredRecord(record);
            handleExistingOrConflictingRecord(fd, stored, node.status().frontier_integer);
            return;
        }

        if (g != node.status().frontier_integer + 1) {
            std::ostringstream out;
            out << "ERROR SUBMIT_COMPOSITE must extend frontier "
                << node.status().frontier_integer
                << " with integer "
                << (node.status().frontier_integer + 1)
                << "\n";
            writeAll(fd, out.str());
            return;
        }

        auto record = makeCompositeRecord(node.status(), proof, provider_address);
        error.clear();
        if (!node.appendComposite(record, error)) {
            writeAll(fd, "ERROR could not append composite record: " + error + "\n");
            return;
        }

        const auto stored = primechain::storage::makeStoredRecord(record);
        propagateRecord(stored);
        writeAll(fd, "COMPOSITE_ACCEPTED "
            + std::to_string(g)
            + " "
            + primechain::crypto::toHex(stored.record_hash)
            + "\n");
    }

    void propagateRecord(const primechain::storage::StoredRecord& record) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitRecordToPeer(peer, record, error)) {
                std::cerr << "record propagation warning to " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
            }
        }
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
        std::vector<primechain::storage::StoredRecord> appended_records;
        if (!node.status().has_genesis) {
            error.clear();
            if (!node.initializeGenesis(error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            appended_records.push_back(
                primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0()));
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
                appended_records.push_back(primechain::storage::makeStoredRecord(record));
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
            appended_records.push_back(primechain::storage::makeStoredRecord(record));
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
        for (const auto& record : appended_records) {
            propagateRecord(record);
        }

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
    std::vector<PeerEndpoint> peers_;
    bool advance_enabled_{false};
    bool ack_mempool_enabled_{false};
    primechain::storage::RecordStore store_;
    std::vector<primechain::protocol::TransactionV0> mempool_;
};

struct Options {
    int port{kDefaultPort};
    std::string bind_address{"127.0.0.1"};
    std::string store_path{kDefaultStorePath};
    std::vector<PeerEndpoint> peers;
    int sync_interval_seconds{0};
    bool enable_advance{false};
    bool enable_ack_mempool{false};
};

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    int index = 1;
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.port = std::stoi(argv[index++]);
    }
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.store_path = argv[index++];
    }
    while (index < argc) {
        const std::string flag = argv[index++];
        if (flag == "--bind") {
            if (index >= argc) {
                return std::nullopt;
            }
            options.bind_address = argv[index++];
            continue;
        }
        if (flag == "--peer") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            PeerEndpoint peer;
            peer.host = argv[index++];
            peer.port = std::stoi(argv[index++]);
            options.peers.push_back(peer);
            continue;
        }
        if (flag == "--sync-interval") {
            if (index >= argc) {
                return std::nullopt;
            }
            options.sync_interval_seconds = std::stoi(argv[index++]);
            if (options.sync_interval_seconds < 0) {
                return std::nullopt;
            }
            continue;
        }
        if (flag == "--enable-advance") {
            options.enable_advance = true;
            continue;
        }
        if (flag == "--enable-ack-mempool") {
            options.enable_ack_mempool = true;
            continue;
        }
        {
            return std::nullopt;
        }
    }
    return options;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [port] [record_store_path] [--bind address] [--peer host port] [--sync-interval seconds] [--enable-advance] [--enable-ack-mempool]\n"
              << "example:\n"
              << "  " << argv0 << " 18889 ./data/sequential-500.dat\n"
              << "  " << argv0 << " 18890 ./data/node-b.dat --peer 127.0.0.1 18889 --sync-interval 5\n"
              << "  " << argv0 << " 18889 ./data/public-node.dat --bind 0.0.0.0\n"
              << "  " << argv0 << " 18889 ./data/dev-node.dat --enable-advance --enable-ack-mempool\n";
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

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    auto server = listenOnPort(options.bind_address, options.port);
    if (!server.has_value()) {
        return 1;
    }

    SyncServer sync_server(
        options.store_path,
        options.peers,
        options.enable_advance,
        options.enable_ack_mempool);
    if (!options.peers.empty()) {
        std::string error;
        sync_server.discoverPeersFromKnown();
        if (!sync_server.syncFromKnownPeers(error)) {
            std::cerr << "peer sync failed: " << error << "\n";
            return 1;
        }
        std::cout << "peer sync complete from " << options.peers.size() << " configured peer(s)\n";
    }

    std::cout << "Primechain sync server listening on " << options.bind_address << ":" << options.port << "\n";
    std::cout << "record store: " << options.store_path << "\n";
    if (options.sync_interval_seconds > 0 && !options.peers.empty()) {
        std::cout << "continuous peer sync interval: " << options.sync_interval_seconds << "s\n";
    }
    if (options.enable_advance) {
        std::cout << "development command enabled: ADVANCE_TO\n";
    }
    if (options.enable_ack_mempool) {
        std::cout << "development command enabled: ACK_MEMPOOL\n";
    }

    auto next_sync = std::chrono::steady_clock::now()
        + std::chrono::seconds(options.sync_interval_seconds > 0 ? options.sync_interval_seconds : 1);
    auto runPeriodicSync = [&]() {
        if (options.sync_interval_seconds <= 0 || !sync_server.hasKnownPeers()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_sync) {
            return;
        }
        std::string error;
        sync_server.discoverPeersFromKnown();
        sync_server.syncFromKnownPeers(error);
        next_sync = now + std::chrono::seconds(options.sync_interval_seconds);
    };

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
            runPeriodicSync();
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
        setSocketTimeouts(client.fd(), kPeerReadTimeoutMs);
        sync_server.handleClient(client.fd());
        runPeriodicSync();
    }

    std::cout << "sync server stopped\n";
    return 0;
}
