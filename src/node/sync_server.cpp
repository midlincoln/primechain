#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <set>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "primechain/core/consensus.hpp"
#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/protocol/validator_governance.hpp"
#include "primechain/storage/commitment_store.hpp"
#include "primechain/storage/finalization_store.hpp"
#include "primechain/storage/phase_store.hpp"
#include "primechain/storage/record_store.hpp"
#include "primechain/storage/round_change_store.hpp"
#include "primechain/storage/validator_epoch_store.hpp"
#include "primechain/version.hpp"
#include "primechain/wallet/miner_identity.hpp"

namespace {

constexpr int kDefaultPort = 18889;
constexpr const char* kDefaultStorePath = "data/sequential-chain.dat";
constexpr std::size_t kMaxLineBytes = 8192;
constexpr std::size_t kFrameThresholdBytes = 4096;
constexpr std::size_t kMaxFrameBytes = 1024 * 1024;
constexpr std::uint64_t kMaxRecordRangeCount = 10000;
constexpr std::size_t kMaxMempoolTransactions = 1000;
constexpr std::size_t kMaxMempoolTransactionsPerSender = 25;
constexpr std::uint64_t kMempoolMaxTransactionAgeSeconds = 60 * 60;
constexpr std::size_t kMaxCompositeCommitments = 1024;
constexpr std::size_t kMaxKnownPeers = 32;
constexpr std::uint64_t kPeerQuarantineFailureThreshold = 3;
constexpr int kPeerConnectTimeoutMs = 1500;
constexpr int kPeerReadTimeoutMs = 20000;
constexpr std::size_t kMaxCommandsPerConnection = 128;
constexpr std::size_t kMaxWriteCommandsPerConnection = 16;
constexpr std::size_t kMaxInvalidCommandsPerConnection = 3;
constexpr std::size_t kClientViolationBanThreshold = 6;
constexpr std::uint64_t kClientViolationBanSeconds = 60;
constexpr std::size_t kMaxActiveRemoteConnectionsPerIp = 64;
constexpr std::size_t kMaxActiveRemoteConnectionsTotal = 128;
constexpr int kMempoolRebroadcastIntervalSeconds = 30;
constexpr std::uint32_t kDefaultCompositeLotteryWinBps = 5000;
constexpr std::size_t kMaxCompositeLotteryCandidates = 64;
volatile std::sig_atomic_t g_running = 1;
std::mutex g_client_connection_mutex;
std::map<std::uint32_t, std::size_t> g_active_remote_connections;
std::size_t g_active_remote_connection_total = 0;
std::mutex g_peer_sync_mutex;
std::uint64_t g_peer_sync_counter = 0;
std::mutex g_record_range_mutex;

void handleSignal(int) {
    g_running = 0;
}

std::string versionLine() {
    std::ostringstream out;
    out << "VERSION"
        << " name=" << primechain::version::kName
        << " version=" << primechain::version::kVersion
        << " git_commit=" << primechain::version::kGitCommit
        << " build_time=" << primechain::version::kBuildTimestamp
        << " protocol=" << primechain::version::kProtocolVersion
        << " network=" << primechain::version::kNetworkVersion;
    return out.str();
}

bool removeIfPresent(const std::string& path, std::string& error) {
    if (std::remove(path.c_str()) == 0) return true;
    if (errno == ENOENT) return true;
    error = std::string("could not remove ") + path + ": " + std::strerror(errno);
    return false;
}

struct PeerEndpoint {
    std::string host;
    int port{0};
};

struct PeerRuntimeState {
    std::uint64_t consecutive_failures{0};
    bool quarantined{false};
    std::uint64_t last_success_time{0};
    std::uint64_t last_failure_time{0};
    std::string last_error;
};

struct ClientPenaltyState {
    std::size_t violations{0};
    std::uint64_t banned_until{0};
};

struct CommitPhaseTimeoutVote {
    primechain::Address validator_address;
    std::vector<std::uint8_t> public_key;
    primechain::Hash256 previous_record_hash{};
    primechain::PrimeValue integer{0};
    std::uint64_t current_round{1};
    std::uint64_t new_round{0};
    std::vector<std::uint8_t> signature;
};

struct EconomicPolicyVoteRecord {
    primechain::Hash256 previous_record_hash{};
    primechain::PrimeValue record_integer{0};
    std::uint64_t transfer_fee_micro_units{0};
    std::uint64_t validator_min_reserve_micro_units{0};
    primechain::PrimeValue effective_integer{0};
    std::uint64_t sequence{0};
    primechain::protocol::EconomicPolicyVoteV1 vote;
};

struct SignedCompositeReveal {
    primechain::PrimeValue g{0};
    primechain::PrimeValue d{0};
    primechain::PrimeValue e{0};
    std::uint64_t nonce{0};
    primechain::Address provider_address;
    std::vector<std::uint8_t> public_key;
    std::vector<std::uint8_t> signature;
};

struct ValidatorCandidateStats {
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
};

bool samePeer(const PeerEndpoint& left, const PeerEndpoint& right) {
    return left.host == right.host && left.port == right.port;
}

std::string peerKey(const PeerEndpoint& peer) {
    return peer.host + ":" + std::to_string(peer.port);
}

bool validPeerEndpoint(const PeerEndpoint& peer) {
    if (peer.port <= 0 || peer.port > 65535) {
        return false;
    }
    sockaddr_in addr{};
    return inet_pton(AF_INET, peer.host.c_str(), &addr.sin_addr) == 1;
}


std::string healthToken(std::string value) {
    if (value.empty()) return "none";
    for (char& ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '=') ch = '_';
    }
    return value;
}

std::uint64_t currentUnixTime() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool isLoopbackClient(const sockaddr_in& addr) {
    return ntohl(addr.sin_addr.s_addr) == INADDR_LOOPBACK;
}

std::uint32_t clientIpKey(const sockaddr_in& addr) {
    return ntohl(addr.sin_addr.s_addr);
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

bool writeCommand(int fd, std::string command) {
    if (!command.empty() && command.back() == '\n') command.pop_back();
    if (command.size() <= kFrameThresholdBytes) return writeAll(fd, command + "\n");
    return writeAll(fd, "FRAME " + std::to_string(command.size()) + "\n") &&
        writeAll(fd, command);
}

std::optional<std::string> readRawLine(int fd) {
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

std::optional<std::string> readFrame(int fd, const std::string& header) {
    std::istringstream in(header);
    std::string tag;
    std::size_t size = 0;
    std::string extra;
    in >> tag >> size;
    if (!in || tag != "FRAME" || size == 0 || size > kMaxFrameBytes || (in >> extra)) {
        return std::nullopt;
    }
    std::string payload(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t received = recv(fd, payload.data() + offset, size - offset, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return std::nullopt;
        offset += static_cast<std::size_t>(received);
    }
    return payload;
}

std::optional<std::string> readLine(int fd) {
    auto line = readRawLine(fd);
    if (line.has_value() && line->rfind("FRAME ", 0) == 0) {
        return readFrame(fd, *line);
    }
    return line;
}

bool isWriteCommand(const std::string& line) {
    return line.rfind("ADD_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_TX ", 0) == 0 ||
           line.rfind("SUBMIT_SIGNED_COMMIT ", 0) == 0 ||
           line.rfind("SUBMIT_SIGNED_COMMIT_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_COMMIT ", 0) == 0 ||
           line.rfind("CLOSE_COMMIT_PHASE ", 0) == 0 ||
           line.rfind("SUBMIT_PHASE_VOTE ", 0) == 0 ||
           line.rfind("SUBMIT_PHASE_VOTE_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_PHASE_VOTE_BUNDLE ", 0) == 0 ||
           line.rfind("SUBMIT_PHASE_VOTE_BUNDLE_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_EPOCH_VOTE ", 0) == 0 ||
           line.rfind("SUBMIT_EPOCH_VOTE_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_VALIDATOR_ENDPOINT ", 0) == 0 ||
           line.rfind("SUBMIT_VALIDATOR_ENDPOINT_PEER ", 0) == 0 ||
           line.rfind("SIGN_ROUND_CHANGE ", 0) == 0 ||
           line.rfind("SIGN_COMMIT_PHASE_TIMEOUT ", 0) == 0 ||
           line.rfind("TIMEOUT_COMMIT_PHASE ", 0) == 0 ||
           line.rfind("SIGN_RECORD_CANDIDATE ", 0) == 0 ||
           line.rfind("SIGN_COMPOSITE_LOTTERY ", 0) == 0 ||
           line.rfind("SUBMIT_SIGNED_REVEAL ", 0) == 0 ||
           line.rfind("SUBMIT_SIGNED_REVEAL_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_COMPOSITE_REVEAL ", 0) == 0 ||
           line.rfind("SUBMIT_COMPOSITE ", 0) == 0 ||
           line.rfind("SUBMIT_SIGNED_PRIME ", 0) == 0 ||
           line.rfind("SUBMIT_SIGNED_PRIME_PEER ", 0) == 0 ||
           line.rfind("SUBMIT_PRIME ", 0) == 0 ||
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


std::optional<primechain::Hash256> canonicalRecordHashFromPayload(
    primechain::storage::StoredRecordKind kind,
    const std::vector<std::uint8_t>& payload) {
    std::string error;
    if (kind == primechain::storage::StoredRecordKind::Composite) {
        auto record = primechain::protocol::deserializeCompositeRecord(payload, error);
        if (!record.has_value()) return std::nullopt;
        return primechain::protocol::canonicalStoredRecordHash(*record);
    }
    auto record = primechain::protocol::deserializePrimeRecord(payload, error);
    if (!record.has_value()) return std::nullopt;
    return primechain::protocol::canonicalStoredRecordHash(*record);
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
    if (payload.size() != payload_size) {
        return std::nullopt;
    }
    const auto canonical_hash = canonicalRecordHashFromPayload(*kind, payload);
    if (!canonical_hash.has_value() || *canonical_hash != *hash) {
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

std::optional<primechain::Hash256> recordSubjectHashWithoutFinalization(
    const primechain::storage::StoredRecord& stored,
    std::string& error) {
    if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) return std::nullopt;
        if (decoded->version >= primechain::node::kSubjectHashRecordVersion) {
            return primechain::protocol::subjectRecordHash(*decoded);
        }
        return primechain::protocol::legacyCandidateRecordHashWithoutFinalization(*decoded);
    }

    const auto decoded = primechain::protocol::deserializePrimeRecord(stored.payload, error);
    if (!decoded.has_value()) return std::nullopt;
    if (decoded->version >= primechain::node::kSubjectHashRecordVersion) {
        return primechain::protocol::subjectRecordHash(*decoded);
    }
    return primechain::protocol::legacyCandidateRecordHashWithoutFinalization(*decoded);
}

std::optional<std::uint64_t> recordFinalizationRound(
    const primechain::storage::StoredRecord& stored,
    const std::vector<primechain::Address>& validator_set,
    std::string& error) {
    primechain::protocol::FinalizationProofV0 proof;
    primechain::Hash256 previous_hash{};
    primechain::PrimeValue integer = 0;

    if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) return std::nullopt;
        proof = decoded->finalized_by;
        previous_hash = decoded->previous_record_hash;
        integer = decoded->integer;
    } else {
        const auto decoded = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!decoded.has_value()) return std::nullopt;
        proof = decoded->finalized_by;
        previous_hash = decoded->previous_record_hash;
        integer = decoded->integer;
    }

    std::uint64_t round = 0;
    if (!primechain::protocol::verifyRoundChangeCertificate(
            proof, previous_hash, integer, validator_set, round, error)) {
        return std::nullopt;
    }
    return round;
}

std::optional<bool> recordsShareFinalizationSubject(
    const primechain::storage::StoredRecord& left,
    const primechain::storage::StoredRecord& right,
    std::string& error) {
    if (left.kind != right.kind || left.integer != right.integer || left.height != right.height) {
        return false;
    }

    const auto left_previous = previousRecordHash(left, error);
    if (!left_previous.has_value()) return std::nullopt;
    error.clear();
    const auto right_previous = previousRecordHash(right, error);
    if (!right_previous.has_value()) return std::nullopt;
    if (*left_previous != *right_previous) return false;

    error.clear();
    const auto left_subject = recordSubjectHashWithoutFinalization(left, error);
    if (!left_subject.has_value()) return std::nullopt;
    error.clear();
    const auto right_subject = recordSubjectHashWithoutFinalization(right, error);
    if (!right_subject.has_value()) return std::nullopt;
    return *left_subject == *right_subject;
}

std::string storedKindName(primechain::storage::StoredRecordKind kind) {
    return kind == primechain::storage::StoredRecordKind::Prime ? "PRIME" : "COMPOSITE";
}

std::optional<primechain::storage::StoredRecordKind> parseStoredKindName(const std::string& kind) {
    if (kind == "PRIME") return primechain::storage::StoredRecordKind::Prime;
    if (kind == "COMPOSITE") return primechain::storage::StoredRecordKind::Composite;
    return std::nullopt;
}

std::optional<primechain::Hash256> subjectHashFromCandidatePayload(
    primechain::storage::StoredRecordKind kind,
    const std::vector<std::uint8_t>& payload,
    std::string& error) {
    if (kind == primechain::storage::StoredRecordKind::Prime) {
        const auto record = primechain::protocol::deserializePrimeRecord(payload, error);
        if (!record.has_value()) return std::nullopt;
        if (record->version >= primechain::node::kSubjectHashRecordVersion) {
            return primechain::protocol::subjectRecordHash(*record);
        }
        return primechain::protocol::legacyCandidateRecordHashWithoutFinalization(*record);
    }
    const auto record = primechain::protocol::deserializeCompositeRecord(payload, error);
    if (!record.has_value()) return std::nullopt;
    if (record->version >= primechain::node::kSubjectHashRecordVersion) {
        return primechain::protocol::subjectRecordHash(*record);
    }
    return primechain::protocol::legacyCandidateRecordHashWithoutFinalization(*record);
}

std::optional<primechain::Hash256> finalizationVoteTargetHashFromPayload(
    primechain::storage::StoredRecordKind kind,
    const std::vector<std::uint8_t>& payload,
    const primechain::protocol::FinalizationProofV0& proof,
    std::string& error) {
    if (proof.rule == "fixed-2-of-3-mldsa65-rounds-locks-v4") {
        return subjectHashFromCandidatePayload(kind, payload, error);
    }
    if (kind == primechain::storage::StoredRecordKind::Prime) {
        const auto record = primechain::protocol::deserializePrimeRecord(payload, error);
        if (!record.has_value()) return std::nullopt;
        return primechain::protocol::candidateRecordHash(*record);
    }
    const auto record = primechain::protocol::deserializeCompositeRecord(payload, error);
    if (!record.has_value()) return std::nullopt;
    return primechain::protocol::candidateRecordHash(*record);
}

std::optional<primechain::Address> recordProviderAddress(
    const primechain::storage::StoredRecord& stored,
    std::string& error) {
    if (stored.kind == primechain::storage::StoredRecordKind::Composite) {
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) return std::nullopt;
        return decoded->proof.provider_address;
    }

    const auto decoded = primechain::protocol::deserializePrimeRecord(stored.payload, error);
    if (!decoded.has_value()) return std::nullopt;
    return decoded->proof.provider_address;
}

bool providerCooldownSatisfied(
    const primechain::storage::RecordStore& store,
    const primechain::node::SequentialNode& node,
    const primechain::Address& provider,
    std::string& error) {
    if (!node.status().has_genesis || node.status().height == 0) return true;
    const auto previous = store.findByInteger(node.status().frontier_integer, error);
    if (!previous.has_value()) {
        if (error.empty()) error = "current frontier record not found";
        return false;
    }
    const auto previous_provider = recordProviderAddress(*previous, error);
    if (!previous_provider.has_value()) return false;
    if (*previous_provider == provider) {
        error = "provider is in winner cooldown for next record";
        return false;
    }
    return true;
}

bool appendStoredRecord(
    primechain::node::SequentialNode& node,
    const primechain::storage::StoredRecord& stored,
    std::string& error) {
    if (!node.status().has_genesis && stored.height == 0 && stored.integer == 2 &&
        stored.kind == primechain::storage::StoredRecordKind::Prime) {
        const auto decoded = primechain::protocol::deserializePrimeRecord(stored.payload, error);
        if (!decoded.has_value() || !primechain::protocol::verifyGenesisConfig(*decoded, error)) {
            return false;
        }
        const auto expected_genesis = primechain::storage::makeStoredRecord(
            primechain::node::makeGenesisPrimeRecordV0(
                decoded->genesis_config.validator_set));
        if (stored.record_hash != expected_genesis.record_hash ||
            stored.payload != expected_genesis.payload) {
            error = "incoming genesis record is not canonical";
            return false;
        }
        return node.initializeGenesis(decoded->genesis_config.validator_set, error);
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

void copyReplaySnapshotIfPresent(const std::string& source_store,
                                 const std::string& destination_store) {
    std::string ignored;
    copyFile(source_store + ".snapshot", destination_store + ".snapshot", ignored);
}

void removeStoreTempArtifacts(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".idx").c_str());
    std::remove((path + ".snapshot").c_str());
    std::remove((path + ".snapshot.tmp").c_str());
    std::remove((path + ".finalization").c_str());
    std::remove((path + ".rounds").c_str());
    std::remove((path + ".peers").c_str());
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
    const std::string& prime_miner_address,
    const primechain::protocol::Bytes& authentication = {}) {
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
    record.proof.signature = authentication;
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
    record.proof.signature = proof.signature;
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
    if (!writeCommand(socket->fd(), "GET_STATUS\n")) {
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
    if (!writeCommand(socket->fd(), "GET_PEERS\n")) {
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


std::string commitmentWireLine(const primechain::storage::StoredCommitment& commitment);

std::string commitmentWireLine(
    primechain::PrimeValue integer,
    const primechain::protocol::CommitCertificateEntryV1& commitment);

bool parseCommitmentWireLine(
    const std::string& line,
    primechain::PrimeValue integer,
    std::uint64_t commit_round,
    primechain::storage::StoredCommitment& commitment,
    std::string& error);

std::string phaseVoteWireSuffix(const primechain::storage::CommitPhaseVote& vote);

bool parsePhaseVoteWireSuffix(
    const std::string& response,
    const std::string& response_prefix,
    primechain::PrimeValue integer,
    primechain::storage::CommitPhaseVote& vote);


std::vector<primechain::storage::StoredCommitment> requestCommitments(
    const std::string& host,
    int port,
    primechain::PrimeValue integer,
    std::string& error) {
    std::vector<primechain::storage::StoredCommitment> commitments;
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return {};
    }
    if (!writeCommand(socket->fd(), "GET_COMMITMENTS " + std::to_string(integer) + "\n")) {
        error = "could not request peer commitments";
        return {};
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto header = readLine(socket->fd());
    if (!header.has_value()) {
        error = "peer did not return commitment header";
        return {};
    }
    std::istringstream header_in(*header);
    std::string tag;
    primechain::PrimeValue response_integer = 0;
    std::uint64_t expected_count = 0;
    header_in >> tag >> response_integer >> expected_count;
    if (!header_in || tag != "COMMITMENTS" || response_integer != integer ||
        expected_count > kMaxCompositeCommitments) {
        error = "invalid peer commitment header";
        return {};
    }

    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_COMMITMENTS") {
            break;
        }
        primechain::storage::StoredCommitment commitment;
        if (!parseCommitmentWireLine(*line, integer, 1, commitment, error)) {
            error = "invalid peer commitment entry: " + error;
            return {};
        }
        commitments.push_back(std::move(commitment));
        if (commitments.size() > expected_count) {
            error = "peer commitment count mismatch";
            return {};
        }
    }
    if (commitments.size() != expected_count) {
        error = "peer commitment count mismatch";
        return {};
    }
    return commitments;
}

std::string commitmentWireLine(const primechain::storage::StoredCommitment& commitment) {
    return "COMMITMENT " + std::to_string(commitment.integer) + " "
        + primechain::crypto::toHex(commitment.commitment_hash) + " "
        + commitment.provider_address + " "
        + (commitment.public_key.empty() ? "-" : bytesToHex(commitment.public_key)) + " "
        + (commitment.signature.empty() ? "-" : bytesToHex(commitment.signature));
}

std::string commitmentWireLine(
    primechain::PrimeValue integer,
    const primechain::protocol::CommitCertificateEntryV1& commitment) {
    return "COMMITMENT " + std::to_string(integer) + " "
        + primechain::crypto::toHex(commitment.commitment_hash) + " "
        + commitment.provider_address + " "
        + (commitment.public_key.empty() ? "-" : bytesToHex(commitment.public_key)) + " "
        + (commitment.signature.empty() ? "-" : bytesToHex(commitment.signature));
}

bool parseCommitmentWireLine(
    const std::string& line,
    primechain::PrimeValue integer,
    std::uint64_t commit_round,
    primechain::storage::StoredCommitment& commitment,
    std::string& error) {
    std::istringstream in(line);
    std::string entry_tag;
    std::string hash_hex;
    std::string public_key_hex;
    std::string signature_hex;
    std::string extra;
    in >> entry_tag >> commitment.integer >> hash_hex >> commitment.provider_address
       >> public_key_hex >> signature_hex;
    const auto hash = parseHash(hash_hex);
    if (!in || entry_tag != "COMMITMENT" || commitment.integer != integer ||
        !hash.has_value() || (in >> extra)) {
        error = "invalid commitment entry";
        return false;
    }
    commitment.commit_round = commit_round;
    commitment.commitment_hash = *hash;
    if (public_key_hex != "-" || signature_hex != "-") {
        commitment.public_key = hexToBytes(public_key_hex);
        commitment.signature = hexToBytes(signature_hex);
        std::string verification_error;
        if (commitment.provider_address !=
                primechain::crypto::addressFromProtocolPublicKey(commitment.public_key) ||
            !primechain::crypto::verifyProtocolMessageSignature(
                commitment.public_key,
                primechain::crypto::compositeCommitSigningPayload(
                    commitment.integer,
                    commitment.commitment_hash,
                    commitment.provider_address),
                commitment.signature,
                verification_error)) {
            error = "invalid signed commitment entry";
            return false;
        }
    } else if (!primechain::protocol::isDevelopmentAddress(commitment.provider_address)) {
        error = "invalid legacy commitment address";
        return false;
    }
    return true;
}

std::string phaseVoteWireSuffix(const primechain::storage::CommitPhaseVote& vote) {
    return std::to_string(vote.integer) + " " + std::to_string(vote.commit_round) + " "
        + primechain::crypto::toHex(vote.snapshot_hash) + " "
        + vote.validator_address + " " + bytesToHex(vote.public_key) + " "
        + bytesToHex(vote.signature);
}

bool parsePhaseVoteWireSuffix(
    const std::string& response,
    const std::string& response_prefix,
    primechain::PrimeValue integer,
    primechain::storage::CommitPhaseVote& vote) {
    if (response.rfind(response_prefix, 0) != 0) return false;
    std::istringstream in(response.substr(response_prefix.size()));
    std::string snapshot_hex;
    std::string public_key_hex;
    std::string signature_hex;
    std::string votes_token;
    std::string extra;
    in >> vote.integer >> vote.commit_round >> snapshot_hex >> vote.validator_address
       >> public_key_hex >> signature_hex >> votes_token;
    const auto snapshot = parseHash(snapshot_hex);
    if (!in || vote.integer != integer || !snapshot.has_value() ||
        votes_token.rfind("votes=", 0) != 0 || (in >> extra)) {
        return false;
    }
    vote.snapshot_hash = *snapshot;
    vote.public_key = hexToBytes(public_key_hex);
    vote.signature = hexToBytes(signature_hex);
    return true;
}

std::vector<primechain::storage::CommitPhaseVote> requestPhaseVotes(
    const std::string& host,
    int port,
    primechain::PrimeValue integer,
    std::string& error) {
    std::vector<primechain::storage::CommitPhaseVote> votes;
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return {};
    }
    if (!writeCommand(socket->fd(), "GET_PHASE_VOTES " + std::to_string(integer) + "\n")) {
        error = "could not request peer phase votes";
        return {};
    }
    shutdown(socket->fd(), SHUT_WR);
    const auto header = readLine(socket->fd());
    if (!header.has_value()) {
        error = "peer did not return phase vote header";
        return {};
    }
    std::istringstream header_in(*header);
    std::string tag;
    primechain::PrimeValue response_integer = 0;
    std::uint64_t expected_count = 0;
    header_in >> tag >> response_integer >> expected_count;
    if (!header_in || tag != "PHASE_VOTES" || response_integer != integer ||
        expected_count > kMaxKnownPeers) {
        error = "invalid peer phase vote header";
        return {};
    }
    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_PHASE_VOTES") break;
        std::istringstream in(*line);
        std::string entry_tag, maybe_round_or_snapshot, snapshot_hex, public_key_hex, signature_hex, extra;
        primechain::storage::CommitPhaseVote vote;
        in >> entry_tag >> vote.integer >> maybe_round_or_snapshot;
        auto snapshot = parseHash(maybe_round_or_snapshot);
        if (snapshot.has_value()) {
            snapshot_hex = maybe_round_or_snapshot;
            vote.commit_round = 1;
            in >> vote.validator_address >> public_key_hex >> signature_hex;
        } else {
            vote.commit_round = std::stoull(maybe_round_or_snapshot);
            in >> snapshot_hex >> vote.validator_address >> public_key_hex >> signature_hex;
            snapshot = parseHash(snapshot_hex);
        }
        if (!in || entry_tag != "PHASE_VOTE" || vote.integer != integer ||
            !snapshot.has_value() || (in >> extra)) {
            error = "invalid peer phase vote entry";
            return {};
        }
        vote.snapshot_hash = *snapshot;
        vote.public_key = hexToBytes(public_key_hex);
        vote.signature = hexToBytes(signature_hex);
        votes.push_back(std::move(vote));
        if (votes.size() > expected_count) {
            error = "peer phase vote count mismatch";
            return {};
        }
    }
    if (votes.size() != expected_count) {
        error = "peer phase vote count mismatch";
        return {};
    }
    return votes;
}

std::optional<primechain::protocol::CompositeLotteryProofV1> requestCompositeLotteryWin(
    const PeerEndpoint& peer,
    const primechain::protocol::CompositeRecordV0& candidate,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) { error = "could not connect to validator peer"; return std::nullopt; }
    std::ostringstream command;
    command << "SIGN_COMPOSITE_LOTTERY "
            << bytesToHex(primechain::protocol::serializeCompositeRecord(candidate)) << "\n";
    if (!writeCommand(socket->fd(), command.str())) {
        error = "could not submit composite lottery candidate to validator";
        return std::nullopt;
    }
    shutdown(socket->fd(), SHUT_WR);
    const auto response = readLine(socket->fd());
    if (!response.has_value()) {
        error = "validator did not return composite lottery response";
        return std::nullopt;
    }
    std::istringstream in(*response);
    std::string tag, subject_hex, assigned, public_hex, signature_hex, extra;
    primechain::protocol::CompositeLotteryProofV1 proof;
    in >> tag >> proof.round >> proof.win_bps >> subject_hex >> assigned >> public_hex >> signature_hex;
    const auto subject = parseHash(subject_hex);
    if (!in || tag != "COMPOSITE_LOTTERY_WIN" || !subject.has_value() || (in >> extra)) {
        error = *response;
        return std::nullopt;
    }
    proof.subject_hash = *subject;
    proof.assigned_validator = assigned;
    proof.public_key = hexToBytes(public_hex);
    proof.signature = hexToBytes(signature_hex);
    return proof;
}

std::optional<primechain::protocol::ValidatorVoteV0> requestRecordFinalizationVote(
    const PeerEndpoint& peer,
    primechain::storage::StoredRecordKind kind,
    const std::vector<std::uint8_t>& candidate_payload,
    const primechain::protocol::ValidatorVoteV0* proposer_vote,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) { error = "could not connect to validator peer"; return std::nullopt; }
    std::ostringstream command;
    command << "SIGN_RECORD_CANDIDATE " << kindName(kind) << " "
            << bytesToHex(candidate_payload);
    if (proposer_vote != nullptr) {
        command << " " << proposer_vote->validator_address << " "
                << bytesToHex(proposer_vote->public_key) << " "
                << primechain::crypto::toHex(proposer_vote->record_hash) << " "
                << proposer_vote->round << " " << bytesToHex(proposer_vote->signature);
    }
    command << "\n";
    if (!writeCommand(socket->fd(), command.str())) { error = "could not submit candidate to validator"; return std::nullopt; }
    shutdown(socket->fd(), SHUT_WR);
    const auto response = readLine(socket->fd());
    if (!response.has_value()) { error = "validator did not return finalization vote"; return std::nullopt; }
    std::istringstream in(*response);
    std::string tag, hash_hex, public_hex, signature_hex, extra;
    primechain::protocol::ValidatorVoteV0 vote;
    in >> tag >> vote.validator_address >> public_hex >> hash_hex >> vote.round >> signature_hex;
    const auto hash = parseHash(hash_hex);
    if (!in || tag != "FINALIZATION_VOTE" || !hash.has_value() || (in >> extra)) {
        error = *response;
        return std::nullopt;
    }
    vote.public_key = hexToBytes(public_hex);
    vote.record_hash = *hash;
    vote.signature = hexToBytes(signature_hex);
    return vote;
}

std::optional<primechain::protocol::RoundChangeVoteV1> requestRoundChangeVote(
    const PeerEndpoint& peer,
    const primechain::protocol::RoundChangeVoteV1& proposer_vote,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) { error = "could not connect to validator peer"; return std::nullopt; }
    std::ostringstream command;
    command << "SIGN_ROUND_CHANGE "
            << primechain::crypto::toHex(proposer_vote.previous_record_hash) << " "
            << proposer_vote.integer << " " << proposer_vote.new_round << " "
            << proposer_vote.locked_round << " "
            << (proposer_vote.locked_candidate_kind.empty() ? "NONE" : proposer_vote.locked_candidate_kind) << " "
            << primechain::crypto::toHex(proposer_vote.locked_candidate_hash) << " "
            << (proposer_vote.locked_candidate_payload.empty() ? "-" : bytesToHex(proposer_vote.locked_candidate_payload)) << " "
            << proposer_vote.validator_address << " "
            << bytesToHex(proposer_vote.public_key) << " "
            << bytesToHex(proposer_vote.signature) << "\n";
    if (!writeCommand(socket->fd(), command.str())) { error = "could not submit round change"; return std::nullopt; }
    shutdown(socket->fd(), SHUT_WR);
    const auto response = readLine(socket->fd());
    if (!response.has_value()) { error = "validator did not return round-change vote"; return std::nullopt; }
    std::istringstream in(*response);
    std::string tag, previous_hex, kind, locked_hash_hex, locked_payload_hex, public_hex, signature_hex, extra;
    primechain::protocol::RoundChangeVoteV1 vote;
    in >> tag >> previous_hex >> vote.integer >> vote.new_round
       >> vote.locked_round >> kind >> locked_hash_hex >> locked_payload_hex
       >> vote.validator_address >> public_hex >> signature_hex;
    const auto previous = parseHash(previous_hex);
    const auto locked_hash = parseHash(locked_hash_hex);
    if (!in || tag != "ROUND_CHANGE_VOTE" || !previous.has_value() || !locked_hash.has_value() || (in >> extra)) {
        error = *response; return std::nullopt;
    }
    vote.previous_record_hash = *previous;
    vote.locked_candidate_kind = (kind == "NONE") ? std::string{} : kind;
    vote.locked_candidate_hash = *locked_hash;
    vote.locked_candidate_payload = (locked_payload_hex == "-") ? std::vector<std::uint8_t>{} : hexToBytes(locked_payload_hex);
    vote.public_key = hexToBytes(public_hex);
    vote.signature = hexToBytes(signature_hex);
    return vote;
}

std::optional<CommitPhaseTimeoutVote> requestCommitPhaseTimeoutVote(
    const PeerEndpoint& peer,
    const CommitPhaseTimeoutVote& proposer_vote,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) { error = "could not connect to validator peer"; return std::nullopt; }
    std::ostringstream command;
    command << "SIGN_COMMIT_PHASE_TIMEOUT "
            << primechain::crypto::toHex(proposer_vote.previous_record_hash) << " "
            << proposer_vote.integer << " " << proposer_vote.current_round << " "
            << proposer_vote.new_round << " " << proposer_vote.validator_address << " "
            << bytesToHex(proposer_vote.public_key) << " "
            << bytesToHex(proposer_vote.signature) << "\n";
    if (!writeCommand(socket->fd(), command.str())) { error = "could not submit commit-phase timeout"; return std::nullopt; }
    shutdown(socket->fd(), SHUT_WR);
    const auto response = readLine(socket->fd());
    if (!response.has_value()) { error = "validator did not return commit-phase timeout vote"; return std::nullopt; }
    std::istringstream in(*response);
    std::string tag, previous_hex, public_hex, signature_hex, extra;
    CommitPhaseTimeoutVote vote;
    in >> tag >> previous_hex >> vote.integer >> vote.current_round >> vote.new_round
       >> vote.validator_address >> public_hex >> signature_hex;
    const auto previous = parseHash(previous_hex);
    if (!in || tag != "COMMIT_PHASE_TIMEOUT_VOTE" || !previous.has_value() || (in >> extra)) {
        error = *response;
        return std::nullopt;
    }
    vote.previous_record_hash = *previous;
    vote.public_key = hexToBytes(public_hex);
    vote.signature = hexToBytes(signature_hex);
    return vote;
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
    if (!writeCommand(socket->fd(), command.str())) {
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
    if (!writeCommand(socket->fd(), "SUBMIT_TX " + bytesToHex(bytes) + "\n")) {
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

bool fetchMempoolFromPeer(
    const std::string& host,
    int port,
    std::vector<primechain::protocol::TransactionV0>& transactions,
    std::string& error) {
    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    if (!writeCommand(socket->fd(), "GET_MEMPOOL\n")) {
        error = "could not request peer mempool";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto header = readLine(socket->fd());
    if (!header.has_value()) {
        error = "peer did not return mempool header";
        return false;
    }

    std::istringstream header_in(*header);
    std::string header_tag;
    std::uint64_t expected_count = 0;
    header_in >> header_tag >> expected_count;
    if (!header_in || header_tag != "MEMPOOL") {
        error = "invalid peer mempool header";
        return false;
    }
    if (expected_count > kMaxMempoolTransactions) {
        error = "peer mempool exceeds configured limit";
        return false;
    }

    std::uint64_t received = 0;
    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_MEMPOOL") {
            break;
        }
        std::istringstream in(*line);
        std::string tag;
        std::string hash_hex;
        std::size_t size = 0;
        std::string tx_hex;
        in >> tag >> hash_hex >> size >> tx_hex;
        if (!in || tag != "TX" || tx_hex.empty()) {
            error = "invalid peer mempool transaction line";
            return false;
        }
        const auto bytes = hexToBytes(tx_hex);
        if (bytes.size() != size) {
            error = "peer mempool transaction size mismatch";
            return false;
        }
        std::string tx_error;
        const auto tx = primechain::protocol::deserializeTransaction(bytes, tx_error);
        if (!tx.has_value()) {
            error = "invalid peer mempool transaction: " + tx_error;
            return false;
        }
        if (primechain::crypto::toHex(primechain::protocol::transactionHash(*tx)) != hash_hex) {
            error = "peer mempool transaction hash mismatch";
            return false;
        }
        transactions.push_back(*tx);
        ++received;
    }
    if (received != expected_count) {
        error = "peer mempool count mismatch";
        return false;
    }
    return true;
}

std::string signedRevealLine(const SignedCompositeReveal& reveal, bool peer_command) {
    std::ostringstream out;
    out << (peer_command ? "SUBMIT_SIGNED_REVEAL_PEER " : "SUBMIT_SIGNED_REVEAL ")
        << reveal.g << " " << reveal.d << " " << reveal.e << " " << reveal.nonce << " "
        << reveal.provider_address << " " << bytesToHex(reveal.public_key) << " "
        << bytesToHex(reveal.signature) << "\n";
    return out.str();
}

bool submitCommitToPeer(
    const PeerEndpoint& peer,
    const primechain::storage::StoredCommitment& commitment,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    std::ostringstream command;
    if (commitment.public_key.empty()) {
        command << "SUBMIT_COMMIT " << commitment.integer << " "
                << primechain::crypto::toHex(commitment.commitment_hash) << " "
                << commitment.provider_address << "\n";
    } else {
        command << "SUBMIT_SIGNED_COMMIT_PEER " << commitment.integer << " "
                << primechain::crypto::toHex(commitment.commitment_hash) << " "
                << commitment.provider_address << " "
                << bytesToHex(commitment.public_key) << " "
                << bytesToHex(commitment.signature) << "\n";
    }
    if (!writeCommand(socket->fd(), command.str())) {
        error = "could not submit commitment to peer";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto response = readLine(socket->fd());
    if (!response.has_value()) {
        error = "peer did not return commitment response";
        return false;
    }
    if (response->rfind("COMMIT_ACCEPTED ", 0) == 0 ||
        response->rfind("COMMIT_DUPLICATE ", 0) == 0) {
        return true;
    }
    error = "peer rejected commitment: " + *response;
    return false;
}

bool submitRevealToPeer(
    const PeerEndpoint& peer,
    const SignedCompositeReveal& reveal,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    if (!writeCommand(socket->fd(), signedRevealLine(reveal, true))) {
        error = "could not submit reveal to peer";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto response = readLine(socket->fd());
    if (!response.has_value()) {
        error = "peer did not return reveal response";
        return false;
    }
    if (response->rfind("COMPOSITE_ACCEPTED ", 0) == 0 ||
        response->rfind("REVEAL_PENDING ", 0) == 0 ||
        response->rfind("REVEAL_DUPLICATE ", 0) == 0 ||
        response->find("no prior commitment for reveal") != std::string::npos ||
        response->find("commitment not selected for reveal") != std::string::npos) {
        return true;
    }
    error = "peer rejected reveal: " + *response;
    return false;
}

bool submitSignedPrimeToPeer(
    const PeerEndpoint& peer,
    const std::string& signed_prime_line,
    std::string& error) {
    auto socket = connectToServer(peer.host, peer.port);
    if (!socket.has_value()) {
        error = "could not connect to peer";
        return false;
    }

    const std::string external_prefix = "SUBMIT_SIGNED_PRIME ";
    std::string command = signed_prime_line;
    if (command.rfind(external_prefix, 0) != 0) {
        error = "invalid signed prime command for peer propagation";
        return false;
    }
    command.replace(0, external_prefix.size(), "SUBMIT_SIGNED_PRIME_PEER ");
    if (command.empty() || command.back() != '\n') command.push_back('\n');

    if (!writeCommand(socket->fd(), command)) {
        error = "could not submit prime evidence to peer";
        return false;
    }
    shutdown(socket->fd(), SHUT_WR);

    const auto response = readLine(socket->fd());
    if (!response.has_value()) {
        error = "peer did not return prime evidence response";
        return false;
    }
    if (response->rfind("PRIME_EVIDENCE_ACCEPTED ", 0) == 0 ||
        response->rfind("PRIME_ACCEPTED ", 0) == 0 ||
        response->rfind("RECORD_DUPLICATE ", 0) == 0 ||
        response->rfind("RECORD_CONFLICT", 0) == 0) {
        return true;
    }
    error = "peer rejected prime evidence: " + *response;
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

    if (!writeCommand(socket->fd(), submitRecordLine(record))) {
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
        std::string bind_address,
        int listen_port,
        std::vector<PeerEndpoint> peers,
        bool advance_enabled,
        bool ack_mempool_enabled,
        bool factorization_helper_enabled,
        int finalization_timeout_ms,
        int composite_lottery_window_ms,
        std::uint32_t composite_lottery_win_bps,
        std::vector<primechain::Address> validator_set,
        std::optional<primechain::wallet::MinerIdentity> validator_identity,
        bool use_chain_endpoints,
        bool allow_remote_admin)
        : store_path_(std::move(store_path)),
          bind_address_(std::move(bind_address)),
          listen_port_(listen_port),
          advance_enabled_(advance_enabled),
          ack_mempool_enabled_(ack_mempool_enabled),
          factorization_helper_enabled_(factorization_helper_enabled),
          finalization_timeout_ms_(finalization_timeout_ms),
          composite_lottery_window_ms_(composite_lottery_window_ms),
          composite_lottery_win_bps_(composite_lottery_win_bps),
          store_(store_path_),
          commitment_store_(store_path_ + ".commitments"),
          phase_store_(store_path_ + ".phases"),
          epoch_store_(store_path_ + ".epochs"),
          finalization_store_(store_path_ + ".finalization"),
          round_change_store_(store_path_ + ".rounds"),
          genesis_validator_set_(validator_set),
          validator_set_(std::move(validator_set)),
          validator_identity_(std::move(validator_identity)),
          use_chain_endpoints_(use_chain_endpoints),
          allow_remote_admin_(allow_remote_admin) {
        std::string peer_error;
        if (!loadPeerState(peer_error)) {
            std::cerr << "peer state load warning: " << peer_error << "\n";
        }
        for (const auto& peer : peers) {
            addPeer(peer);
        }
        if (!peers.empty()) {
            std::string persist_error;
            if (!persistPeerState(persist_error)) {
                std::cerr << "peer state persistence warning: " << persist_error << "\n";
            }
        }
    }

    bool loadGenesisValidatorSet(
        const std::string& path,
        std::vector<primechain::Address>& validators,
        std::string& error) const {
        primechain::storage::RecordStore source(path);
        const auto record = source.findByInteger(2, error);
        if (!error.empty()) return false;
        if (!record.has_value()) return true;
        if (record->kind != primechain::storage::StoredRecordKind::Prime) {
            error = "genesis record must be prime";
            return false;
        }
        const auto genesis = primechain::protocol::deserializePrimeRecord(
            record->payload, error);
        if (!genesis.has_value() || !primechain::protocol::verifyGenesisConfig(*genesis, error)) {
            return false;
        }
        validators = genesis->genesis_config.validator_set;
        return true;
    }

    bool ensureValidatorAnchor(std::string& error) {
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        if (!quorumEnabled()) return true;
        if (!node.status().has_genesis) {
            if (!node.initializeGenesis(genesis_validator_set_, error)) return false;
        } else {
            std::vector<primechain::Address> anchored;
            if (!loadGenesisValidatorSet(store_path_, anchored, error)) return false;
            if (anchored != genesis_validator_set_) {
                error = anchored.empty()
                    ? "quorum mode requires validator set anchored in genesis"
                    : "configured validator set differs from genesis anchor";
                return false;
            }
        }
        validator_set_ = node.validatorSet();
        if (use_chain_endpoints_) {
            loadChainEndpointPeers();
        }
        return true;
    }

    bool localValidatorActive() const {
        return validator_identity_.has_value() &&
               std::binary_search(
                   validator_set_.begin(), validator_set_.end(), validator_identity_->address);
    }

    std::size_t loadChainEndpointPeers() {
        std::string error;
        primechain::storage::RecordStore store(store_path_);
        const auto records = store.loadAll(error);
        if (!error.empty()) {
            std::cerr << "chain endpoint peer load warning: " << error << "\n";
            return 0;
        }

        std::map<primechain::Address, primechain::protocol::ValidatorEndpointUpdateV1> latest;
        for (const auto& stored : records) {
            if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
                auto prime = primechain::protocol::deserializePrimeRecord(stored.payload, error);
                if (!prime.has_value()) {
                    std::cerr << "chain endpoint peer load warning: " << error << "\n";
                    return 0;
                }
                for (const auto& update : prime->validator_endpoints) latest[update.validator_address] = update;
            } else {
                auto composite = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
                if (!composite.has_value()) {
                    std::cerr << "chain endpoint peer load warning: " << error << "\n";
                    return 0;
                }
                for (const auto& update : composite->validator_endpoints) latest[update.validator_address] = update;
            }
        }

        std::size_t added = 0;
        for (const auto& validator : validator_set_) {
            if (validator_identity_.has_value() && validator == validator_identity_->address) {
                continue;
            }
            const auto found = latest.find(validator);
            if (found == latest.end()) continue;
            PeerEndpoint peer{found->second.host, static_cast<int>(found->second.port)};
            const auto before = peers_.size();
            if (addPeer(peer) && peers_.size() > before) ++added;
        }
        if (added > 0) {
            std::string persist_error;
            if (!persistPeerState(persist_error)) {
                std::cerr << "peer state persistence warning: " << persist_error << "\n";
            }
        }
        return added;
    }

    bool clientBanned(std::uint32_t client_ip, bool client_loopback) const {
        if (client_loopback) return false;
        std::lock_guard<std::mutex> lock(client_penalty_mutex_);
        const auto found = client_penalties_.find(client_ip);
        if (found == client_penalties_.end()) return false;
        const auto now = currentUnixTime();
        return found->second.banned_until > now;
    }

    void recordClientViolation(std::uint32_t client_ip, bool client_loopback) const {
        if (client_loopback) return;
        std::lock_guard<std::mutex> lock(client_penalty_mutex_);
        auto& state = client_penalties_[client_ip];
        const auto now = currentUnixTime();
        if (state.banned_until > now) return;
        ++state.violations;
        if (state.violations >= kClientViolationBanThreshold) {
            state.banned_until = now + kClientViolationBanSeconds;
            state.violations = 0;
        }
    }

    bool adminAllowed(bool client_loopback) const {
        return client_loopback || allow_remote_admin_;
    }

    bool rejectRemoteAdmin(int fd, std::uint32_t client_ip, bool client_loopback) const {
        if (adminAllowed(client_loopback)) return false;
        recordClientViolation(client_ip, client_loopback);
        writeAll(fd, "ERROR admin command requires local connection; restart with --allow-remote-admin to override\n");
        return true;
    }

    void handleClient(int fd, std::uint32_t client_ip, bool client_loopback) {
        std::size_t command_count = 0;
        std::size_t write_command_count = 0;
        std::size_t invalid_command_count = 0;
        while (auto line = readLine(fd)) {
            ++command_count;
            if (command_count > kMaxCommandsPerConnection) {
                recordClientViolation(client_ip, client_loopback);
                writeAll(fd, "ERROR rate limit exceeded: too many commands on one connection\n");
                return;
            }
            if (isWriteCommand(*line)) {
                ++write_command_count;
                if (write_command_count > kMaxWriteCommandsPerConnection) {
                    recordClientViolation(client_ip, client_loopback);
                    writeAll(fd, "ERROR rate limit exceeded: too many write commands on one connection\n");
                    return;
                }
            }
            if (*line == "GET_STATUS") {
                sendStatus(fd);
                continue;
            }
            if (*line == "GET_VERSION") {
                writeAll(fd, versionLine() + "\n");
                continue;
            }
            if (*line == "GET_VALIDATORS") {
                sendValidators(fd);
                continue;
            }
            if (*line == "GET_VALIDATOR_EPOCH") {
                sendValidatorEpoch(fd);
                continue;
            }
            if (*line == "GET_VALIDATOR_ENDPOINTS") {
                sendValidatorEndpoints(fd);
                continue;
            }
            if (*line == "GET_ECONOMIC_POLICY") {
                sendEconomicPolicy(fd);
                continue;
            }
            if (*line == "GET_MINING_VIEW" || line->rfind("GET_MINING_VIEW ", 0) == 0) {
                sendMiningView(fd, *line);
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
            if (line->rfind("GET_FACTORIZATION ", 0) == 0) {
                if (!factorization_helper_enabled_) {
                    writeAll(fd, "ERROR GET_FACTORIZATION disabled; restart with --enable-factorization-helper\n");
                    continue;
                }
                sendFactorization(fd, *line);
                continue;
            }
            if (*line == "GET_PEERS") {
                sendPeers(fd);
                continue;
            }
            if (*line == "GET_PEER_HEALTH") {
                if (rejectRemoteAdmin(fd, client_ip, client_loopback)) continue;
                sendPeerHealth(fd);
                continue;
            }
            if (*line == "GET_PEER_STATE") {
                if (rejectRemoteAdmin(fd, client_ip, client_loopback)) continue;
                sendPeerState(fd);
                continue;
            }
            if (*line == "RESET_PEER_STATE" || line->rfind("RESET_PEER_STATE ", 0) == 0) {
                if (rejectRemoteAdmin(fd, client_ip, client_loopback)) continue;
                resetPeerStateCommand(fd, *line);
                continue;
            }
            if (line->rfind("ADD_PEER ", 0) == 0) {
                if (rejectRemoteAdmin(fd, client_ip, client_loopback)) continue;
                addPeerCommand(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_TX ", 0) == 0) {
                submitTx(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_SIGNED_COMMIT ", 0) == 0) {
                submitSignedCommit(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_SIGNED_COMMIT_PEER ", 0) == 0) {
                submitSignedCommit(fd, *line, false);
                continue;
            }
            if (line->rfind("SUBMIT_COMMIT ", 0) == 0) {
                submitCommit(fd, *line);
                continue;
            }
            if (line->rfind("GET_COMMITMENTS ", 0) == 0) {
                sendCommitments(fd, *line);
                continue;
            }
            if (line->rfind("GET_COMMIT_WINNER ", 0) == 0) {
                sendCommitWinner(fd, *line);
                continue;
            }
            if (line->rfind("GET_COMMIT_PHASE ", 0) == 0) {
                sendCommitPhase(fd, *line);
                continue;
            }
            if (line->rfind("GET_PHASE_VOTES ", 0) == 0) {
                sendPhaseVotes(fd, *line);
                continue;
            }
            if (line->rfind("CLOSE_COMMIT_PHASE ", 0) == 0) {
                closeCommitPhase(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_PHASE_VOTE ", 0) == 0 ||
                line->rfind("SUBMIT_PHASE_VOTE_BUNDLE ", 0) == 0) {
                submitPhaseVote(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_PHASE_VOTE_PEER ", 0) == 0 ||
                line->rfind("SUBMIT_PHASE_VOTE_BUNDLE_PEER ", 0) == 0) {
                submitPhaseVote(fd, *line, false);
                continue;
            }
            if (*line == "GET_EPOCH_VOTES") {
                sendEpochVotes(fd);
                continue;
            }
            if (line->rfind("SUBMIT_EPOCH_VOTE ", 0) == 0) {
                submitEpochVote(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_EPOCH_VOTE_PEER ", 0) == 0) {
                submitEpochVote(fd, *line, false);
                continue;
            }
            if (*line == "GET_VALIDATOR_APPLICATIONS") {
                sendValidatorApplications(fd);
                continue;
            }
            if (*line == "GET_VALIDATOR_WORK_BINDINGS") {
                sendValidatorWorkBindings(fd);
                continue;
            }
            if (line->rfind("SUBMIT_VALIDATOR_APPLICATION ", 0) == 0) {
                submitValidatorApplication(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_VALIDATOR_APPLICATION_PEER ", 0) == 0) {
                submitValidatorApplication(fd, *line, false);
                continue;
            }
            if (line->rfind("SUBMIT_VALIDATOR_WORK_BINDING ", 0) == 0) {
                submitValidatorWorkBinding(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_VALIDATOR_WORK_BINDING_PEER ", 0) == 0) {
                submitValidatorWorkBinding(fd, *line, false);
                continue;
            }
            if (line->rfind("SUBMIT_VALIDATOR_ENDPOINT ", 0) == 0) {
                submitValidatorEndpoint(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_VALIDATOR_ENDPOINT_PEER ", 0) == 0) {
                submitValidatorEndpoint(fd, *line, false);
                continue;
            }
            if (*line == "GET_POLICY_VOTES") {
                sendPolicyVotes(fd);
                continue;
            }
            if (line->rfind("SUBMIT_POLICY_VOTE ", 0) == 0) {
                submitPolicyVote(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_POLICY_VOTE_PEER ", 0) == 0) {
                submitPolicyVote(fd, *line, false);
                continue;
            }
            if (line->rfind("SIGN_ROUND_CHANGE ", 0) == 0) {
                signRoundChange(fd, *line);
                continue;
            }
            if (line->rfind("SIGN_COMMIT_PHASE_TIMEOUT ", 0) == 0) {
                signCommitPhaseTimeout(fd, *line);
                continue;
            }
            if (line->rfind("TIMEOUT_COMMIT_PHASE ", 0) == 0) {
                timeoutCommitPhase(fd, *line);
                continue;
            }
            if (line->rfind("SIGN_RECORD_CANDIDATE ", 0) == 0) {
                signRecordCandidate(fd, *line);
                continue;
            }
            if (line->rfind("SIGN_COMPOSITE_LOTTERY ", 0) == 0) {
                signCompositeLottery(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_SIGNED_REVEAL ", 0) == 0) {
                submitSignedCompositeReveal(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_SIGNED_REVEAL_PEER ", 0) == 0) {
                submitSignedCompositeReveal(fd, *line, false);
                continue;
            }
            if (line->rfind("SUBMIT_COMPOSITE_REVEAL ", 0) == 0) {
                submitCompositeReveal(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_COMPOSITE ", 0) == 0) {
                submitComposite(fd, *line);
                continue;
            }
            if (line->rfind("SUBMIT_SIGNED_PRIME ", 0) == 0) {
                submitPrime(fd, *line, true);
                continue;
            }
            if (line->rfind("SUBMIT_SIGNED_PRIME_PEER ", 0) == 0) {
                submitPrime(fd, *line, false);
                continue;
            }
            if (line->rfind("SUBMIT_PRIME ", 0) == 0) {
                submitPrime(fd, *line, false);
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
            if (*line == "GET_MEMPOOL_SUMMARY") {
                sendMempoolSummary(fd);
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
            if (line->rfind("GET_NONCE ", 0) == 0) {
                sendNonce(fd, *line);
                continue;
            }
            ++invalid_command_count;
            recordClientViolation(client_ip, client_loopback);
            writeAll(fd, "ERROR unknown command\n");
            if (invalid_command_count >= kMaxInvalidCommandsPerConnection) {
                writeAll(fd, "ERROR rate limit exceeded: too many invalid commands on one connection\n");
                return;
            }
        }
    }

    bool syncFromPeer(const std::string& host, int port, std::string& error) {
        std::lock_guard<std::mutex> sync_lock(g_peer_sync_mutex);
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
        const std::uint64_t sync_id = ++g_peer_sync_counter;
        const std::string temp_path = store_path_ + ".sync." + std::to_string(getpid()) +
            "." + std::to_string(sync_id);
        removeStoreTempArtifacts(temp_path);
        if (!copyFileOrCreateEmpty(store_path_, temp_path, error)) {
            removeStoreTempArtifacts(temp_path);
            return false;
        }
        copyReplaySnapshotIfPresent(store_path_, temp_path);

        primechain::storage::RecordStore temp_store(temp_path);
        if (!downloadRecordRange(host, port, start, peer_status->frontier_integer, temp_store, error)) {
            removeStoreTempArtifacts(temp_path);
            return false;
        }

        primechain::node::SequentialNode reloaded(temp_path);
        if (!reloaded.load(error)) {
            removeStoreTempArtifacts(temp_path);
            return false;
        }
        if (!reloaded.status().has_genesis ||
            reloaded.status().frontier_integer != peer_status->frontier_integer) {
            error = "auto-sync replay frontier mismatch";
            removeStoreTempArtifacts(temp_path);
            return false;
        }
        if (quorumEnabled()) {
            std::vector<primechain::Address> anchored;
            if (!loadGenesisValidatorSet(temp_path, anchored, error) ||
                anchored != genesis_validator_set_) {
                if (error.empty()) error = "peer genesis validator set differs from configured validator set";
                removeStoreTempArtifacts(temp_path);
                return false;
            }
        }

        primechain::node::SequentialNode current(store_path_);
        if (!current.load(error)) {
            removeStoreTempArtifacts(temp_path);
            return false;
        }
        if (current.status().has_genesis != local.status().has_genesis ||
            current.status().frontier_integer != local.status().frontier_integer ||
            current.status().latest_record_hash != local.status().latest_record_hash) {
            if (current.status().has_genesis &&
                current.status().frontier_integer >= peer_status->frontier_integer) {
                removeStoreTempArtifacts(temp_path);
                return true;
            }
            error = "local store changed during peer sync";
            removeStoreTempArtifacts(temp_path);
            return false;
        }

        if (!store_.installValidatedStore(temp_path, error)) {
            removeStoreTempArtifacts(temp_path);
            return false;
        }
        removeStoreTempArtifacts(temp_path);
        validator_set_ = reloaded.validatorSet();
        if (use_chain_endpoints_) {
            loadChainEndpointPeers();
        }
        clearEpochVotesAfterRecord();
        clearEndpointUpdatesAfterRecord();
        clearPolicyVotesAfterRecord();
        revalidateMempool();
        removeStoreTempArtifacts(temp_path);
        return true;
    }

    bool loadCommitments(std::string& error) {
        const auto stored = commitment_store_.loadAll(error);
        if (!error.empty()) {
            return false;
        }

        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            return false;
        }
        const primechain::PrimeValue frontier =
            node.status().has_genesis ? node.status().frontier_integer : 2;
        const primechain::PrimeValue target = frontier + 1;
        commitments_.clear();
        bool pruned = false;
        for (const auto& commitment : stored) {
            if (commitment.integer != target) {
                pruned = true;
                continue;
            }
            if (commitment.public_key.empty()) {
                if (!primechain::protocol::isDevelopmentAddress(commitment.provider_address)) {
                    error = "invalid legacy provider address in commitment store";
                    return false;
                }
            } else {
                std::string verification_error;
                if (commitment.provider_address !=
                        primechain::crypto::addressFromProtocolPublicKey(commitment.public_key) ||
                    !primechain::crypto::verifyProtocolMessageSignature(
                        commitment.public_key,
                        primechain::crypto::compositeCommitSigningPayload(
                            commitment.integer,
                            commitment.commitment_hash,
                            commitment.provider_address),
                        commitment.signature,
                        verification_error)) {
                    error = "invalid signed commitment in commitment store";
                    return false;
                }
            }
            const auto key = std::make_tuple(commitment.integer, commitment.commit_round, commitment.provider_address);
            const auto inserted = commitments_.emplace(key, commitment);
            if (!inserted.second && inserted.first->second.commitment_hash != commitment.commitment_hash) {
                error = "conflicting provider commitments in commitment store";
                return false;
            }
            if (commitments_.size() > kMaxCompositeCommitments) {
                error = "commitment store exceeds configured limit";
                return false;
            }
        }
        if (pruned) {
            return persistCommitments(error);
        }
        return true;
    }

    bool syncFromKnownPeers(std::string& error) {
        return syncFromPeers(peers_, error);
    }

    bool acceptPeerMempoolTransaction(
        const primechain::protocol::TransactionV0& tx,
        std::string& error) {
        std::lock_guard<std::mutex> lock(mempool_mutex_);
        revalidateMempoolLocked();
        if (mempool_.size() >= kMaxMempoolTransactions) {
            error = "mempool full";
            return false;
        }

        if (!primechain::protocol::isProtocolFeePoolAddress(tx.sender_address) &&
            !primechain::protocol::isProtocolValidatorRewardPoolAddress(tx.sender_address) &&
            !primechain::protocol::verifyAuthenticatedTransactionSignature(tx, error)) {
            error = "invalid transaction signature: " + error;
            return false;
        }

        const auto hash = primechain::protocol::transactionHash(tx);
        std::size_t sender_pending = 0;
        for (const auto& existing : mempool_) {
            if (primechain::protocol::transactionHash(existing) == hash) {
                error.clear();
                return true;
            }
            if (existing.sender_address == tx.sender_address) {
                ++sender_pending;
                if (existing.nonce == tx.nonce) {
                    error = "conflicting transaction for sender nonce";
                    return false;
                }
            }
        }
        if (sender_pending >= kMaxMempoolTransactionsPerSender) {
            error = "sender mempool limit exceeded";
            return false;
        }

        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        auto pending = mempool_;
        pending.push_back(tx);
        error.clear();
        if (!node.validatePendingTransactions(pending, error)) {
            error = "invalid pending transaction: " + error;
            return false;
        }

        mempool_first_seen_[primechain::crypto::toHex(hash)] = currentUnixTime();
        mempool_.push_back(tx);
        return true;
    }

    bool syncMempoolFromPeerEndpoint(const PeerEndpoint& peer, std::string& error) {
        std::vector<primechain::protocol::TransactionV0> peer_transactions;
        if (!fetchMempoolFromPeer(peer.host, peer.port, peer_transactions, error)) {
            return false;
        }

        std::uint64_t accepted = 0;
        std::uint64_t skipped = 0;
        for (const auto& tx : peer_transactions) {
            std::string tx_error;
            const auto before = mempoolSize();
            if (acceptPeerMempoolTransaction(tx, tx_error)) {
                if (mempoolSize() > before) ++accepted;
                else ++skipped;
            } else {
                ++skipped;
                std::cerr << "mempool sync skipped transaction from "
                          << peer.host << ":" << peer.port << ": " << tx_error << "\n";
            }
        }
        error = "accepted=" + std::to_string(accepted) +
            " skipped=" + std::to_string(skipped);
        return true;
    }

    bool hasKnownPeers() const {
        return !peers_.empty();
    }

    const std::vector<primechain::Address>& activeValidatorSet() const {
        return validator_set_;
    }

    bool peerDiscoveryEnabled() const {
        return !quorumEnabled();
    }

    bool syncFromPeersPastInteger(primechain::PrimeValue integer, std::string& error) {
        if (!syncFromPeers(peers_, error)) return false;
        primechain::node::SequentialNode refreshed(store_path_);
        if (!refreshed.load(error)) return false;
        if (refreshed.status().has_genesis && refreshed.status().frontier_integer >= integer) {
            error.clear();
            return true;
        }
        error = "peer sync did not advance past integer " + std::to_string(integer);
        return false;
    }

    std::vector<PeerEndpoint> peersByDescendingFrontier(
        const std::vector<PeerEndpoint>& peers,
        std::string& error) {
        struct Candidate {
            PeerEndpoint peer;
            PeerStatus status;
            std::size_t order{0};
        };

        std::vector<Candidate> candidates;
        std::size_t order = 0;
        for (const auto& peer : peers) {
            std::string status_error;
            const auto status = requestPeerStatus(peer.host, peer.port, status_error);
            if (!status.has_value()) {
                markPeerFailure(peer, status_error);
                std::cerr << "peer status warning from " << peer.host << ":" << peer.port
                          << ": " << status_error << "\n";
                ++order;
                continue;
            }
            markPeerSuccess(peer);
            candidates.push_back({peer, *status, order++});
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
            if (left.status.has_genesis != right.status.has_genesis) {
                return left.status.has_genesis && !right.status.has_genesis;
            }
            if (left.status.frontier_integer != right.status.frontier_integer) {
                return left.status.frontier_integer > right.status.frontier_integer;
            }
            return left.order < right.order;
        });

        std::vector<PeerEndpoint> ordered;
        ordered.reserve(candidates.size());
        for (const auto& candidate : candidates) ordered.push_back(candidate.peer);
        if (ordered.empty()) error = "no reachable sync peers";
        else error.clear();
        return ordered;
    }

    bool syncFromPeers(const std::vector<PeerEndpoint>& peers, std::string& error) {
        auto ordered_peers = peersByDescendingFrontier(peers, error);
        bool synced_any = false;
        std::size_t attempted = 0;
        for (const auto& peer : ordered_peers) {
            ++attempted;
            error.clear();
            if (syncFromPeer(peer.host, peer.port, error)) {
                markPeerSuccess(peer);
                synced_any = true;
                std::string commitment_error;
                if (!syncCommitmentsFromPeer(peer.host, peer.port, commitment_error)) {
                    std::cerr << "commitment sync warning from " << peer.host << ":" << peer.port
                              << ": " << commitment_error << "\n";
                }
                if (quorumEnabled()) {
                    std::string phase_error;
                    if (!syncPhaseVotesFromPeer(peer.host, peer.port, phase_error)) {
                        std::cerr << "phase vote sync warning from " << peer.host << ":" << peer.port
                                  << ": " << phase_error << "\n";
                    }
                }
                std::string mempool_error;
                if (!syncMempoolFromPeerEndpoint(peer, mempool_error)) {
                    std::cerr << "mempool sync warning from " << peer.host << ":" << peer.port
                              << ": " << mempool_error << "\n";
                }
                continue;
            }
            markPeerFailure(peer, error);
            std::cerr << "peer sync warning from " << peer.host << ":" << peer.port
                      << ": " << error << "\n";
        }
        if (synced_any || attempted == 0) {
            error.clear();
            return true;
        }
        return false;
    }

    void discoverPeersFromKnown() {
        const auto snapshot = activeKnownPeers();
        for (const auto& peer : snapshot) {
            std::string error;
            const auto discovered = requestPeerList(peer.host, peer.port, error);
            if (!error.empty()) {
                markPeerFailure(peer, error);
                std::cerr << "peer discovery warning from " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
                continue;
            }
            markPeerSuccess(peer);
            bool peer_added = false;
            for (const auto& discovered_peer : discovered) {
                const auto before = peers_.size();
                if (addPeer(discovered_peer) && peers_.size() > before) peer_added = true;
            }
            if (peer_added) persistPeerStateWarning();
        }
    }

    bool hasPendingMempool() {
        std::lock_guard<std::mutex> lock(mempool_mutex_);
        revalidateMempoolLocked();
        return !mempool_.empty();
    }

    void rebroadcastMempool() {
        std::vector<primechain::protocol::TransactionV0> snapshot;
        {
            std::lock_guard<std::mutex> lock(mempool_mutex_);
            revalidateMempoolLocked();
            snapshot = mempool_;
        }
        for (const auto& tx : snapshot) {
            propagateTransaction(tx);
        }
    }

    bool loadPhaseVotes(std::string& error) {
        return loadPhaseVotesInternal(error);
    }

    bool loadEpochVotes(std::string& error) {
        return loadEpochVotesInternal(error);
    }

    bool loadFinalizationVotes(std::string& error) {
        if (!loadFinalizationVotesInternal(error) || !loadRoundChangesInternal(error)) {
            return false;
        }
        for (const auto& entry : signed_candidates_) {
            if (entry.first.second > activeFinalizationRound(std::get<0>(entry.first))) {
                error = "persisted finalization vote has no certified round change";
                return false;
            }
        }
        return true;
    }

private:
    std::vector<primechain::storage::StoredCommitment> commitmentSnapshot() const {
        std::vector<primechain::storage::StoredCommitment> out;
        out.reserve(commitments_.size());
        for (const auto& entry : commitments_) {
            out.push_back(entry.second);
        }
        return out;
    }

    bool persistCommitments(std::string& error) const {
        return commitment_store_.replaceAll(commitmentSnapshot(), error);
    }

    bool syncCommitmentsFromPeer(const std::string& host, int port, std::string& error) {
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            return false;
        }
        const primechain::PrimeValue frontier =
            node.status().has_genesis ? node.status().frontier_integer : 2;
        const primechain::PrimeValue target = frontier + 1;
        bool changed = false;
        for (auto it = commitments_.begin(); it != commitments_.end();) {
            if (std::get<0>(it->first) != target) {
                it = commitments_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed && !persistCommitments(error)) {
            return false;
        }
        changed = false;
        const auto commit_round = activeCommitPhaseRound(target);
        const auto remote = requestCommitments(host, port, target, error);
        if (!error.empty()) {
            return false;
        }

        for (const auto& commitment : remote) {
            const auto key = std::make_tuple(commitment.integer, commitment.commit_round, commitment.provider_address);
            const auto existing = commitments_.find(key);
            if (existing != commitments_.end()) {
                if (existing->second.commitment_hash != commitment.commitment_hash ||
                    existing->second.public_key != commitment.public_key ||
                    existing->second.signature != commitment.signature) {
                    error = "peer supplied conflicting commitment for provider";
                    return false;
                }
                continue;
            }
            if (commitments_.size() >= kMaxCompositeCommitments) {
                error = "commitment pool full during peer sync";
                return false;
            }
            commitments_[key] = commitment;
            changed = true;
        }
        return !changed || persistCommitments(error);
    }

    bool syncPhaseVotesFromPeer(const std::string& host, int port, std::string& error) {
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const primechain::PrimeValue target =
            (node.status().has_genesis ? node.status().frontier_integer : 2) + 1;
        const auto remote = requestPhaseVotes(host, port, target, error);
        if (!error.empty()) return false;
        for (const auto& vote : remote) {
            if (!acceptPhaseVote(vote, error, false)) return false;
        }
        return true;
    }

    void pruneFinalizedCommitments(primechain::PrimeValue finalized_integer) {
        bool changed = false;
        for (auto it = commitments_.begin(); it != commitments_.end();) {
            if (std::get<0>(it->first) <= finalized_integer) {
                it = commitments_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) {
            std::string error;
            if (!persistCommitments(error)) {
                std::cerr << "commitment persistence warning: " << error << "\n";
            }
        }
        bool phase_changed = false;
        for (auto it = phase_votes_.begin(); it != phase_votes_.end();) {
            if (std::get<0>(it->first) <= finalized_integer) {
                it = phase_votes_.erase(it);
                phase_changed = true;
            } else {
                ++it;
            }
        }
        if (phase_changed) {
            std::string error;
            if (!persistPhaseVotes(error)) {
                std::cerr << "phase vote persistence warning: " << error << "\n";
            }
        }
        for (auto it = pending_reveals_.begin(); it != pending_reveals_.end();) {
            if (std::get<0>(it->first) <= finalized_integer) {
                it = pending_reveals_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool isSelfPeer(const PeerEndpoint& peer) const {
        if (peer.port != listen_port_) return false;
        if (peer.host == bind_address_) return true;
        if (peer.host == "127.0.0.1" || peer.host == "0.0.0.0") return true;
        return false;
    }

    PeerRuntimeState& peerRuntime(const PeerEndpoint& peer) {
        return peer_state_[peerKey(peer)];
    }

    const PeerRuntimeState* findPeerRuntime(const PeerEndpoint& peer) const {
        const auto found = peer_state_.find(peerKey(peer));
        return found == peer_state_.end() ? nullptr : &found->second;
    }

    bool peerQuarantined(const PeerEndpoint& peer) const {
        const auto* state = findPeerRuntime(peer);
        return state != nullptr && state->quarantined;
    }

    void markPeerSuccess(const PeerEndpoint& peer) {
        auto& state = peerRuntime(peer);
        state.consecutive_failures = 0;
        state.quarantined = false;
        state.last_success_time = currentUnixTime();
        state.last_error.clear();
        persistPeerStateWarning();
    }

    void markPeerFailure(const PeerEndpoint& peer, const std::string& error) {
        auto& state = peerRuntime(peer);
        ++state.consecutive_failures;
        state.last_failure_time = currentUnixTime();
        state.last_error = error.empty() ? "unknown" : error;
        if (state.consecutive_failures >= kPeerQuarantineFailureThreshold) {
            state.quarantined = true;
        }
        persistPeerStateWarning();
    }

    std::vector<PeerEndpoint> activePeers(const std::vector<PeerEndpoint>& peers) const {
        std::vector<PeerEndpoint> out;
        out.reserve(peers.size());
        for (const auto& peer : peers) {
            if (!peerQuarantined(peer)) out.push_back(peer);
        }
        return out;
    }

    std::vector<PeerEndpoint> activeKnownPeers() const {
        return activePeers(peers_);
    }

    std::string peerStatePath() const {
        return store_path_ + ".peers";
    }

    bool persistPeerState(std::string& error) const {
        const auto path = peerStatePath();
        const auto tmp_path = path + ".tmp";
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            error = "could not open peer state temp file";
            return false;
        }
        out << "# primechain peer state v1\n";
        for (const auto& peer : peers_) {
            PeerRuntimeState state;
            const auto* found = findPeerRuntime(peer);
            if (found != nullptr) state = *found;
            out << "PEER " << peer.host << " " << peer.port
                << " " << state.consecutive_failures
                << " " << (state.quarantined ? 1 : 0)
                << " " << state.last_success_time
                << " " << state.last_failure_time
                << " " << healthToken(state.last_error) << "\n";
        }
        out.close();
        if (!out) {
            error = "could not write peer state temp file";
            return false;
        }
        if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
            error = std::string("could not replace peer state file: ") + std::strerror(errno);
            std::remove(tmp_path.c_str());
            return false;
        }
        error.clear();
        return true;
    }

    bool loadPeerState(std::string& error) {
        std::ifstream in(peerStatePath());
        if (!in) {
            error.clear();
            return true;
        }
        std::string line;
        std::size_t loaded = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream parser(line);
            std::string tag;
            PeerEndpoint peer;
            PeerRuntimeState state;
            int quarantined = 0;
            parser >> tag >> peer.host >> peer.port
                   >> state.consecutive_failures
                   >> quarantined
                   >> state.last_success_time
                   >> state.last_failure_time
                   >> state.last_error;
            if (!parser || tag != "PEER" || !validPeerEndpoint(peer) || isSelfPeer(peer)) {
                continue;
            }
            state.quarantined = quarantined != 0;
            const auto before = peers_.size();
            addPeer(peer);
            if (peers_.size() > before || findPeerRuntime(peer) != nullptr) {
                peer_state_[peerKey(peer)] = state;
                ++loaded;
            }
        }
        if (!in.eof()) {
            error = "could not read peer state file";
            return false;
        }
        error.clear();
        return true;
    }

    void persistPeerStateWarning() const {
        std::string error;
        if (!persistPeerState(error)) {
            std::cerr << "peer state persistence warning: " << error << "\n";
        }
    }

    bool addPeer(const PeerEndpoint& peer) {
        if (!validPeerEndpoint(peer) || isSelfPeer(peer)) {
            return false;
        }
        const auto found = std::find_if(peers_.begin(), peers_.end(), [&](const PeerEndpoint& existing) {
            return samePeer(existing, peer);
        });
        if (found != peers_.end()) {
            peerRuntime(peer);
            return true;
        }
        if (peers_.size() >= kMaxKnownPeers) {
            return false;
        }
        peers_.push_back(peer);
        peerRuntime(peer);
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


    void sendPeerHealth(int fd) {
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const auto& local = node.status();
        const auto local_hash = primechain::crypto::toHex(local.latest_record_hash);

        std::ostringstream out;
        out << "PEER_HEALTH " << peers_.size()
            << " local_frontier=" << local.frontier_integer
            << " local_hash=" << local_hash << "\n";
        for (const auto& peer : peers_) {
            std::string status_error;
            const auto status = requestPeerStatus(peer.host, peer.port, status_error);
            if (!status.has_value()) {
                markPeerFailure(peer, status_error);
                const auto& state = peerRuntime(peer);
                out << "PEER_HEALTH_ENTRY"
                    << " host=" << peer.host
                    << " port=" << peer.port
                    << " reachable=0"
                    << " failures=" << state.consecutive_failures
                    << " quarantined=" << (state.quarantined ? 1 : 0)
                    << " last_success=" << state.last_success_time
                    << " last_failure=" << state.last_failure_time
                    << " last_error=" << healthToken(state.last_error)
                    << " error=" << healthToken(status_error) << "\n";
                continue;
            }

            std::string list_error;
            const auto listed_peers = requestPeerList(peer.host, peer.port, list_error);
            const auto peer_hash = primechain::crypto::toHex(status->latest_record_hash);
            const auto hash_match = peer_hash == local_hash;
            const auto peer_list_ok = list_error.empty();
            if (peer_list_ok) {
                markPeerSuccess(peer);
            } else {
                markPeerFailure(peer, list_error);
            }
            const auto& state = peerRuntime(peer);
            const auto frontier_delta = static_cast<std::int64_t>(status->frontier_integer) -
                static_cast<std::int64_t>(local.frontier_integer);
            out << "PEER_HEALTH_ENTRY"
                << " host=" << peer.host
                << " port=" << peer.port
                << " reachable=1"
                << " failures=" << state.consecutive_failures
                << " quarantined=" << (state.quarantined ? 1 : 0)
                << " last_success=" << state.last_success_time
                << " last_failure=" << state.last_failure_time
                << " last_error=" << (state.last_error.empty() ? "none" : healthToken(state.last_error))
                << " has_genesis=" << (status->has_genesis ? 1 : 0)
                << " frontier=" << status->frontier_integer
                << " height=" << status->height
                << " hash=" << peer_hash
                << " hash_match=" << (hash_match ? 1 : 0)
                << " frontier_delta=" << frontier_delta
                << " peer_list_ok=" << (peer_list_ok ? 1 : 0)
                << " peer_count=" << (peer_list_ok ? listed_peers.size() : 0);
            if (!list_error.empty()) out << " peer_list_error=" << healthToken(list_error);
            out << "\n";
        }
        out << "END_PEER_HEALTH\n";
        writeAll(fd, out.str());
    }

    void sendPeerState(int fd) const {
        std::ostringstream out;
        out << "PEER_STATE path=" << peerStatePath()
            << " peers=" << peers_.size()
            << " quarantine_threshold=" << kPeerQuarantineFailureThreshold << "\n";
        for (const auto& peer : peers_) {
            PeerRuntimeState state;
            const auto* found = findPeerRuntime(peer);
            if (found != nullptr) state = *found;
            out << "PEER_STATE_ENTRY"
                << " host=" << peer.host
                << " port=" << peer.port
                << " failures=" << state.consecutive_failures
                << " quarantined=" << (state.quarantined ? 1 : 0)
                << " last_success=" << state.last_success_time
                << " last_failure=" << state.last_failure_time
                << " last_error=" << (state.last_error.empty() ? "none" : healthToken(state.last_error))
                << "\n";
        }
        out << "END_PEER_STATE\n";
        writeAll(fd, out.str());
    }

    void resetPeerStateCommand(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        in >> command;
        PeerEndpoint target;
        std::string extra;
        const bool has_host = static_cast<bool>(in >> target.host);
        const bool has_port = has_host && static_cast<bool>(in >> target.port);
        if (command != "RESET_PEER_STATE" || (has_host && !has_port) ||
            (has_port && !validPeerEndpoint(target)) || (in >> extra)) {
            writeAll(fd, "ERROR invalid RESET_PEER_STATE\n");
            return;
        }

        std::size_t reset = 0;
        for (const auto& peer : peers_) {
            if (has_port && !samePeer(peer, target)) continue;
            auto& state = peerRuntime(peer);
            state.consecutive_failures = 0;
            state.quarantined = false;
            state.last_error.clear();
            ++reset;
        }
        persistPeerStateWarning();
        writeAll(fd, "PEER_STATE_RESET " + std::to_string(reset) + "\n");
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

        if (isSelfPeer(peer)) {
            writeAll(fd, "ERROR self peer not allowed\n");
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
        persistPeerStateWarning();
        writeAll(fd, "PEER_ADDED " + peer.host + " " + std::to_string(peer.port) + "\n");
    }

    void sendStatus(int fd) const {
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        primechain::storage::RecordKindCounts counts;
        const auto& status = node.status();
        const auto latest = store_.latest(error);
        if (!error.empty()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (latest.has_value() && !store_.countRangeByKind(2, latest->integer, counts, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        std::ostringstream out;
        out << "STATUS "
            << counts.total << " "
            << counts.prime << " "
            << counts.composite << " "
            << (status.has_genesis ? 1 : 0) << " "
            << status.height << " "
            << status.frontier_integer << " "
            << primechain::crypto::toHex(status.latest_record_hash) << "\n";
        writeAll(fd, out.str());
    }

    void sendValidators(int fd) const {
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        std::ostringstream out;
        out << "VALIDATORS " << node.validatorSet().size();
        for (const auto& validator : node.validatorSet()) out << " " << validator;
        out << "\n";
        writeAll(fd, out.str());
    }

    void sendValidatorEpoch(int fd) const {
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        std::ostringstream out;
        out << "VALIDATOR_EPOCH " << node.validatorEpoch() << " "
            << (node.status().frontier_integer + 1) << " "
            << primechain::crypto::toHex(node.status().latest_record_hash) << "\n";
        writeAll(fd, out.str());
    }

    void sendEconomicPolicy(int fd) const {
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        std::ostringstream out;
        out << "ECONOMIC_POLICY transfer_fee_micro_units=" << node.transferFeeMicroUnits()
            << " validator_min_reserve_micro_units=" << node.validatorMinReserveMicroUnits()
            << " next_integer=" << (node.status().frontier_integer + 1)
            << " previous_hash=" << primechain::crypto::toHex(node.status().latest_record_hash)
            << "\n";
        writeAll(fd, out.str());
    }

    void sendValidatorEndpoints(int fd) const {
        std::string error;
        primechain::storage::RecordStore store(store_path_);
        const auto records = store.loadAll(error);
        if (!error.empty()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        std::map<primechain::Address, primechain::protocol::ValidatorEndpointUpdateV1> latest;
        for (const auto& stored : records) {
            std::optional<primechain::protocol::PrimeRecordV0> prime;
            std::optional<primechain::protocol::CompositeRecordV0> composite;
            if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
                prime = primechain::protocol::deserializePrimeRecord(stored.payload, error);
                if (!prime.has_value()) { writeAll(fd, "ERROR " + error + "\n"); return; }
                for (const auto& update : prime->validator_endpoints) latest[update.validator_address] = update;
            } else {
                composite = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
                if (!composite.has_value()) { writeAll(fd, "ERROR " + error + "\n"); return; }
                for (const auto& update : composite->validator_endpoints) latest[update.validator_address] = update;
            }
        }
        std::ostringstream out;
        out << "VALIDATOR_ENDPOINTS " << latest.size() << "\n";
        for (const auto& entry : latest) {
            const auto& update = entry.second;
            out << "VALIDATOR_ENDPOINT " << update.validator_address << " "
                << update.host << " " << update.port << " "
                << update.effective_integer << " " << update.sequence << "\n";
        }
        out << "END_VALIDATOR_ENDPOINTS\n";
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
        writeCommand(fd, recordLine(*record));
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

        std::lock_guard<std::mutex> range_lock(g_record_range_mutex);
        std::string error;
        if (!store_.hasContiguousRange(start, end, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        const auto expected_count = end - start + 1;
        std::ostringstream header;
        header << "RECORD_RANGE " << start << " " << end << " " << expected_count << "\n";
        if (!writeAll(fd, header.str())) return;

        bool write_failed = false;
        const bool streamed = store_.forEachRange(start, end,
            [&](const primechain::storage::StoredRecord& record) {
                if (!writeCommand(fd, recordLine(record))) {
                    write_failed = true;
                    return false;
                }
                return true;
            }, error);
        if (!streamed) {
            if (!write_failed) writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        writeAll(fd, "END_RECORD_RANGE\n");
    }

    void sendFactorization(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue n = 0;
        in >> command >> n;
        std::string extra;
        if (!in || command != "GET_FACTORIZATION" || n < 2 || (in >> extra)) {
            writeAll(fd, "ERROR invalid GET_FACTORIZATION; expected GET_FACTORIZATION n\n");
            return;
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (!node.status().has_genesis || n > node.status().frontier_integer) {
            writeAll(fd, "ERROR factorization helper only serves integers at or below the local frontier\n");
            return;
        }

        MapProofIndex proofs;
        if (!loadCompositeProofIndex(store_, proofs, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        const auto factorization = primechain::math::factorizeFromProofIndex(n, proofs);
        if (!factorization.has_value()) {
            writeAll(fd, "ERROR factorization unavailable\n");
            return;
        }

        const auto product = primechain::math::multiplyFactorization(*factorization);
        if (!primechain::math::isCanonicalFactorization(*factorization) ||
            !product.has_value() ||
            *product != n) {
            writeAll(fd, "ERROR internal factorization validation failed\n");
            return;
        }

        std::ostringstream out;
        out << "FACTORIZATION " << n << " FACTORS " << factorization->factors.size();
        for (const auto& factor : factorization->factors) {
            out << " PRIME " << factor.prime << " EXP " << factor.exponent;
        }
        out << "\n";
        writeAll(fd, out.str());
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
        if (!primechain::protocol::isProtocolFeePoolAddress(tx->sender_address) &&
            !primechain::protocol::isProtocolValidatorRewardPoolAddress(tx->sender_address) &&
            !primechain::protocol::verifyAuthenticatedTransactionSignature(*tx, error)) {
            writeAll(fd, "ERROR invalid transaction signature: " + error + "\n");
            return;
        }

        const auto hash = primechain::protocol::transactionHash(*tx);
        {
            std::lock_guard<std::mutex> lock(mempool_mutex_);
            revalidateMempoolLocked();
            if (mempool_.size() >= kMaxMempoolTransactions) {
                writeAll(fd, "ERROR mempool full; max="
                    + std::to_string(kMaxMempoolTransactions)
                    + "\n");
                return;
            }

            std::size_t sender_pending = 0;
            for (const auto& existing : mempool_) {
                if (primechain::protocol::transactionHash(existing) == hash) {
                    writeAll(fd, "TX_DUPLICATE " + primechain::crypto::toHex(hash) + "\n");
                    return;
                }
                if (existing.sender_address == tx->sender_address) {
                    ++sender_pending;
                    if (existing.nonce == tx->nonce) {
                        writeAll(fd, "ERROR conflicting transaction for sender nonce\n");
                        return;
                    }
                }
            }
            if (sender_pending >= kMaxMempoolTransactionsPerSender) {
                writeAll(fd, "ERROR sender mempool limit exceeded; max=" +
                    std::to_string(kMaxMempoolTransactionsPerSender) + "\n");
                return;
            }

            primechain::node::SequentialNode node(store_path_);
            if (!node.load(error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            auto pending = mempool_;
            pending.push_back(*tx);
            error.clear();
            if (!node.validatePendingTransactions(pending, error)) {
                writeAll(fd, "ERROR invalid pending transaction: " + error + "\n");
                return;
            }

            mempool_first_seen_[primechain::crypto::toHex(hash)] = currentUnixTime();
            mempool_.push_back(*tx);
        }
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

    bool validateQuorumCompositeRecord(
        const primechain::storage::StoredRecord& submitted,
        std::string& error) const {
        if (!quorumEnabled() ||
            submitted.kind != primechain::storage::StoredRecordKind::Composite) {
            return true;
        }
        const auto record = primechain::protocol::deserializeCompositeRecord(
            submitted.payload, error);
        if (!record.has_value()) return false;
        if (record->version < 1) {
            error = "quorum mode requires a supported composite record version";
            return false;
        }
        if (record->version <= primechain::node::kValidatorRewardRecordVersion &&
            record->commit_phase.validator_set != validator_set_) {
            error = "embedded validator set differs from configured validator set";
            return false;
        }
        return primechain::protocol::verifyCommitPhaseCertificate(*record, error);
    }

    bool validateQuorumFinalizationCommittee(
        const primechain::storage::StoredRecord& submitted,
        std::string& error) const {
        if (!quorumEnabled()) return true;

        primechain::protocol::FinalizationProofV0 proof;
        primechain::Hash256 previous_hash{};
        primechain::Hash256 candidate_hash{};
        primechain::PrimeValue integer = 0;
        if (submitted.kind == primechain::storage::StoredRecordKind::Composite) {
            const auto record = primechain::protocol::deserializeCompositeRecord(
                submitted.payload, error);
            if (!record.has_value()) return false;
            proof = record->finalized_by;
            previous_hash = record->previous_record_hash;
            candidate_hash = record->version >= primechain::node::kSubjectHashRecordVersion
                ? primechain::protocol::subjectRecordHash(*record)
                : (record->finalized_by.rule == "fixed-2-of-3-mldsa65-rounds-locks-v4"
                    ? primechain::protocol::legacyCandidateRecordHashWithoutFinalization(*record)
                    : primechain::protocol::candidateRecordHash(*record));
            integer = record->integer;
        } else {
            const auto record = primechain::protocol::deserializePrimeRecord(
                submitted.payload, error);
            if (!record.has_value()) return false;
            if (record->height == 0) return true;
            proof = record->finalized_by;
            previous_hash = record->previous_record_hash;
            candidate_hash = record->version >= primechain::node::kSubjectHashRecordVersion
                ? primechain::protocol::subjectRecordHash(*record)
                : (record->finalized_by.rule == "fixed-2-of-3-mldsa65-rounds-locks-v4"
                    ? primechain::protocol::legacyCandidateRecordHashWithoutFinalization(*record)
                    : primechain::protocol::candidateRecordHash(*record));
            integer = record->integer;
        }

        if (!primechain::protocol::verifyRecordFinalization(
                proof, candidate_hash, previous_hash, integer, validator_set_, error)) {
            return false;
        }
        return finalizationVotesMatchAssignedCommittee(
            proof, previous_hash, integer, error);
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
            if (quorumEnabled()) {
                const auto existing = store_.findByInteger(submitted->integer, error);
                if (!error.empty()) {
                    writeAll(fd, "ERROR " + error + "\n");
                    return;
                }
                if (!existing.has_value()) {
                    writeAll(fd, "ERROR finalized quorum record is immutable\n");
                    return;
                }
                if (existing->record_hash != submitted->record_hash &&
                    submitted->integer < node.status().frontier_integer) {
                    writeAll(fd, "ERROR finalized quorum record is immutable\n");
                    return;
                }
            }
            handleExistingOrConflictingRecord(fd, *submitted, node.status().frontier_integer);
            return;
        }

        error.clear();
        if (!validateQuorumCompositeRecord(*submitted, error) ||
            !validateQuorumFinalizationCommittee(*submitted, error)) {
            writeAll(fd, "ERROR invalid quorum record: " + error + "\n");
            return;
        }
        error.clear();
        if (!appendStoredRecord(node, *submitted, error)) {
            writeAll(fd, "ERROR could not append submitted record: " + error + "\n");
            return;
        }
        validator_set_ = node.validatorSet();
        clearSignedCandidate(submitted->integer);
        revalidateMempool();

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
        if (existing.has_value() && quorumEnabled()) {
            if (submitted.integer == frontier_integer) {
                error.clear();
                const auto same_subject = recordsShareFinalizationSubject(*existing, submitted, error);
                if (!same_subject.has_value()) {
                    writeAll(fd, "ERROR could not compare quorum tip replacement: " + error + "\n");
                    return;
                }
                if (*same_subject) {
                    error.clear();
                    const auto existing_round = recordFinalizationRound(*existing, validator_set_, error);
                    if (!existing_round.has_value()) {
                        writeAll(fd, "ERROR could not verify existing tip finalization round: " + error + "\n");
                        return;
                    }
                    error.clear();
                    const auto submitted_round = recordFinalizationRound(submitted, validator_set_, error);
                    if (!submitted_round.has_value()) {
                        writeAll(fd, "ERROR could not verify submitted tip finalization round: " + error + "\n");
                        return;
                    }
                    if (*submitted_round > *existing_round) {
                        error.clear();
                        const auto validated =
                            validateTipReplacementCandidate(store_path_, *existing, submitted, error);
                        if (!validated.has_value()) {
                            writeAll(fd, "ERROR invalid newer-round tip replacement: " + error + "\n");
                            return;
                        }
                        error.clear();
                        if (!store_.replaceTip(existing->record_hash, *validated, error)) {
                            writeAll(fd, "ERROR could not replace tip: " + error + "\n");
                            return;
                        }
                        primechain::node::SequentialNode reloaded(store_path_);
                        if (!reloaded.load(error)) {
                            writeAll(fd, "ERROR could not replay replaced tip: " + error + "\n");
                            return;
                        }
                        validator_set_ = reloaded.validatorSet();
                        clearEpochVotesAfterRecord();
                        clearSignedCandidate(submitted.integer);
                        revalidateMempool();
                        propagateRecord(*validated);
                        writeAll(fd, "RECORD_REPLACED "
                            + primechain::crypto::toHex(validated->record_hash)
                            + " "
                            + primechain::crypto::toHex(existing->record_hash)
                            + "\n");
                        return;
                    }
                }
            }
            writeAll(fd, "RECORD_CONFLICT_WORSE "
                + primechain::crypto::toHex(submitted.record_hash)
                + " "
                + primechain::crypto::toHex(existing->record_hash)
                + "\n");
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
                primechain::node::SequentialNode reloaded(store_path_);
                if (!reloaded.load(error)) {
                    writeAll(fd, "ERROR could not replay replaced tip: " + error + "\n");
                    return;
                }
                validator_set_ = reloaded.validatorSet();
                clearEpochVotesAfterRecord();
                revalidateMempool();
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

    std::vector<primechain::storage::SignedCandidateRecord> signedCandidateSnapshot() const {
        std::vector<primechain::storage::SignedCandidateRecord> out;
        for (const auto& entry : signed_candidates_) out.push_back(entry.second);
        return out;
    }

    bool persistSignedCandidates(std::string& error) const {
        return finalization_store_.replaceAll(signedCandidateSnapshot(), error);
    }

    std::vector<primechain::protocol::RoundChangeVoteV1> roundChangeSnapshot() const {
        std::vector<primechain::protocol::RoundChangeVoteV1> out;
        for (const auto& entry : round_changes_) out.push_back(entry.second);
        return out;
    }

    bool persistRoundChanges(std::string& error) const {
        return round_change_store_.replaceAll(roundChangeSnapshot(), error);
    }

    std::vector<primechain::protocol::RoundChangeVoteV1> certifiedRoundChanges(
        primechain::PrimeValue integer,
        std::uint64_t round) const {
        std::vector<primechain::protocol::RoundChangeVoteV1> out;
        for (const auto& entry : round_changes_) {
            if (std::get<0>(entry.first) == integer && std::get<1>(entry.first) == round) {
                out.push_back(entry.second);
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
            return left.validator_address < right.validator_address;
        });
        if (out.size() > validator_set_.size()) out.resize(validator_set_.size());
        return out;
    }

    std::uint64_t activeFinalizationRound(primechain::PrimeValue integer) const {
        if (!quorumEnabled()) return 1;
        std::uint64_t active = 1;
        for (const auto& entry : round_changes_) {
            if (std::get<0>(entry.first) != integer) continue;
            const auto round = std::get<1>(entry.first);
            if (certifiedRoundChanges(integer, round).size() >= validatorQuorumRequired()) active = std::max(active, round);
        }
        return active;
    }

    bool verifyRoundChangeVote(
        const primechain::protocol::RoundChangeVoteV1& vote,
        std::string& error) const {
        error.clear();
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const auto target = node.status().frontier_integer + 1;
        const auto active_round = activeFinalizationRound(target);
        if (vote.integer != target || vote.previous_record_hash != node.status().latest_record_hash ||
            vote.new_round < std::max<std::uint64_t>(2, active_round) ||
            vote.new_round > active_round + 1 ||
            !std::binary_search(validator_set_.begin(), validator_set_.end(), vote.validator_address) ||
            vote.validator_address != primechain::crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "invalid round-change vote target";
            return false;
        }
        if (vote.locked_round == 0) {
            if (!vote.locked_candidate_kind.empty() ||
                vote.locked_candidate_payload.size() != 0 ||
                std::any_of(vote.locked_candidate_hash.begin(), vote.locked_candidate_hash.end(),
                    [](std::uint8_t byte) { return byte != 0; })) {
                error = "empty round-change lock carries candidate data";
                return false;
            }
        } else {
            const auto kind = parseStoredKindName(vote.locked_candidate_kind);
            if (!kind.has_value() || vote.locked_round >= vote.new_round || vote.locked_candidate_payload.empty()) {
                error = "invalid round-change lock";
                return false;
            }
            std::string subject_error;
            const auto subject = subjectHashFromCandidatePayload(*kind, vote.locked_candidate_payload, subject_error);
            if (!subject.has_value() || *subject != vote.locked_candidate_hash) {
                error = subject_error.empty() ? "round-change lock subject mismatch" : subject_error;
                return false;
            }
        }
        return primechain::crypto::verifyProtocolMessageSignature(
            vote.public_key,
            primechain::crypto::lockedRoundChangeVoteSigningPayload(
                vote.previous_record_hash, vote.integer, vote.new_round,
                vote.locked_round, vote.locked_candidate_kind,
                vote.locked_candidate_hash, vote.locked_candidate_payload,
                vote.validator_address),
            vote.signature, error);
    }

    bool acceptRoundChangeVote(
        const primechain::protocol::RoundChangeVoteV1& vote,
        std::string& error) {
        if (!verifyRoundChangeVote(vote, error)) return false;
        return acceptVerifiedRoundChangeVote(vote, error);
    }

    bool acceptVerifiedRoundChangeVote(
        const primechain::protocol::RoundChangeVoteV1& vote,
        std::string& error) {
        const auto key = std::make_tuple(vote.integer, vote.new_round, vote.validator_address);
        const auto existing = round_changes_.find(key);
        if (existing != round_changes_.end()) {
            if (existing->second.public_key == vote.public_key &&
                existing->second.locked_round == vote.locked_round &&
                existing->second.locked_candidate_kind == vote.locked_candidate_kind &&
                existing->second.locked_candidate_hash == vote.locked_candidate_hash &&
                existing->second.locked_candidate_payload == vote.locked_candidate_payload) return true;
            error = "validator already submitted a different round-change vote";
            return false;
        }
        round_changes_[key] = vote;
        if (!persistRoundChanges(error)) {
            round_changes_.erase(key);
            return false;
        }
        return true;
    }

    bool importCertifiedRoundChanges(
        const primechain::protocol::FinalizationProofV0& proof,
        std::string& error) {
        if (proof.round_changes.empty()) return true;
        bool changed = false;
        std::vector<std::tuple<primechain::PrimeValue, std::uint64_t, primechain::Address>> inserted;
        for (const auto& change : proof.round_changes) {
            const auto key = std::make_tuple(change.integer, change.new_round, change.validator_address);
            const auto existing = round_changes_.find(key);
            if (existing != round_changes_.end()) {
                if (existing->second.public_key == change.public_key &&
                    existing->second.locked_round == change.locked_round &&
                    existing->second.locked_candidate_kind == change.locked_candidate_kind &&
                    existing->second.locked_candidate_hash == change.locked_candidate_hash &&
                    existing->second.locked_candidate_payload == change.locked_candidate_payload) {
                    continue;
                }
                error = "conflicting certified round-change vote";
                return false;
            }
            round_changes_[key] = change;
            inserted.push_back(key);
            changed = true;
        }
        if (!changed) return true;
        if (!persistRoundChanges(error)) {
            for (const auto& key : inserted) round_changes_.erase(key);
            return false;
        }
        return true;
    }

    bool loadRoundChangesInternal(std::string& error) {
        round_changes_.clear();
        const auto stored = round_change_store_.loadAll(error);
        if (!error.empty()) return false;
        if (!quorumEnabled()) {
            if (!stored.empty()) { error = "round-change store exists but validator quorum is not configured"; return false; }
            return true;
        }
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const auto target = node.status().frontier_integer + 1;
        bool pruned = false;
        for (const auto& vote : stored) {
            if (vote.integer < target) { pruned = true; continue; }
            if (vote.integer != target || !verifyRoundChangeVote(vote, error)) {
                error = "invalid persisted round-change vote: " + error;
                return false;
            }
            const auto key = std::make_tuple(vote.integer, vote.new_round, vote.validator_address);
            if (!round_changes_.emplace(key, vote).second) {
                error = "duplicate persisted round-change vote";
                return false;
            }
        }
        if (pruned && !persistRoundChanges(error)) return false;
        return true;
    }

    bool loadFinalizationVotesInternal(std::string& error) {
        signed_candidates_.clear();
        const auto stored = finalization_store_.loadAll(error);
        if (!error.empty()) return false;
        if (!quorumEnabled()) {
            if (!stored.empty()) { error = "finalization store exists but validator quorum is not configured"; return false; }
            return true;
        }
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const auto target = node.status().frontier_integer + 1;
        bool pruned = false;
        for (const auto& record : stored) {
            if (record.integer < target) { pruned = true; continue; }
            const auto& vote = record.vote;
            if (record.integer != target || vote.round == 0 ||
                vote.validator_address != validator_identity_->address ||
                vote.validator_address != primechain::crypto::addressFromProtocolPublicKey(vote.public_key)) {
                error = "invalid persisted finalization vote";
                return false;
            }
            std::string verify_error;
            if (!primechain::crypto::verifyProtocolMessageSignature(
                    vote.public_key,
                    primechain::crypto::recordFinalizationVoteSigningPayload(
                        vote.record_hash, vote.round, vote.validator_address),
                    vote.signature, verify_error)) {
                error = "invalid persisted finalization signature";
                return false;
            }
            if (!record.candidate_payload.empty()) {
                const auto kind = parseStoredKindName(record.candidate_kind);
                if (!kind.has_value()) {
                    error = "invalid persisted finalization candidate kind";
                    return false;
                }
                std::string subject_error;
                if (!subjectHashFromCandidatePayload(*kind, record.candidate_payload, subject_error).has_value()) {
                    error = "invalid persisted finalization candidate payload: " + subject_error;
                    return false;
                }
            }
            if (!signed_candidates_.emplace(std::make_pair(record.integer, vote.round), record).second) {
                error = "duplicate persisted finalization vote";
                return false;
            }
        }
        if (pruned && !persistSignedCandidates(error)) return false;
        return true;
    }

    std::vector<primechain::Address> finalizationCommitteeOrdered(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t round) const {
        std::vector<primechain::Address> committee;
        if (validator_set_.empty()) return committee;
        const auto quorum = validatorQuorumRequired();
        if (quorum == 0 || quorum > validator_set_.size()) return committee;

        std::string payload = "primechain-finalization-committee-rotation-v1:";
        payload += primechain::crypto::toHex(previous_hash);
        payload += ":";
        payload += std::to_string(integer);
        const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
        const auto hash = primechain::crypto::sha3_256(bytes);
        std::uint64_t selector = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            selector = (selector << 8) | hash[i];
        }
        const auto base = static_cast<std::size_t>(selector % validator_set_.size());
        const auto start = (base + static_cast<std::size_t>((round - 1) % validator_set_.size())) % validator_set_.size();
        committee.reserve(quorum);
        for (std::size_t i = 0; i < quorum; ++i) {
            committee.push_back(validator_set_[(start + i) % validator_set_.size()]);
        }
        if (!committee.empty()) {
            const auto proposer_offset = static_cast<std::size_t>(((round - 1) / validator_set_.size()) % committee.size());
            std::rotate(committee.begin(), committee.begin() + proposer_offset, committee.end());
        }
        return committee;
    }

    std::vector<primechain::Address> finalizationCommittee(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t round) const {
        auto committee = finalizationCommitteeOrdered(previous_hash, integer, round);
        std::sort(committee.begin(), committee.end());
        return committee;
    }

    primechain::Address finalizationProposer(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t round) const {
        const auto committee = finalizationCommitteeOrdered(previous_hash, integer, round);
        if (committee.empty()) return {};
        return committee.front();
    }

    bool validatorInAssignedFinalizationCommittee(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t round,
        const primechain::Address& validator) const {
        const auto committee = finalizationCommittee(previous_hash, integer, round);
        return std::binary_search(committee.begin(), committee.end(), validator);
    }

    bool finalizationVotesMatchAssignedCommittee(
        const primechain::protocol::FinalizationProofV0& proof,
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::string& error) const {
        if (!quorumEnabled()) return true;
        std::uint64_t round = 0;
        if (!primechain::protocol::verifyRoundChangeCertificate(
                proof, previous_hash, integer, validator_set_, round, error)) {
            return false;
        }
        const auto committee = finalizationCommittee(previous_hash, integer, round);
        if (proof.votes.size() != committee.size()) {
            error = "finalization votes do not match assigned validator committee";
            return false;
        }
        for (std::size_t i = 0; i < committee.size(); ++i) {
            if (proof.votes[i].validator_address != committee[i]) {
                error = "finalization votes do not match assigned validator committee";
                return false;
            }
        }
        return true;
    }

    void clearSignedCandidate(primechain::PrimeValue integer) {
        std::lock_guard<std::mutex> lock(finalization_mutex_);
        bool changed = false;
        for (auto it = signed_candidates_.begin(); it != signed_candidates_.end();) {
            if (std::get<0>(it->first) == integer) { it = signed_candidates_.erase(it); changed = true; }
            else ++it;
        }
        for (auto it = round_changes_.begin(); it != round_changes_.end();) {
            if (std::get<0>(it->first) == integer) { it = round_changes_.erase(it); changed = true; }
            else ++it;
        }
        if (!changed) return;
        std::string error;
        const bool candidates_persisted = persistSignedCandidates(error);
        const bool rounds_persisted = persistRoundChanges(error);
        if (!candidates_persisted || !rounds_persisted) {
            std::cerr << "finalization cleanup warning: " << error << "\n";
        }
    }

    bool makeLocalFinalizationVote(
        primechain::storage::StoredRecordKind kind,
        const std::vector<std::uint8_t>& candidate_payload,
        const primechain::protocol::ValidatorVoteV0* proposer_vote,
        primechain::protocol::ValidatorVoteV0& vote,
        std::string& error) {
        if (!quorumEnabled() || !localValidatorActive()) {
            error = "local validator identity is not active in current validator epoch";
            return false;
        }
        std::lock_guard<std::mutex> lock(finalization_mutex_);
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;

        primechain::PrimeValue integer = 0;
        primechain::Hash256 candidate_hash{};
        primechain::protocol::FinalizationProofV0 proof;
        primechain::Hash256 previous_hash{};
        if (kind == primechain::storage::StoredRecordKind::Composite) {
            auto record = primechain::protocol::deserializeCompositeRecord(candidate_payload, error);
            if (!record.has_value()) return false;
            proof = record->finalized_by;
            if (!record->finalized_by.votes.empty()) { error = "candidate finalization votes must be empty"; return false; }
            if (!node.validateCompositeCandidate(*record, error)) return false;
            integer = record->integer;
            previous_hash = record->previous_record_hash;
            candidate_hash = primechain::protocol::candidateRecordHash(*record);
        } else {
            auto record = primechain::protocol::deserializePrimeRecord(candidate_payload, error);
            if (!record.has_value()) return false;
            if (record->height == 0) { error = "genesis is not signed through candidate voting"; return false; }
            proof = record->finalized_by;
            if (!record->finalized_by.votes.empty()) { error = "candidate finalization votes must be empty"; return false; }
            if (!node.validatePrimeCandidate(*record, error)) return false;
            integer = record->integer;
            previous_hash = record->previous_record_hash;
            candidate_hash = primechain::protocol::candidateRecordHash(*record);
        }
        std::uint64_t round = 0;
        if (!primechain::protocol::verifyRoundChangeCertificate(
                proof, previous_hash, integer, validator_set_, round, error)) return false;
        error.clear();
        if (!importCertifiedRoundChanges(proof, error)) return false;
        error.clear();
        const auto vote_target_hash = finalizationVoteTargetHashFromPayload(kind, candidate_payload, proof, error);
        if (!vote_target_hash.has_value()) return false;
        candidate_hash = *vote_target_hash;
        if (proof.rule == "fixed-2-of-3-mldsa65-rounds-locks-v4") {
            std::uint64_t highest_lock = 0;
            primechain::Hash256 highest_hash{};
            bool have_lock = false;
            for (const auto& change : proof.round_changes) {
                if (change.locked_round == 0) continue;
                if (change.locked_round > highest_lock) {
                    highest_lock = change.locked_round;
                    highest_hash = change.locked_candidate_hash;
                    have_lock = true;
                } else if (change.locked_round == highest_lock &&
                           change.locked_candidate_hash != highest_hash) {
                    error = "conflicting highest round-change locks";
                    return false;
                }
            }
            if (have_lock && highest_hash != candidate_hash) {
                error = "candidate does not match highest round-change lock";
                return false;
            }
        }
        if (round != activeFinalizationRound(integer)) {
            error = "candidate does not target active finalization round";
            return false;
        }

        if (!validatorInAssignedFinalizationCommittee(
                previous_hash, integer, round, validator_identity_->address)) {
            error = "local validator is not assigned to this finalization committee";
            return false;
        }

        if (proposer_vote == nullptr &&
            validator_identity_->address != finalizationProposer(previous_hash, integer, round)) {
            error = "local validator is not assigned proposer for this finalization round";
            return false;
        }

        if (proposer_vote != nullptr) {
            if (proposer_vote->validator_address != finalizationProposer(previous_hash, integer, round)) {
                error = "candidate proposer is not assigned for this finalization round";
                return false;
            }
            if (proposer_vote->record_hash != candidate_hash || proposer_vote->round != round ||
                !std::binary_search(validator_set_.begin(), validator_set_.end(), proposer_vote->validator_address) ||
                proposer_vote->validator_address != primechain::crypto::addressFromProtocolPublicKey(proposer_vote->public_key)) {
                error = "candidate request is not authorized by an active validator";
                return false;
            }
            std::string authorization_error;
            if (!primechain::crypto::verifyProtocolMessageSignature(
                    proposer_vote->public_key,
                    primechain::crypto::recordFinalizationVoteSigningPayload(
                        proposer_vote->record_hash, proposer_vote->round, proposer_vote->validator_address),
                    proposer_vote->signature, authorization_error)) {
                error = "invalid candidate proposer signature";
                return false;
            }
        }

        const auto key = std::make_pair(integer, round);
        const auto existing = signed_candidates_.find(key);
        if (existing != signed_candidates_.end()) {
            if (existing->second.vote.record_hash != candidate_hash) {
                error = "validator already signed a different candidate in this round";
                return false;
            }
            vote = existing->second.vote;
            return true;
        }
        vote = primechain::protocol::makeSignedValidatorVote(
            validator_identity_->address, validator_identity_->public_key,
            validator_identity_->private_key, candidate_hash, round, error);
        if (vote.signature.empty()) return false;
        primechain::storage::SignedCandidateRecord signed_record;
        signed_record.integer = integer;
        signed_record.candidate_kind = storedKindName(kind);
        signed_record.candidate_payload = candidate_payload;
        signed_record.vote = vote;
        signed_candidates_[key] = signed_record;
        if (!persistSignedCandidates(error)) {
            signed_candidates_.erase(key);
            return false;
        }
        return true;
    }

    std::size_t commitPhaseTimeoutVoteCount(
        primechain::PrimeValue integer,
        std::uint64_t current_round,
        std::uint64_t new_round) const {
        std::size_t count = 0;
        for (const auto& entry : commit_phase_timeouts_) {
            if (std::get<0>(entry.first) == integer &&
                std::get<1>(entry.first) == current_round &&
                std::get<2>(entry.first) == new_round) ++count;
        }
        return count;
    }

    bool commitPhaseTimeoutCertified(
        primechain::PrimeValue integer,
        std::uint64_t current_round,
        std::uint64_t new_round) const {
        return quorumEnabled() &&
               commitPhaseTimeoutVoteCount(integer, current_round, new_round) >= validatorQuorumRequired();
    }

    std::uint64_t activeCommitPhaseRound(primechain::PrimeValue integer) const {
        if (!quorumEnabled()) return 1;
        std::uint64_t round = 1;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& entry : commit_phase_timeouts_) {
                if (std::get<0>(entry.first) == integer &&
                    std::get<1>(entry.first) == round &&
                    std::get<2>(entry.first) == round + 1 &&
                    commitPhaseTimeoutCertified(integer, round, round + 1)) {
                    ++round;
                    changed = true;
                    break;
                }
            }
        }
        return round;
    }

    bool commitPhaseStarted(primechain::PrimeValue integer, std::uint64_t commit_round) const {
        if (phaseFrozen(integer, commit_round)) return true;
        for (const auto& entry : commitments_) {
            if (std::get<0>(entry.first) == integer && std::get<1>(entry.first) == commit_round) return true;
        }
        return false;
    }

    bool verifyCommitPhaseTimeoutVote(
        const CommitPhaseTimeoutVote& vote,
        std::string& error) const {
        error.clear();
        if (!quorumEnabled()) { error = "validator quorum is not configured"; return false; }
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const auto target = node.status().frontier_integer + 1;
        const bool already_certified = commitPhaseTimeoutCertified(
            vote.integer, vote.current_round, vote.new_round);
        if (vote.integer != target || vote.previous_record_hash != node.status().latest_record_hash ||
            vote.new_round != vote.current_round + 1 ||
            (!already_certified && vote.current_round != activeCommitPhaseRound(vote.integer)) ||
            (!already_certified && !commitPhaseStarted(vote.integer, vote.current_round)) ||
            !std::binary_search(validator_set_.begin(), validator_set_.end(), vote.validator_address) ||
            vote.validator_address != primechain::crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "invalid commit-phase timeout vote target";
            return false;
        }
        return primechain::crypto::verifyProtocolMessageSignature(
            vote.public_key,
            primechain::crypto::commitPhaseTimeoutSigningPayload(
                vote.previous_record_hash, vote.integer, vote.current_round, vote.new_round,
                vote.validator_address),
            vote.signature, error);
    }

    bool acceptCommitPhaseTimeoutVote(
        const CommitPhaseTimeoutVote& vote,
        std::string& error) {
        if (!verifyCommitPhaseTimeoutVote(vote, error)) return false;
        const auto key = std::make_tuple(
            vote.integer, vote.current_round, vote.new_round, vote.validator_address);
        const auto existing = commit_phase_timeouts_.find(key);
        if (existing != commit_phase_timeouts_.end()) {
            if (existing->second.public_key == vote.public_key) return true;
            error = "validator already submitted a different commit-phase timeout vote";
            return false;
        }
        commit_phase_timeouts_[key] = vote;
        return true;
    }

    CommitPhaseTimeoutVote makeLocalCommitPhaseTimeoutVote(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t current_round,
        std::uint64_t new_round,
        std::string& error) const {
        CommitPhaseTimeoutVote vote;
        if (!localValidatorActive()) { error = "local validator identity is not active in current validator epoch"; return vote; }
        vote.validator_address = validator_identity_->address;
        vote.public_key = validator_identity_->public_key;
        vote.previous_record_hash = previous_hash;
        vote.integer = integer;
        vote.current_round = current_round;
        vote.new_round = new_round;
        const auto signature = primechain::crypto::signProtocolMessage(
            validator_identity_->private_key,
            primechain::crypto::commitPhaseTimeoutSigningPayload(
                previous_hash, integer, current_round, new_round, vote.validator_address), error);
        if (signature.has_value()) vote.signature = *signature;
        return vote;
    }

    bool clearCommitPhaseForTimeout(
        primechain::PrimeValue integer,
        std::uint64_t commit_round,
        std::string& error) {
        bool commitments_changed = false;
        for (auto it = commitments_.begin(); it != commitments_.end();) {
            if (std::get<0>(it->first) == integer && std::get<1>(it->first) == commit_round) {
                it = commitments_.erase(it);
                commitments_changed = true;
            } else ++it;
        }
        bool phases_changed = false;
        for (auto it = phase_votes_.begin(); it != phase_votes_.end();) {
            if (std::get<0>(it->first) == integer && std::get<1>(it->first) == commit_round) {
                it = phase_votes_.erase(it);
                phases_changed = true;
            } else ++it;
        }
        for (auto it = pending_reveals_.begin(); it != pending_reveals_.end();) {
            if (std::get<0>(it->first) == integer && std::get<1>(it->first) == commit_round) {
                it = pending_reveals_.erase(it);
            } else ++it;
        }
        if (commitments_changed && !persistCommitments(error)) return false;
        if (phases_changed && !persistPhaseVotes(error)) return false;
        return true;
    }

    bool applyCommitPhaseTimeoutIfCertified(
        primechain::PrimeValue integer,
        std::uint64_t current_round,
        std::uint64_t new_round,
        std::string& error) {
        if (!commitPhaseTimeoutCertified(integer, current_round, new_round)) return true;
        return clearCommitPhaseForTimeout(integer, current_round, error);
    }

    void timeoutCommitPhase(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, extra;
        primechain::PrimeValue integer = 0;
        in >> command >> integer;
        if (!in || command != "TIMEOUT_COMMIT_PHASE" || (in >> extra)) {
            writeAll(fd, "ERROR invalid TIMEOUT_COMMIT_PHASE\n");
            return;
        }
        if (!validator_identity_.has_value()) {
            writeAll(fd, "ERROR this node has no validator identity\n");
            return;
        }
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (integer != node.status().frontier_integer + 1) {
            writeAll(fd, "ERROR commit-phase timeout must target next integer "
                + std::to_string(node.status().frontier_integer + 1) + "\n");
            return;
        }
        if (!advanceCommitPhaseRound(node.status().latest_record_hash, integer, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        writeAll(fd, "COMMIT_PHASE_TIMED_OUT " + std::to_string(integer) + "\n");
    }

    void signCommitPhaseTimeout(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, previous_hex, proposer_public_hex, proposer_signature_hex, extra;
        CommitPhaseTimeoutVote proposer;
        in >> command >> previous_hex >> proposer.integer >> proposer.current_round
           >> proposer.new_round >> proposer.validator_address >> proposer_public_hex
           >> proposer_signature_hex;
        const auto previous = parseHash(previous_hex);
        if (!in || command != "SIGN_COMMIT_PHASE_TIMEOUT" || !previous.has_value() || (in >> extra)) {
            writeAll(fd, "ERROR invalid SIGN_COMMIT_PHASE_TIMEOUT\n");
            return;
        }
        proposer.previous_record_hash = *previous;
        proposer.public_key = hexToBytes(proposer_public_hex);
        proposer.signature = hexToBytes(proposer_signature_hex);
        std::string error;
        if (!acceptCommitPhaseTimeoutVote(proposer, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        auto local = makeLocalCommitPhaseTimeoutVote(
            proposer.previous_record_hash, proposer.integer, proposer.current_round,
            proposer.new_round, error);
        if (local.signature.empty() || !acceptCommitPhaseTimeoutVote(local, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (!applyCommitPhaseTimeoutIfCertified(
                proposer.integer, proposer.current_round, proposer.new_round, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        writeCommand(fd, "COMMIT_PHASE_TIMEOUT_VOTE "
            + primechain::crypto::toHex(local.previous_record_hash) + " "
            + std::to_string(local.integer) + " " + std::to_string(local.current_round)
            + " " + std::to_string(local.new_round) + " "
            + local.validator_address + " " + bytesToHex(local.public_key) + " "
            + bytesToHex(local.signature) + "\n");
    }

    bool advanceCommitPhaseRound(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::string& error) {
        const std::uint64_t current_round = activeCommitPhaseRound(integer);
        const std::uint64_t next_round = current_round + 1;
        auto local = makeLocalCommitPhaseTimeoutVote(
            previous_hash, integer, current_round, next_round, error);
        if (local.signature.empty() || !acceptCommitPhaseTimeoutVote(local, error)) return false;
        for (const auto& peer : peers_) {
            if (commitPhaseTimeoutCertified(integer, current_round, next_round)) break;
            std::string peer_error;
            const auto vote = requestCommitPhaseTimeoutVote(peer, local, peer_error);
            if (!vote.has_value()) {
                std::cerr << "commit-phase timeout warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
            if (!acceptCommitPhaseTimeoutVote(*vote, peer_error)) {
                std::cerr << "commit-phase timeout rejected from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
        }
        if (!commitPhaseTimeoutCertified(integer, current_round, next_round)) {
            error = "could not collect validator-quorum commit-phase timeout signatures";
            return false;
        }
        return clearCommitPhaseForTimeout(integer, current_round, error);
    }

    primechain::protocol::RoundChangeVoteV1 makeLocalRoundChangeVote(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t new_round,
        std::string& error) {
        primechain::protocol::RoundChangeVoteV1 vote;
        if (!localValidatorActive()) { error = "local validator identity is not active in current validator epoch"; return vote; }
        vote.validator_address = validator_identity_->address;
        vote.public_key = validator_identity_->public_key;
        vote.previous_record_hash = previous_hash;
        vote.integer = integer;
        vote.new_round = new_round;

        std::lock_guard<std::mutex> lock(finalization_mutex_);
        for (const auto& entry : signed_candidates_) {
            const auto& signed_record = entry.second;
            const auto& signed_vote = signed_record.vote;
            if (signed_record.integer != integer || signed_vote.round == 0 ||
                signed_vote.round >= new_round || signed_record.candidate_payload.empty()) continue;
            const auto kind = parseStoredKindName(signed_record.candidate_kind);
            if (!kind.has_value()) continue;
            std::string candidate_error;
            primechain::Hash256 candidate_previous{};
            primechain::PrimeValue candidate_integer = 0;
            if (*kind == primechain::storage::StoredRecordKind::Prime) {
                const auto record = primechain::protocol::deserializePrimeRecord(
                    signed_record.candidate_payload, candidate_error);
                if (!record.has_value()) continue;
                candidate_previous = record->previous_record_hash;
                candidate_integer = record->integer;
            } else {
                const auto record = primechain::protocol::deserializeCompositeRecord(
                    signed_record.candidate_payload, candidate_error);
                if (!record.has_value()) continue;
                candidate_previous = record->previous_record_hash;
                candidate_integer = record->integer;
            }
            if (candidate_previous != previous_hash || candidate_integer != integer) continue;
            candidate_error.clear();
            const auto subject = subjectHashFromCandidatePayload(
                *kind, signed_record.candidate_payload, candidate_error);
            if (!subject.has_value()) continue;
            if (signed_vote.round > vote.locked_round) {
                vote.locked_round = signed_vote.round;
                vote.locked_candidate_kind = signed_record.candidate_kind;
                vote.locked_candidate_hash = *subject;
                vote.locked_candidate_payload = signed_record.candidate_payload;
            }
        }

        const auto signature = primechain::crypto::signProtocolMessage(
            validator_identity_->private_key,
            primechain::crypto::lockedRoundChangeVoteSigningPayload(
                previous_hash, integer, new_round, vote.locked_round,
                vote.locked_candidate_kind, vote.locked_candidate_hash,
                vote.locked_candidate_payload, vote.validator_address), error);
        if (signature.has_value()) vote.signature = *signature;
        return vote;
    }

    void signRoundChange(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, previous_hex, locked_kind, locked_hash_hex, locked_payload_hex,
            proposer_public_hex, proposer_signature_hex, extra;
        primechain::protocol::RoundChangeVoteV1 proposer;
        in >> command >> previous_hex >> proposer.integer >> proposer.new_round
           >> proposer.locked_round >> locked_kind >> locked_hash_hex >> locked_payload_hex
           >> proposer.validator_address >> proposer_public_hex >> proposer_signature_hex;
        const auto previous = parseHash(previous_hex);
        const auto locked_hash = parseHash(locked_hash_hex);
        if (!in || command != "SIGN_ROUND_CHANGE" || !previous.has_value() ||
            !locked_hash.has_value() || (in >> extra)) {
            writeAll(fd, "ERROR invalid SIGN_ROUND_CHANGE\n");
            return;
        }
        proposer.previous_record_hash = *previous;
        proposer.locked_candidate_kind = (locked_kind == "NONE") ? std::string{} : locked_kind;
        proposer.locked_candidate_hash = *locked_hash;
        proposer.locked_candidate_payload = (locked_payload_hex == "-")
            ? std::vector<std::uint8_t>{} : hexToBytes(locked_payload_hex);
        proposer.public_key = hexToBytes(proposer_public_hex);
        proposer.signature = hexToBytes(proposer_signature_hex);
        std::string error;
        if (!acceptRoundChangeVote(proposer, error)) {
            std::string sync_error;
            if (!syncFromPeers(peers_, sync_error) || !acceptRoundChangeVote(proposer, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
        }
        auto local = makeLocalRoundChangeVote(
            proposer.previous_record_hash, proposer.integer, proposer.new_round, error);
        if (local.signature.empty() || !acceptRoundChangeVote(local, error)) {
            std::string sync_error;
            if (!syncFromPeers(peers_, sync_error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            error.clear();
            local = makeLocalRoundChangeVote(
                proposer.previous_record_hash, proposer.integer, proposer.new_round, error);
            if (local.signature.empty() || !acceptRoundChangeVote(local, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
        }
        writeCommand(fd, "ROUND_CHANGE_VOTE "
            + primechain::crypto::toHex(local.previous_record_hash) + " "
            + std::to_string(local.integer) + " " + std::to_string(local.new_round) + " "
            + std::to_string(local.locked_round) + " "
            + (local.locked_candidate_kind.empty() ? std::string("NONE") : local.locked_candidate_kind) + " "
            + primechain::crypto::toHex(local.locked_candidate_hash) + " "
            + (local.locked_candidate_payload.empty() ? std::string("-") : bytesToHex(local.locked_candidate_payload)) + " "
            + local.validator_address + " " + bytesToHex(local.public_key) + " "
            + bytesToHex(local.signature) + "\n");
    }

    void signCompositeLottery(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, payload_hex, extra;
        in >> command >> payload_hex;
        const auto payload = hexToBytes(payload_hex);
        if (!in || command != "SIGN_COMPOSITE_LOTTERY" || payload.empty() || (in >> extra)) {
            writeAll(fd, "ERROR invalid SIGN_COMPOSITE_LOTTERY\n");
            return;
        }
        std::string error;
        auto record = primechain::protocol::deserializeCompositeRecord(payload, error);
        if (!record.has_value()) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        auto validation_record = *record;
        validation_record.composite_lottery = {};
        if (!node.validateCompositeCandidate(validation_record, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (!passCompositeLottery(*record, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        writeCommand(fd, "COMPOSITE_LOTTERY_WIN "
            + std::to_string(record->composite_lottery.round) + " "
            + std::to_string(record->composite_lottery.win_bps) + " "
            + primechain::crypto::toHex(record->composite_lottery.subject_hash) + " "
            + record->composite_lottery.assigned_validator + " "
            + bytesToHex(record->composite_lottery.public_key) + " "
            + bytesToHex(record->composite_lottery.signature) + "\n");
    }

    void signRecordCandidate(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, kind_text, payload_hex, proposer_public_hex;
        std::string proposer_hash_hex, proposer_signature_hex, extra;
        primechain::protocol::ValidatorVoteV0 proposer_vote;
        in >> command >> kind_text >> payload_hex;
        const auto kind = parseKind(kind_text);
        const auto payload = hexToBytes(payload_hex);
        const bool has_proposer = static_cast<bool>(in >> proposer_vote.validator_address);
        if (!has_proposer) in.clear();
        std::optional<primechain::Hash256> proposer_hash;
        if (has_proposer) {
            in >> proposer_public_hex >> proposer_hash_hex >> proposer_vote.round
               >> proposer_signature_hex;
            proposer_hash = parseHash(proposer_hash_hex);
        }
        if (!in || command != "SIGN_RECORD_CANDIDATE" || !kind.has_value() ||
            payload.empty() || (has_proposer && !proposer_hash.has_value()) || (in >> extra)) {
            writeAll(fd, "ERROR invalid SIGN_RECORD_CANDIDATE\n");
            return;
        }
        if (has_proposer) {
            proposer_vote.public_key = hexToBytes(proposer_public_hex);
            proposer_vote.record_hash = *proposer_hash;
            proposer_vote.signature = hexToBytes(proposer_signature_hex);
        }
        primechain::protocol::ValidatorVoteV0 vote;
        std::string error;
        if (!makeLocalFinalizationVote(*kind, payload,
                has_proposer ? &proposer_vote : nullptr, vote, error)) {
            std::string sync_error;
            if (!syncFromPeers(peers_, sync_error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            error.clear();
            if (!makeLocalFinalizationVote(*kind, payload,
                    has_proposer ? &proposer_vote : nullptr, vote, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
        }
        writeCommand(fd, "FINALIZATION_VOTE " + vote.validator_address + " "
            + bytesToHex(vote.public_key) + " "
            + primechain::crypto::toHex(vote.record_hash) + " "
            + std::to_string(vote.round) + " " + bytesToHex(vote.signature) + "\n");
    }

    bool acceptFinalizationVote(
        const primechain::protocol::ValidatorVoteV0& vote,
        const primechain::Hash256& candidate_hash,
        std::uint64_t expected_round,
        std::vector<primechain::protocol::ValidatorVoteV0>& votes,
        std::string& error) const {
        if (vote.record_hash != candidate_hash || vote.round != expected_round ||
            !std::binary_search(validator_set_.begin(), validator_set_.end(), vote.validator_address) ||
            vote.validator_address != primechain::crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "invalid validator finalization vote target";
            return false;
        }
        for (const auto& existing : votes) {
            if (existing.validator_address == vote.validator_address) return true;
        }
        std::string signature_error;
        if (!primechain::crypto::verifyProtocolMessageSignature(
                vote.public_key,
                primechain::crypto::recordFinalizationVoteSigningPayload(
                    vote.record_hash, vote.round, vote.validator_address),
                vote.signature,
                signature_error)) {
            error = "invalid validator finalization signature";
            return false;
        }
        votes.push_back(vote);
        return true;
    }

    template <typename Record>
    bool collectFinalizationVotes(
        Record& record,
        primechain::storage::StoredRecordKind kind,
        std::uint64_t round,
        std::string& error) {
        record.finalized_by.votes.clear();
        std::vector<std::uint8_t> payload;
        if constexpr (std::is_same_v<Record, primechain::protocol::CompositeRecordV0>) {
            payload = primechain::protocol::serializeCompositeRecord(record);
        } else {
            payload = primechain::protocol::serializePrimeRecord(record);
        }
        error.clear();
        const auto target_hash = finalizationVoteTargetHashFromPayload(kind, payload, record.finalized_by, error);
        if (!target_hash.has_value()) return false;
        const auto candidate_hash = *target_hash;
        const auto committee = finalizationCommittee(record.previous_record_hash, record.integer, round);
        const auto proposer = finalizationProposer(record.previous_record_hash, record.integer, round);
        std::optional<primechain::protocol::ValidatorVoteV0> proposer_vote;

        if (localValidatorActive() && validator_identity_->address == proposer) {
            primechain::protocol::ValidatorVoteV0 local_vote;
            std::string local_error;
            if (makeLocalFinalizationVote(kind, payload, nullptr, local_vote, local_error)) {
                proposer_vote = local_vote;
                if (!acceptFinalizationVote(local_vote, candidate_hash, round,
                        record.finalized_by.votes, error)) return false;
            }
        }

        if (!proposer_vote.has_value()) {
            for (const auto& peer : peers_) {
                std::string peer_error;
                const auto vote = requestRecordFinalizationVote(peer, kind, payload, nullptr, peer_error);
                if (!vote.has_value()) {
                    std::cerr << "finalization proposer vote warning from " << peer.host << ":"
                              << peer.port << ": " << peer_error << "\n";
                    continue;
                }
                if (vote->validator_address != proposer) continue;
                if (!acceptFinalizationVote(*vote, candidate_hash, round,
                        record.finalized_by.votes, peer_error)) {
                    std::cerr << "finalization proposer vote rejected from " << peer.host << ":"
                              << peer.port << ": " << peer_error << "\n";
                    continue;
                }
                proposer_vote = *vote;
                break;
            }
        }

        if (!proposer_vote.has_value()) {
            error = "could not collect assigned finalization proposer signature in round "
                + std::to_string(round);
            return false;
        }

        if (localValidatorActive() && validator_identity_->address != proposer &&
            std::binary_search(committee.begin(), committee.end(), validator_identity_->address)) {
            primechain::protocol::ValidatorVoteV0 local_vote;
            std::string local_error;
            if (makeLocalFinalizationVote(kind, payload, &*proposer_vote, local_vote, local_error)) {
                if (!acceptFinalizationVote(local_vote, candidate_hash, round,
                        record.finalized_by.votes, error)) return false;
            }
        }

        for (const auto& peer : peers_) {
            if (record.finalized_by.votes.size() >= committee.size()) break;
            std::string peer_error;
            const auto vote = requestRecordFinalizationVote(peer, kind, payload, &*proposer_vote, peer_error);
            if (!vote.has_value()) {
                std::cerr << "finalization vote warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
            if (!std::binary_search(committee.begin(), committee.end(), vote->validator_address)) {
                continue;
            }
            if (!acceptFinalizationVote(*vote, candidate_hash, round,
                    record.finalized_by.votes, peer_error)) {
                std::cerr << "finalization vote rejected from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
        }
        std::sort(record.finalized_by.votes.begin(), record.finalized_by.votes.end(),
            [](const auto& left, const auto& right) {
                return left.validator_address < right.validator_address;
            });
        if (!primechain::protocol::verifyRecordFinalization(
                record.finalized_by, candidate_hash, record.previous_record_hash,
                record.integer, validator_set_, error)) {
            if (record.finalized_by.votes.size() < committee.size()) {
                error = "could not collect assigned finalization committee signatures in round "
                    + std::to_string(round);
            }
            return false;
        }
        return finalizationVotesMatchAssignedCommittee(
            record.finalized_by, record.previous_record_hash, record.integer, error);
    }

    bool advanceFinalizationRound(
        const primechain::Hash256& previous_hash,
        primechain::PrimeValue integer,
        std::uint64_t new_round,
        std::string& error) {
        auto local = makeLocalRoundChangeVote(previous_hash, integer, new_round, error);
        if (local.signature.empty() || !acceptRoundChangeVote(local, error)) return false;
        for (const auto& peer : peers_) {
            if (certifiedRoundChanges(integer, new_round).size() >= validatorQuorumRequired()) break;
            std::string peer_error;
            const auto vote = requestRoundChangeVote(peer, local, peer_error);
            if (!vote.has_value()) {
                std::cerr << "round-change warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
            if (!acceptRoundChangeVote(*vote, peer_error)) {
                std::cerr << "round-change rejected from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
        }
        if (certifiedRoundChanges(integer, new_round).size() < validatorQuorumRequired()) {
            error = "could not collect validator-quorum round-change signatures";
            return false;
        }
        return true;
    }

    template <typename Record>
    bool finalizeRecordCandidate(
        Record& record,
        primechain::storage::StoredRecordKind kind,
        std::string& error) {
        std::uint64_t round = activeFinalizationRound(record.integer);
        auto apply_highest_lock = [&](std::uint64_t target_round) -> bool {
            if (record.finalized_by.rule != "fixed-2-of-3-mldsa65-rounds-locks-v4") return true;
            std::uint64_t highest_lock = 0;
            primechain::Hash256 highest_hash{};
            const primechain::protocol::RoundChangeVoteV1* locked_vote = nullptr;
            for (const auto& change : record.finalized_by.round_changes) {
                if (change.locked_round == 0) continue;
                if (change.locked_round > highest_lock) {
                    highest_lock = change.locked_round;
                    highest_hash = change.locked_candidate_hash;
                    locked_vote = &change;
                } else if (change.locked_round == highest_lock &&
                           change.locked_candidate_hash != highest_hash) {
                    error = "conflicting highest round-change locks";
                    return false;
                }
            }
            if (locked_vote == nullptr) return true;
            const auto locked_kind = parseStoredKindName(locked_vote->locked_candidate_kind);
            if (!locked_kind.has_value() || *locked_kind != kind) {
                error = "highest round-change lock has wrong record kind";
                return false;
            }
            if constexpr (std::is_same_v<Record, primechain::protocol::PrimeRecordV0>) {
                const auto locked = primechain::protocol::deserializePrimeRecord(
                    locked_vote->locked_candidate_payload, error);
                if (!locked.has_value()) return false;
                record = *locked;
            } else {
                const auto locked = primechain::protocol::deserializeCompositeRecord(
                    locked_vote->locked_candidate_payload, error);
                if (!locked.has_value()) return false;
                record = *locked;
            }
            record.finalized_by.rule = "fixed-2-of-3-mldsa65-rounds-locks-v4";
            record.finalized_by.round_changes = certifiedRoundChanges(record.integer, target_round);
            record.finalized_by.votes.clear();
            return true;
        };

        auto ensure_lottery_after_recovery = [&]() -> bool {
            if constexpr (std::is_same_v<Record, primechain::protocol::CompositeRecordV0>) {
                if (record.version >= primechain::node::kSubjectHashRecordVersion &&
                    record.composite_lottery.round == 0) {
                    return ensureCompositeLottery(record, error);
                }
            }
            return true;
        };

        if (round == 1) {
            record.finalized_by.rule = "fixed-2-of-3-mldsa65-v2";
            record.finalized_by.round_changes.clear();
        } else {
            record.finalized_by.rule = "fixed-2-of-3-mldsa65-rounds-locks-v4";
            record.finalized_by.round_changes = certifiedRoundChanges(record.integer, round);
            record.finalized_by.votes.clear();
            if (!apply_highest_lock(round) || !ensure_lottery_after_recovery()) return false;
        }
        if (collectFinalizationVotes(record, kind, round, error)) return true;
        if (finalization_timeout_ms_ <= 0) return false;

        const auto retry_delay = std::chrono::milliseconds(
            std::max(25, std::min(250, finalization_timeout_ms_ / 10)));
        const auto retry_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(finalization_timeout_ms_);
        while (std::chrono::steady_clock::now() < retry_deadline) {
            std::this_thread::sleep_for(retry_delay);
            std::string retry_error;
            if (collectFinalizationVotes(record, kind, round, retry_error)) return true;
            if (!retry_error.empty()) error = retry_error;
        }


        std::cerr << "finalization round " << round << " stalled for integer "
                  << record.integer << "; requesting round change after "
                  << finalization_timeout_ms_ << " ms retry window\n";

        error.clear();
        const std::uint64_t max_round_attempts = std::max<std::uint64_t>(
            2, static_cast<std::uint64_t>(validator_set_.size()) * 4);
        for (std::uint64_t attempt = 0; attempt < max_round_attempts; ++attempt) {
            const std::uint64_t next_round = round + 1 + attempt;
            if (!advanceFinalizationRound(
                    record.previous_record_hash, record.integer, next_round, error)) return false;
            record.finalized_by.rule = "fixed-2-of-3-mldsa65-rounds-locks-v4";
            record.finalized_by.round_changes = certifiedRoundChanges(record.integer, next_round);
            record.finalized_by.votes.clear();
            if (!apply_highest_lock(next_round) || !ensure_lottery_after_recovery()) return false;
            std::string round_error;
            if (collectFinalizationVotes(record, kind, next_round, round_error)) return true;
            if (!round_error.empty()) error = round_error;
            std::cerr << "finalization round " << next_round << " failed for integer "
                      << record.integer << "; trying next round: " << error << "\n";
        }
        if (error.empty()) error = "could not finalize after round-change attempts";
        return false;
    }

    bool quorumEnabled() const {
        return !validator_set_.empty();
    }

    std::size_t validatorQuorumRequired() const {
        return primechain::core::requiredValidatorQuorum(validator_set_.size());
    }

    std::vector<primechain::storage::CommitPhaseVote> phaseVoteSnapshot() const {
        std::vector<primechain::storage::CommitPhaseVote> out;
        out.reserve(phase_votes_.size());
        for (const auto& entry : phase_votes_) out.push_back(entry.second);
        return out;
    }

    bool persistPhaseVotes(std::string& error) const {
        return phase_store_.replaceAll(phaseVoteSnapshot(), error);
    }

    std::vector<primechain::protocol::CommitCertificateEntryV1> certificateCommitments(
        primechain::PrimeValue integer,
        std::uint64_t commit_round) const {
        std::vector<primechain::protocol::CommitCertificateEntryV1> entries;
        for (const auto& item : commitments_) {
            if (std::get<0>(item.first) != integer || std::get<1>(item.first) != commit_round) continue;
            primechain::protocol::CommitCertificateEntryV1 entry;
            entry.commitment_hash = item.second.commitment_hash;
            entry.provider_address = item.second.provider_address;
            entry.public_key = item.second.public_key;
            entry.signature = item.second.signature;
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            if (left.commitment_hash != right.commitment_hash) {
                return left.commitment_hash < right.commitment_hash;
            }
            return left.provider_address < right.provider_address;
        });
        return entries;
    }

    std::vector<primechain::protocol::CommitCertificateEntryV1> certificateCommitments(
        primechain::PrimeValue integer) const {
        return certificateCommitments(integer, activeCommitPhaseRound(integer));
    }

    primechain::Hash256 commitmentSnapshotHash(
        primechain::PrimeValue integer,
        std::uint64_t commit_round) const {
        return primechain::protocol::commitPhaseSnapshotHash(
            integer, certificateCommitments(integer, commit_round));
    }

    primechain::Hash256 commitmentSnapshotHash(primechain::PrimeValue integer) const {
        return commitmentSnapshotHash(integer, activeCommitPhaseRound(integer));
    }

    primechain::protocol::CommitPhaseCertificateV1 embeddedCommitPhaseCertificate(
        primechain::PrimeValue integer) const {
        primechain::protocol::CommitPhaseCertificateV1 certificate;
        certificate.integer = integer;
        certificate.validator_set = validator_set_;
        std::sort(certificate.validator_set.begin(), certificate.validator_set.end());
        const auto commit_round = activeCommitPhaseRound(integer);
        certificate.commitments = certificateCommitments(integer, commit_round);
        certificate.snapshot_hash = primechain::protocol::commitPhaseSnapshotHash(
            integer, certificate.commitments);
        for (const auto& item : phase_votes_) {
            if (std::get<0>(item.first) != integer || std::get<1>(item.first) != commit_round) continue;
            primechain::protocol::CommitCertificateVoteV1 vote;
            vote.validator_address = item.second.validator_address;
            vote.public_key = item.second.public_key;
            vote.signature = item.second.signature;
            certificate.votes.push_back(std::move(vote));
        }
        std::sort(certificate.votes.begin(), certificate.votes.end(),
            [](const auto& left, const auto& right) {
                return left.validator_address < right.validator_address;
            });
        return certificate;
    }

    std::size_t phaseVoteCount(primechain::PrimeValue integer, std::uint64_t commit_round) const {
        std::size_t count = 0;
        for (const auto& entry : phase_votes_) {
            if (std::get<0>(entry.first) == integer && std::get<1>(entry.first) == commit_round) ++count;
        }
        return count;
    }

    std::size_t phaseVoteCount(primechain::PrimeValue integer) const {
        return phaseVoteCount(integer, activeCommitPhaseRound(integer));
    }

    bool phaseClosed(primechain::PrimeValue integer, std::uint64_t commit_round) const {
        return quorumEnabled() && phaseVoteCount(integer, commit_round) >= validatorQuorumRequired();
    }

    bool phaseClosed(primechain::PrimeValue integer) const {
        return phaseClosed(integer, activeCommitPhaseRound(integer));
    }

    bool phaseFrozen(primechain::PrimeValue integer, std::uint64_t commit_round) const {
        return phaseVoteCount(integer, commit_round) != 0;
    }

    bool phaseFrozen(primechain::PrimeValue integer) const {
        return phaseFrozen(integer, activeCommitPhaseRound(integer));
    }

    bool loadPhaseVotesInternal(std::string& error) {
        phase_votes_.clear();
        const auto stored = phase_store_.loadAll(error);
        if (!error.empty()) return false;
        if (!quorumEnabled()) {
            if (!stored.empty()) {
                error = "phase vote store exists but validator quorum is not configured";
                return false;
            }
            return true;
        }

        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const primechain::PrimeValue target =
            (node.status().has_genesis ? node.status().frontier_integer : 2) + 1;
        bool pruned = false;
        for (const auto& vote : stored) {
            if (vote.integer < target) {
                pruned = true;
                continue;
            }
            const auto expected_snapshot = commitmentSnapshotHash(target, vote.commit_round);
            if (vote.integer != target || vote.snapshot_hash != expected_snapshot ||
                std::find(validator_set_.begin(), validator_set_.end(), vote.validator_address) ==
                    validator_set_.end() ||
                vote.validator_address !=
                    primechain::crypto::addressFromProtocolPublicKey(vote.public_key)) {
                error = "invalid persisted commit-phase vote";
                return false;
            }
            std::string verify_error;
            if (!primechain::crypto::verifyProtocolMessageSignature(
                    vote.public_key,
                    primechain::crypto::commitPhaseVoteSigningPayload(
                        vote.integer, vote.snapshot_hash, vote.validator_address),
                    vote.signature,
                    verify_error)) {
                error = "invalid persisted commit-phase vote signature";
                return false;
            }
            const auto key = std::make_tuple(vote.integer, vote.commit_round, vote.validator_address);
            if (!phase_votes_.emplace(key, vote).second) {
                error = "duplicate persisted validator vote";
                return false;
            }
        }
        if (pruned && !persistPhaseVotes(error)) return false;
        return true;
    }

    bool submitPhaseVoteToPeer(
        const PeerEndpoint& peer,
        const primechain::storage::CommitPhaseVote& vote,
        const std::vector<primechain::protocol::CommitCertificateEntryV1>& commitments,
        std::string& error) const {
        auto socket = connectToServer(peer.host, peer.port);
        if (!socket.has_value()) {
            error = "could not connect to peer";
            return false;
        }
        std::ostringstream header;
        header << "SUBMIT_PHASE_VOTE_BUNDLE_PEER " << vote.integer << " " << vote.commit_round << " "
               << primechain::crypto::toHex(vote.snapshot_hash) << " "
               << vote.validator_address << " " << bytesToHex(vote.public_key) << " "
               << bytesToHex(vote.signature) << " " << commitments.size() << "\n";
        if (!writeCommand(socket->fd(), header.str())) {
            error = "could not submit phase vote";
            return false;
        }
        for (const auto& commitment : commitments) {
            if (!writeCommand(socket->fd(), commitmentWireLine(vote.integer, commitment) + "\n")) {
                error = "could not submit phase vote commitment bundle";
                return false;
            }
        }
        if (!writeCommand(socket->fd(), "END_PHASE_VOTE_BUNDLE\n")) {
            error = "could not submit phase vote commitment bundle terminator";
            return false;
        }
        shutdown(socket->fd(), SHUT_WR);
        const auto response = readLine(socket->fd());
        if (response.has_value() &&
            (response->rfind("PHASE_VOTE_ACCEPTED ", 0) == 0 ||
             response->rfind("PHASE_VOTE_DUPLICATE ", 0) == 0)) return true;
        error = response.has_value() ? *response : "peer did not return phase vote response";
        return false;
    }

    bool requestPeerCommitPhaseClose(
        const PeerEndpoint& peer,
        primechain::PrimeValue integer,
        primechain::storage::CommitPhaseVote& peer_vote,
        bool& has_peer_vote,
        std::string& error) const {
        has_peer_vote = false;
        auto socket = connectToServer(peer.host, peer.port);
        if (!socket.has_value()) {
            error = "could not connect to peer";
            return false;
        }
        std::ostringstream command;
        command << "CLOSE_COMMIT_PHASE " << integer << "\n";
        if (!writeCommand(socket->fd(), command.str())) {
            error = "could not request peer commit-phase close";
            return false;
        }
        shutdown(socket->fd(), SHUT_WR);
        const auto response = readLine(socket->fd());
        if (response.has_value() && response->rfind("PHASE_VOTE_ACCEPTED ", 0) == 0) {
            has_peer_vote = parsePhaseVoteWireSuffix(
                *response, "PHASE_VOTE_ACCEPTED ", integer, peer_vote);
            return true;
        }
        if (response.has_value() && response->rfind("PHASE_VOTE_DUPLICATE ", 0) == 0) {
            has_peer_vote = parsePhaseVoteWireSuffix(
                *response, "PHASE_VOTE_DUPLICATE ", integer, peer_vote);
            return true;
        }
        error = response.has_value() ? *response : "peer did not return commit-phase close response";
        return false;
    }

    void propagatePhaseVote(const primechain::storage::CommitPhaseVote& vote) const {
        const auto bundled_commitments = certificateCommitments(vote.integer, vote.commit_round);
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitPhaseVoteToPeer(peer, vote, bundled_commitments, error)) {
                std::cerr << "phase vote propagation warning to " << peer.host << ":"
                          << peer.port << ": " << error << "\n";
            }
        }
    }

    bool importBundledCommitmentsForPhaseVote(
        const primechain::storage::CommitPhaseVote& vote,
        const std::vector<primechain::storage::StoredCommitment>& bundled_commitments,
        std::string& error) {
        if (bundled_commitments.empty() || phaseFrozen(vote.integer, vote.commit_round)) {
            return true;
        }
        bool changed = false;
        std::vector<std::tuple<primechain::PrimeValue, std::uint64_t, std::string>> inserted_keys;
        for (const auto& commitment : bundled_commitments) {
            if (commitment.integer != vote.integer || commitment.commit_round != vote.commit_round) {
                error = "phase vote commitment bundle target mismatch";
                return false;
            }
            const auto key = std::make_tuple(
                commitment.integer, commitment.commit_round, commitment.provider_address);
            const auto existing = commitments_.find(key);
            if (existing != commitments_.end()) {
                if (existing->second.commitment_hash != commitment.commitment_hash ||
                    existing->second.public_key != commitment.public_key) {
                    error = "phase vote commitment bundle conflicts with local commitment";
                    return false;
                }
                continue;
            }
            if (commitments_.size() >= kMaxCompositeCommitments) {
                error = "commitment pool full during phase vote bundle import";
                return false;
            }
            commitments_[key] = commitment;
            changed = true;
            inserted_keys.push_back(key);
        }
        if (changed && !persistCommitments(error)) {
            for (const auto& key : inserted_keys) commitments_.erase(key);
            return false;
        }
        return true;
    }

    bool acceptPhaseVote(
        const primechain::storage::CommitPhaseVote& vote,
        std::string& error,
        bool propagate) {
        if (!quorumEnabled()) {
            error = "validator quorum is not configured";
            return false;
        }
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        const primechain::PrimeValue target =
            (node.status().has_genesis ? node.status().frontier_integer : 2) + 1;
        if (vote.integer != target) {
            error = "phase vote must target next integer " + std::to_string(target);
            return false;
        }
        if (vote.commit_round != activeCommitPhaseRound(vote.integer)) {
            error = "phase vote targets inactive commit round";
            return false;
        }
        if (certificateCommitments(vote.integer, vote.commit_round).empty()) {
            error = "cannot close an empty commit phase";
            return false;
        }
        if (std::find(validator_set_.begin(), validator_set_.end(), vote.validator_address) ==
            validator_set_.end()) {
            error = "validator is not in configured set";
            return false;
        }
        if (vote.validator_address !=
            primechain::crypto::addressFromProtocolPublicKey(vote.public_key)) {
            error = "validator address does not match public key";
            return false;
        }
        const auto snapshot = commitmentSnapshotHash(vote.integer, vote.commit_round);
        if (vote.snapshot_hash != snapshot) {
            error = "phase vote snapshot does not match local commitments";
            return false;
        }
        for (const auto& existing : phase_votes_) {
            if (std::get<0>(existing.first) == vote.integer &&
                std::get<1>(existing.first) == vote.commit_round &&
                existing.second.snapshot_hash != vote.snapshot_hash) {
                error = "commit phase already frozen on a different snapshot";
                return false;
            }
        }
        std::string verify_error;
        if (!primechain::crypto::verifyProtocolMessageSignature(
                vote.public_key,
                primechain::crypto::commitPhaseVoteSigningPayload(
                    vote.integer, vote.snapshot_hash, vote.validator_address),
                vote.signature,
                verify_error)) {
            error = "invalid commit-phase validator signature";
            return false;
        }
        const auto key = std::make_tuple(vote.integer, vote.commit_round, vote.validator_address);
        const auto existing = phase_votes_.find(key);
        if (existing != phase_votes_.end()) {
            if (existing->second.snapshot_hash == vote.snapshot_hash &&
                existing->second.public_key == vote.public_key &&
                existing->second.signature == vote.signature) return true;
            error = "validator already voted differently";
            return false;
        }
        phase_votes_[key] = vote;
        if (!persistPhaseVotes(error)) {
            phase_votes_.erase(key);
            return false;
        }
        if (propagate) propagatePhaseVote(vote);
        return true;
    }

    bool buildLocalCommitPhaseVote(
        primechain::PrimeValue integer,
        primechain::storage::CommitPhaseVote& vote,
        std::string& error) const {
        if (!localValidatorActive()) {
            error = "local validator identity is not active in current validator epoch";
            return false;
        }
        vote.integer = integer;
        vote.commit_round = activeCommitPhaseRound(integer);
        vote.snapshot_hash = commitmentSnapshotHash(integer, vote.commit_round);
        vote.validator_address = validator_identity_->address;
        vote.public_key = validator_identity_->public_key;
        const auto signature = primechain::crypto::signProtocolMessage(
            validator_identity_->private_key,
            primechain::crypto::commitPhaseVoteSigningPayload(
                integer, vote.snapshot_hash, vote.validator_address),
            error);
        if (!signature.has_value()) return false;
        vote.signature = *signature;
        return true;
    }

    bool acceptLocalCommitPhaseVote(
        primechain::PrimeValue integer,
        bool propagate,
        bool& duplicate,
        primechain::storage::CommitPhaseVote& vote,
        std::string& error) {
        if (!buildLocalCommitPhaseVote(integer, vote, error)) return false;
        duplicate = phase_votes_.find(
            std::make_tuple(integer, vote.commit_round, vote.validator_address)) != phase_votes_.end();
        return acceptPhaseVote(vote, error, propagate);
    }

    bool closeCommitPhaseQuorum(primechain::PrimeValue integer, std::string& error) {
        if (!quorumEnabled()) return true;
        if (phaseClosed(integer)) return true;

        primechain::storage::CommitPhaseVote vote;
        bool duplicate = false;
        if (!acceptLocalCommitPhaseVote(integer, true, duplicate, vote, error)) {
            return false;
        }
        if (phaseClosed(integer)) return true;

        const auto commit_round = activeCommitPhaseRound(integer);
        std::vector<primechain::storage::StoredCommitment> local_commitments;
        for (const auto& entry : commitments_) {
            if (std::get<0>(entry.first) == integer &&
                std::get<1>(entry.first) == commit_round) {
                local_commitments.push_back(entry.second);
            }
        }

        for (const auto& peer : peers_) {
            for (const auto& commitment : local_commitments) {
                std::string commit_error;
                if (!submitCommitToPeer(peer, commitment, commit_error)) {
                    std::cerr << "commitment close warmup warning to " << peer.host << ":"
                              << peer.port << ": " << commit_error << "\n";
                }
            }
            std::string peer_error;
            primechain::storage::CommitPhaseVote peer_vote;
            bool has_peer_vote = false;
            if (!requestPeerCommitPhaseClose(peer, integer, peer_vote, has_peer_vote, peer_error)) {
                std::cerr << "commit phase close warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
            } else if (has_peer_vote && !acceptPhaseVote(peer_vote, peer_error, false)) {
                std::cerr << "commit phase close vote warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
            }
            peer_error.clear();
            if (!syncPhaseVotesFromPeer(peer.host, peer.port, peer_error)) {
                std::cerr << "phase vote sync warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
            }
        }

        if (phaseClosed(integer)) return true;
        error = "could not close commit phase with validator quorum";
        return false;
    }

    void closeCommitPhase(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, extra;
        primechain::PrimeValue integer = 0;
        in >> command >> integer;
        if (!in || command != "CLOSE_COMMIT_PHASE" || (in >> extra)) {
            writeAll(fd, "ERROR invalid CLOSE_COMMIT_PHASE; expected CLOSE_COMMIT_PHASE g\n");
            return;
        }
        primechain::storage::CommitPhaseVote vote;
        std::string error;
        bool duplicate = false;
        if (!acceptLocalCommitPhaseVote(integer, true, duplicate, vote, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        writeCommand(fd, std::string(duplicate ? "PHASE_VOTE_DUPLICATE " : "PHASE_VOTE_ACCEPTED ")
            + phaseVoteWireSuffix(vote)
            + " votes=" + std::to_string(phaseVoteCount(integer)) + "\n");
    }

    void submitPhaseVote(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command, maybe_round_or_snapshot, snapshot_hex, address, public_key_hex, signature_hex, extra;
        primechain::PrimeValue integer = 0;
        std::uint64_t bundled_commitment_count = 0;
        in >> command >> integer >> maybe_round_or_snapshot;
        auto snapshot = parseHash(maybe_round_or_snapshot);
        std::uint64_t commit_round = activeCommitPhaseRound(integer);
        if (snapshot.has_value()) {
            snapshot_hex = maybe_round_or_snapshot;
            in >> address >> public_key_hex >> signature_hex;
        } else {
            commit_round = std::stoull(maybe_round_or_snapshot);
            in >> snapshot_hex >> address >> public_key_hex >> signature_hex;
            snapshot = parseHash(snapshot_hex);
        }
        const bool bundled_command = command == "SUBMIT_PHASE_VOTE_BUNDLE" ||
            command == "SUBMIT_PHASE_VOTE_BUNDLE_PEER";
        if (bundled_command) {
            in >> bundled_commitment_count;
        }
        if (!in || (command != "SUBMIT_PHASE_VOTE" && command != "SUBMIT_PHASE_VOTE_PEER" &&
                !bundled_command) ||
            !snapshot.has_value() || bundled_commitment_count > kMaxCompositeCommitments ||
            (in >> extra)) {
            writeAll(fd, "ERROR invalid SUBMIT_PHASE_VOTE\n");
            return;
        }
        primechain::storage::CommitPhaseVote vote;
        vote.integer = integer;
        vote.commit_round = commit_round;
        vote.snapshot_hash = *snapshot;
        vote.validator_address = address;
        vote.public_key = hexToBytes(public_key_hex);
        vote.signature = hexToBytes(signature_hex);
        std::vector<primechain::storage::StoredCommitment> bundled_commitments;
        std::string error;
        if (bundled_command) {
            bundled_commitments.reserve(static_cast<std::size_t>(bundled_commitment_count));
            for (std::uint64_t i = 0; i < bundled_commitment_count; ++i) {
                const auto commitment_line = readLine(fd);
                if (!commitment_line.has_value()) {
                    writeAll(fd, "ERROR truncated phase vote commitment bundle\n");
                    return;
                }
                primechain::storage::StoredCommitment commitment;
                if (!parseCommitmentWireLine(*commitment_line, vote.integer, vote.commit_round,
                        commitment, error)) {
                    writeAll(fd, "ERROR invalid phase vote commitment bundle: " + error + "\n");
                    return;
                }
                bundled_commitments.push_back(std::move(commitment));
            }
            const auto end_line = readLine(fd);
            if (!end_line.has_value() || *end_line != "END_PHASE_VOTE_BUNDLE") {
                writeAll(fd, "ERROR invalid phase vote commitment bundle terminator\n");
                return;
            }
            if (!importBundledCommitmentsForPhaseVote(vote, bundled_commitments, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
        }
        const bool duplicate = phase_votes_.find(std::make_tuple(integer, vote.commit_round, address)) != phase_votes_.end();
        if (!acceptPhaseVote(vote, error, propagate)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        writeAll(fd, std::string(duplicate ? "PHASE_VOTE_DUPLICATE " : "PHASE_VOTE_ACCEPTED ")
            + std::to_string(integer) + " " + snapshot_hex
            + " votes=" + std::to_string(phaseVoteCount(integer)) + "\n");
    }

    void sendCommitPhase(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command, extra;
        primechain::PrimeValue integer = 0;
        in >> command >> integer;
        if (!in || command != "GET_COMMIT_PHASE" || (in >> extra)) {
            writeAll(fd, "ERROR invalid GET_COMMIT_PHASE\n");
            return;
        }
        const auto winner = selectedCommitment(integer);
        const std::string state = phaseClosed(integer) ? "CLOSED" :
            (phaseFrozen(integer) ? "CLOSING" : "OPEN");
        writeAll(fd, "COMMIT_PHASE " + std::to_string(integer) + " " + state + " "
            + std::to_string(phaseVoteCount(integer)) + " "
            + primechain::crypto::toHex(commitmentSnapshotHash(integer)) + " "
            + (winner.has_value() ? winner->provider_address : std::string("-")) + "\n");
    }

    void sendMiningView(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command, extra;
        primechain::PrimeValue requested = 0;
        in >> command;
        if (!in || command != "GET_MINING_VIEW") {
            writeAll(fd, "ERROR invalid GET_MINING_VIEW\n");
            return;
        }
        if (in >> requested) {
            if (in >> extra) {
                writeAll(fd, "ERROR invalid GET_MINING_VIEW\n");
                return;
            }
        } else {
            in.clear();
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const auto status = node.status();
        const primechain::PrimeValue frontier = status.has_genesis ? status.frontier_integer : 2;
        const primechain::PrimeValue integer = requested == 0 ? frontier + 1 : requested;
        const auto winner = selectedCommitment(integer);
        const std::string phase_state = phaseClosed(integer) ? "CLOSED" :
            (phaseFrozen(integer) ? "CLOSING" : "OPEN");
        const auto commit_round = activeCommitPhaseRound(integer);
        std::size_t commitment_count = 0;
        for (const auto& item : commitments_) {
            if (std::get<0>(item.first) == integer && std::get<1>(item.first) == commit_round) ++commitment_count;
        }
        writeAll(fd, "MINING_VIEW "
            + std::to_string(frontier) + " "
            + std::to_string(integer) + " "
            + std::to_string(status.has_genesis ? 1 : 0) + " "
            + primechain::crypto::toHex(status.latest_record_hash) + " "
            + phase_state + " "
            + std::to_string(phaseVoteCount(integer)) + " "
            + primechain::crypto::toHex(commitmentSnapshotHash(integer)) + " "
            + (winner.has_value() ? winner->provider_address : std::string("-")) + " "
            + std::to_string(commitment_count) + " "
            + std::to_string(commit_round) + " "
            + std::to_string(validator_set_.size()) + " "
            + std::to_string(peers_.size()) + "\n");
    }

    void sendPhaseVotes(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command, extra;
        primechain::PrimeValue integer = 0;
        in >> command >> integer;
        if (!in || command != "GET_PHASE_VOTES" || (in >> extra)) {
            writeAll(fd, "ERROR invalid GET_PHASE_VOTES\n");
            return;
        }
        const auto commit_round = activeCommitPhaseRound(integer);
        writeAll(fd, "PHASE_VOTES " + std::to_string(integer) + " "
            + std::to_string(phaseVoteCount(integer, commit_round)) + "\n");
        for (const auto& entry : phase_votes_) {
            if (std::get<0>(entry.first) != integer || std::get<1>(entry.first) != commit_round) continue;
            const auto& vote = entry.second;
            writeCommand(fd, "PHASE_VOTE " + std::to_string(integer) + " "
                + std::to_string(vote.commit_round) + " "
                + primechain::crypto::toHex(vote.snapshot_hash) + " "
                + vote.validator_address + " " + bytesToHex(vote.public_key) + " "
                + bytesToHex(vote.signature) + "\n");
        }
        writeAll(fd, "END_PHASE_VOTES\n");
    }

    void propagateCommit(const primechain::storage::StoredCommitment& commitment) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitCommitToPeer(peer, commitment, error)) {
                std::cerr << "commitment propagation warning to " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
            }
        }
    }

    std::vector<primechain::storage::ValidatorEpochVoteRecord> epochVoteSnapshot() const {
        std::vector<primechain::storage::ValidatorEpochVoteRecord> out;
        for (const auto& entry : epoch_votes_) out.push_back(entry.second);
        return out;
    }

    bool persistEpochVotes(std::string& error) const {
        return epoch_store_.replaceAll(epochVoteSnapshot(), error);
    }

    bool epochProposalReady() const {
        return quorumEnabled() && epoch_votes_.size() >= validatorQuorumRequired();
    }

    bool epochProposalTargetsNextRecord(const primechain::node::SequentialNode& node) const {
        if (epoch_votes_.empty()) return false;
        const auto& proposal = epoch_votes_.begin()->second;
        return proposal.previous_record_hash == node.status().latest_record_hash &&
               proposal.record_integer == node.status().frontier_integer + 1 &&
               proposal.epoch == node.validatorEpoch() + 1 &&
               proposal.activation_integer == proposal.record_integer + 1;
    }

    primechain::protocol::ValidatorEpochTransitionV1 embeddedValidatorEpoch() const {
        primechain::protocol::ValidatorEpochTransitionV1 transition;
        if (!epochProposalReady()) return transition;
        const auto& proposal = epoch_votes_.begin()->second;
        transition.epoch = proposal.epoch;
        transition.activation_integer = proposal.activation_integer;
        transition.next_validator_set = proposal.next_validator_set;
        for (const auto& entry : epoch_votes_) transition.votes.push_back(entry.second.vote);
        std::sort(transition.votes.begin(), transition.votes.end(), [](const auto& left, const auto& right) {
            return left.validator_address < right.validator_address;
        });
        return transition;
    }

    primechain::protocol::ValidatorEpochTransitionV1 embeddedValidatorEpochForNextRecord(
            const primechain::node::SequentialNode& node) {
        if (epoch_votes_.empty()) return {};
        if (!epochProposalTargetsNextRecord(node)) {
            clearEpochVotesAfterRecord();
            return {};
        }
        if (!epochProposalReady()) return {};
        return embeddedValidatorEpoch();
    }

    std::vector<primechain::protocol::ValidatorEndpointUpdateV1> embeddedValidatorEndpointsForNextRecord(
            const primechain::node::SequentialNode& node) {
        std::vector<primechain::protocol::ValidatorEndpointUpdateV1> out;
        for (auto it = pending_endpoint_updates_.begin(); it != pending_endpoint_updates_.end();) {
            const auto& update = it->second;
            if (update.effective_integer < node.status().frontier_integer + 1) {
                it = pending_endpoint_updates_.erase(it);
                continue;
            }
            out.push_back(update);
            ++it;
        }
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
            return left.validator_address < right.validator_address;
        });
        return out;
    }

    bool loadEpochVotesInternal(std::string& error) {
        epoch_votes_.clear();
        const auto stored = epoch_store_.loadAll(error);
        if (!error.empty()) return false;
        if (stored.empty()) return true;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) return false;
        bool pruned = false;
        for (const auto& record : stored) {
            if (record.record_integer < node.status().frontier_integer + 1) {
                pruned = true;
                continue;
            }
            if (record.previous_record_hash != node.status().latest_record_hash ||
                record.record_integer != node.status().frontier_integer + 1 ||
                record.epoch != node.validatorEpoch() + 1 ||
                record.activation_integer != record.record_integer + 1 ||
                !primechain::core::validValidatorSetSize(record.next_validator_set.size()) ||
                !std::is_sorted(record.next_validator_set.begin(), record.next_validator_set.end()) ||
                std::adjacent_find(record.next_validator_set.begin(), record.next_validator_set.end()) != record.next_validator_set.end() ||
                std::find(validator_set_.begin(), validator_set_.end(), record.vote.validator_address) == validator_set_.end() ||
                record.vote.validator_address != primechain::crypto::addressFromProtocolPublicKey(record.vote.public_key)) {
                error = "invalid persisted validator epoch vote";
                return false;
            }
            std::string verify_error;
            if (!primechain::crypto::verifyProtocolMessageSignature(
                    record.vote.public_key,
                    primechain::crypto::validatorEpochVoteSigningPayload(
                        record.previous_record_hash, record.record_integer, record.epoch,
                        record.activation_integer, record.next_validator_set,
                        record.vote.validator_address),
                    record.vote.signature, verify_error)) {
                error = "invalid persisted validator epoch signature";
                return false;
            }
            if (!epoch_votes_.empty()) {
                const auto& first = epoch_votes_.begin()->second;
                if (record.previous_record_hash != first.previous_record_hash ||
                    record.record_integer != first.record_integer || record.epoch != first.epoch ||
                    record.activation_integer != first.activation_integer ||
                    record.next_validator_set != first.next_validator_set) {
                    error = "conflicting persisted validator epoch proposals";
                    return false;
                }
            }
            if (!epoch_votes_.emplace(record.vote.validator_address, record).second) {
                error = "duplicate persisted validator epoch vote";
                return false;
            }
        }
        if (pruned && !persistEpochVotes(error)) return false;
        return true;
    }

    void sendEpochVotes(int fd) const {
        if (epoch_votes_.empty()) {
            writeAll(fd, "EPOCH_VOTES 0\nEND_EPOCH_VOTES\n");
            return;
        }
        const auto& proposal = epoch_votes_.begin()->second;
        std::ostringstream header;
        header << "EPOCH_VOTES " << epoch_votes_.size() << " "
            << primechain::crypto::toHex(proposal.previous_record_hash) << " "
            << proposal.record_integer << " " << proposal.epoch << " "
            << proposal.activation_integer;
        for (const auto& validator : proposal.next_validator_set) header << " " << validator;
        header << "\n";
        writeCommand(fd, header.str());
        for (const auto& entry : epoch_votes_) {
            const auto& vote = entry.second.vote;
            writeCommand(fd, "EPOCH_VOTE " + vote.validator_address + " "
                + bytesToHex(vote.public_key) + " " + bytesToHex(vote.signature) + "\n");
        }
        writeAll(fd, "END_EPOCH_VOTES\n");
    }

    bool submitEpochVoteToPeer(
        const PeerEndpoint& peer,
        const primechain::storage::ValidatorEpochVoteRecord& record,
        std::string& error) const {
        auto socket = connectToServer(peer.host, peer.port);
        if (!socket.has_value()) { error = "could not connect to peer"; return false; }
        std::ostringstream command;
        command << "SUBMIT_EPOCH_VOTE_PEER " << primechain::crypto::toHex(record.previous_record_hash)
                << " " << record.record_integer << " " << record.epoch << " "
                << record.activation_integer;
        for (const auto& validator : record.next_validator_set) command << " " << validator;
        command << " " << record.vote.validator_address << " "
                << bytesToHex(record.vote.public_key) << " " << bytesToHex(record.vote.signature) << "\n";
        if (!writeCommand(socket->fd(), command.str())) { error = "could not submit epoch vote"; return false; }
        shutdown(socket->fd(), SHUT_WR);
        const auto response = readLine(socket->fd());
        if (response.has_value() &&
            (response->rfind("EPOCH_VOTE_ACCEPTED ", 0) == 0 ||
             response->rfind("EPOCH_VOTE_DUPLICATE ", 0) == 0)) return true;
        error = response.has_value() ? *response : "peer did not return epoch vote response";
        return false;
    }

    void propagateEpochVote(const primechain::storage::ValidatorEpochVoteRecord& record) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitEpochVoteToPeer(peer, record, error)) {
                std::cerr << "epoch vote propagation warning to " << peer.host << ":"
                          << peer.port << ": " << error << "\n";
            }
        }
    }

    void submitEpochVote(int fd, const std::string& line, bool propagate = true) {
        std::istringstream in(line);
        std::string command, previous_hex;
        std::vector<std::string> tail;
        primechain::storage::ValidatorEpochVoteRecord record;
        in >> command >> previous_hex >> record.record_integer >> record.epoch >> record.activation_integer;
        for (std::string token; in >> token;) tail.push_back(token);
        const auto previous = parseHash(previous_hex);
        if (tail.size() < 4) {
            writeAll(fd, "ERROR invalid SUBMIT_EPOCH_VOTE\n");
            return;
        }
        const std::string voter = tail[tail.size() - 3];
        const std::string public_hex = tail[tail.size() - 2];
        const std::string signature_hex = tail[tail.size() - 1];
        record.next_validator_set.assign(tail.begin(), tail.end() - 3);
        record.vote.validator_address = voter;
        record.vote.public_key = hexToBytes(public_hex);
        record.vote.signature = hexToBytes(signature_hex);
        if (!in.eof() || command != "SUBMIT_EPOCH_VOTE" && command != "SUBMIT_EPOCH_VOTE_PEER" || !previous.has_value()) {
            writeAll(fd, "ERROR invalid SUBMIT_EPOCH_VOTE\n");
            return;
        }
        record.previous_record_hash = *previous;
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) { writeAll(fd, "ERROR " + error + "\n"); return; }
        if (!quorumEnabled() || record.previous_record_hash != node.status().latest_record_hash ||
            record.record_integer != node.status().frontier_integer + 1 ||
            record.epoch != node.validatorEpoch() + 1 || record.activation_integer != record.record_integer + 1 ||
            !primechain::core::validValidatorSetSize(record.next_validator_set.size()) ||
            !std::is_sorted(record.next_validator_set.begin(), record.next_validator_set.end()) ||
            std::adjacent_find(record.next_validator_set.begin(), record.next_validator_set.end()) != record.next_validator_set.end() ||
            !std::all_of(record.next_validator_set.begin(), record.next_validator_set.end(), primechain::crypto::isProtocolSignatureAddress)) {
            writeAll(fd, "ERROR epoch proposal does not match current chain state\n");
            return;
        }
        if (std::find(validator_set_.begin(), validator_set_.end(), voter) == validator_set_.end() ||
            voter != primechain::crypto::addressFromProtocolPublicKey(record.vote.public_key) ||
            !primechain::crypto::verifyProtocolMessageSignature(
                record.vote.public_key,
                primechain::crypto::validatorEpochVoteSigningPayload(
                    record.previous_record_hash, record.record_integer, record.epoch,
                    record.activation_integer, record.next_validator_set, voter),
                record.vote.signature, error)) {
            writeAll(fd, "ERROR invalid validator epoch vote\n");
            return;
        }
        if (!epoch_votes_.empty()) {
            const auto& first = epoch_votes_.begin()->second;
            if (record.previous_record_hash != first.previous_record_hash ||
                record.record_integer != first.record_integer || record.epoch != first.epoch ||
                record.activation_integer != first.activation_integer ||
                record.next_validator_set != first.next_validator_set) {
                writeAll(fd, "ERROR conflicting validator epoch proposal\n");
                return;
            }
        }
        std::vector<primechain::Address> added_validators;
        std::set_difference(
            record.next_validator_set.begin(), record.next_validator_set.end(),
            node.validatorSet().begin(), node.validatorSet().end(),
            std::back_inserter(added_validators));
        for (const auto& candidate : added_validators) {
            if (!validatorAdmissionEligible(node, candidate, error)) {
                writeAll(fd, "ERROR candidate validator admission failed: " + error + "\n");
                return;
            }
        }
        const auto found = epoch_votes_.find(voter);
        if (found != epoch_votes_.end()) {
            if (found->second.vote.signature == record.vote.signature) {
                writeAll(fd, "EPOCH_VOTE_DUPLICATE " + std::to_string(record.epoch) + " votes=" + std::to_string(epoch_votes_.size()) + "\n");
            } else {
                writeAll(fd, "ERROR validator already voted for this epoch\n");
            }
            return;
        }
        epoch_votes_[voter] = record;
        if (!persistEpochVotes(error)) {
            epoch_votes_.erase(voter);
            writeAll(fd, "ERROR could not persist epoch vote: " + error + "\n");
            return;
        }
        if (propagate) propagateEpochVote(record);
        writeAll(fd, "EPOCH_VOTE_ACCEPTED " + std::to_string(record.epoch) + " votes=" + std::to_string(epoch_votes_.size()) + "\n");
    }

    bool policyProposalReady() const {
        return quorumEnabled() && policy_votes_.size() >= validatorQuorumRequired();
    }

    bool policyProposalTargetsNextRecord(const primechain::node::SequentialNode& node) const {
        if (policy_votes_.empty()) return false;
        const auto& proposal = policy_votes_.begin()->second;
        return proposal.previous_record_hash == node.status().latest_record_hash &&
               proposal.record_integer == node.status().frontier_integer + 1 &&
               proposal.effective_integer == proposal.record_integer + 1;
    }

    primechain::protocol::EconomicPolicyUpdateV1 embeddedEconomicPolicy() const {
        primechain::protocol::EconomicPolicyUpdateV1 update;
        if (!policyProposalReady()) return update;
        const auto& proposal = policy_votes_.begin()->second;
        update.transfer_fee_micro_units = proposal.transfer_fee_micro_units;
        update.validator_min_reserve_micro_units = proposal.validator_min_reserve_micro_units;
        update.effective_integer = proposal.effective_integer;
        update.sequence = proposal.sequence;
        for (const auto& entry : policy_votes_) update.votes.push_back(entry.second.vote);
        std::sort(update.votes.begin(), update.votes.end(), [](const auto& left, const auto& right) {
            return left.validator_address < right.validator_address;
        });
        return update;
    }

    primechain::protocol::EconomicPolicyUpdateV1 embeddedEconomicPolicyForNextRecord(
            const primechain::node::SequentialNode& node) {
        if (policy_votes_.empty()) return {};
        if (!policyProposalTargetsNextRecord(node)) {
            clearPolicyVotesAfterRecord();
            return {};
        }
        if (!policyProposalReady()) return {};
        return embeddedEconomicPolicy();
    }

    void sendPolicyVotes(int fd) const {
        if (policy_votes_.empty()) {
            writeAll(fd, "POLICY_VOTES 0\nEND_POLICY_VOTES\n");
            return;
        }
        const auto& proposal = policy_votes_.begin()->second;
        std::ostringstream header;
        header << "POLICY_VOTES " << policy_votes_.size() << " "
            << primechain::crypto::toHex(proposal.previous_record_hash) << " "
            << proposal.record_integer << " " << proposal.transfer_fee_micro_units << " "
            << proposal.validator_min_reserve_micro_units << " "
            << proposal.effective_integer << " " << proposal.sequence << "\n";
        writeCommand(fd, header.str());
        for (const auto& entry : policy_votes_) {
            const auto& vote = entry.second.vote;
            writeCommand(fd, "POLICY_VOTE " + vote.validator_address + " "
                + bytesToHex(vote.public_key) + " " + bytesToHex(vote.signature) + "\n");
        }
        writeAll(fd, "END_POLICY_VOTES\n");
    }

    bool submitPolicyVoteToPeer(
        const PeerEndpoint& peer,
        const EconomicPolicyVoteRecord& record,
        std::string& error) const {
        auto socket = connectToServer(peer.host, peer.port);
        if (!socket.has_value()) { error = "could not connect to peer"; return false; }
        std::ostringstream command;
        command << "SUBMIT_POLICY_VOTE_PEER " << primechain::crypto::toHex(record.previous_record_hash)
                << " " << record.record_integer << " " << record.transfer_fee_micro_units
                << " " << record.validator_min_reserve_micro_units
                << " " << record.effective_integer << " " << record.sequence
                << " " << record.vote.validator_address << " "
                << bytesToHex(record.vote.public_key) << " " << bytesToHex(record.vote.signature) << "\n";
        if (!writeCommand(socket->fd(), command.str())) { error = "could not submit policy vote"; return false; }
        shutdown(socket->fd(), SHUT_WR);
        const auto response = readLine(socket->fd());
        if (response.has_value() &&
            (response->rfind("POLICY_VOTE_ACCEPTED ", 0) == 0 ||
             response->rfind("POLICY_VOTE_DUPLICATE ", 0) == 0)) return true;
        error = response.has_value() ? *response : "peer did not return policy vote response";
        return false;
    }

    void propagatePolicyVote(const EconomicPolicyVoteRecord& record) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitPolicyVoteToPeer(peer, record, error)) {
                std::cerr << "policy vote propagation warning to " << peer.host << ":"
                          << peer.port << ": " << error << "\n";
            }
        }
    }

    void submitPolicyVote(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command, previous_hex, voter, public_hex, signature_hex, extra;
        EconomicPolicyVoteRecord record;
        in >> command >> previous_hex >> record.record_integer >> record.transfer_fee_micro_units
           >> record.validator_min_reserve_micro_units
           >> record.effective_integer >> record.sequence >> voter >> public_hex >> signature_hex;
        const auto previous = parseHash(previous_hex);
        if (!in || (in >> extra) ||
            (command != "SUBMIT_POLICY_VOTE" && command != "SUBMIT_POLICY_VOTE_PEER") ||
            !previous.has_value()) {
            writeAll(fd, "ERROR invalid SUBMIT_POLICY_VOTE\n");
            return;
        }
        record.previous_record_hash = *previous;
        record.vote.validator_address = voter;
        record.vote.public_key = hexToBytes(public_hex);
        record.vote.signature = hexToBytes(signature_hex);

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) { writeAll(fd, "ERROR " + error + "\n"); return; }
        if (!quorumEnabled() || record.previous_record_hash != node.status().latest_record_hash ||
            record.record_integer != node.status().frontier_integer + 1 ||
            record.effective_integer != record.record_integer + 1 ||
            record.transfer_fee_micro_units == 0 ||
            record.validator_min_reserve_micro_units == 0) {
            writeAll(fd, "ERROR policy proposal does not match current chain state\n");
            return;
        }
        if (std::find(node.validatorSet().begin(), node.validatorSet().end(), voter) ==
                node.validatorSet().end() ||
            voter != primechain::crypto::addressFromProtocolPublicKey(record.vote.public_key) ||
            !primechain::crypto::verifyProtocolMessageSignature(
                record.vote.public_key,
                primechain::crypto::economicPolicySigningPayload(
                    record.previous_record_hash, record.record_integer,
                    record.transfer_fee_micro_units, record.validator_min_reserve_micro_units,
                    record.effective_integer, record.sequence, voter),
                record.vote.signature, error)) {
            writeAll(fd, "ERROR invalid economic policy vote\n");
            return;
        }
        if (!policy_votes_.empty()) {
            const auto& first = policy_votes_.begin()->second;
            if (record.previous_record_hash != first.previous_record_hash ||
                record.record_integer != first.record_integer ||
                record.transfer_fee_micro_units != first.transfer_fee_micro_units ||
                record.validator_min_reserve_micro_units != first.validator_min_reserve_micro_units ||
                record.effective_integer != first.effective_integer ||
                record.sequence != first.sequence) {
                writeAll(fd, "ERROR conflicting economic policy proposal\n");
                return;
            }
        }
        const auto found = policy_votes_.find(voter);
        if (found != policy_votes_.end()) {
            if (found->second.vote.signature == record.vote.signature) {
                writeAll(fd, "POLICY_VOTE_DUPLICATE " + std::to_string(record.sequence) +
                    " votes=" + std::to_string(policy_votes_.size()) + "\n");
            } else {
                writeAll(fd, "ERROR validator already voted for this policy sequence\n");
            }
            return;
        }
        policy_votes_[voter] = record;
        if (propagate) propagatePolicyVote(record);
        writeAll(fd, "POLICY_VOTE_ACCEPTED " + std::to_string(record.sequence) +
            " votes=" + std::to_string(policy_votes_.size()) + "\n");
    }

    std::map<primechain::Address, primechain::protocol::ValidatorApplicationV1> validatorApplicationsFromChain() const {
        std::map<primechain::Address, primechain::protocol::ValidatorApplicationV1> out;
        primechain::storage::RecordStore store(store_path_);
        std::string error;
        const auto records = store.loadAll(error);
        if (!error.empty()) return out;
        for (const auto& stored : records) {
            if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
                const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
                if (!record.has_value()) return out;
                for (const auto& application : record->validator_applications) out[application.candidate_address] = application;
                continue;
            }
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) return out;
            for (const auto& application : record->validator_applications) out[application.candidate_address] = application;
        }
        return out;
    }

    ValidatorCandidateStats validatorCandidateStatsFromChain(const primechain::Address& candidate) const {
        ValidatorCandidateStats stats;
        primechain::storage::RecordStore store(store_path_);
        std::string error;
        const auto records = store.loadAll(error);
        if (!error.empty()) return stats;
        for (const auto& stored : records) {
            if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
                const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
                if (!record.has_value()) return stats;
                if (record->proof.provider_address == candidate) ++stats.prime_records;
                continue;
            }
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) return stats;
            if (record->proof.provider_address == candidate) ++stats.composite_records;
        }
        return stats;
    }

    std::optional<primechain::protocol::ValidatorApplicationV1> validatorApplicationFor(
            const primechain::Address& candidate) const {
        const auto pending = pending_validator_applications_.find(candidate);
        if (pending != pending_validator_applications_.end()) return pending->second;
        const auto chain = validatorApplicationsFromChain();
        const auto found = chain.find(candidate);
        if (found != chain.end()) return found->second;
        return std::nullopt;
    }


    std::map<std::pair<primechain::Address, primechain::Address>, primechain::protocol::ValidatorWorkBindingV1>
    validatorWorkBindingsFromChain() const {
        std::map<std::pair<primechain::Address, primechain::Address>, primechain::protocol::ValidatorWorkBindingV1> out;
        primechain::storage::RecordStore store(store_path_);
        std::string error;
        const auto records = store.loadAll(error);
        if (!error.empty()) return out;
        for (const auto& stored : records) {
            if (stored.kind == primechain::storage::StoredRecordKind::Prime) {
                const auto record = primechain::protocol::deserializePrimeRecord(stored.payload, error);
                if (!record.has_value()) return out;
                for (const auto& binding : record->validator_work_bindings) {
                    out[{binding.candidate_address, binding.miner_address}] = binding;
                }
                continue;
            }
            const auto record = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
            if (!record.has_value()) return out;
            for (const auto& binding : record->validator_work_bindings) {
                out[{binding.candidate_address, binding.miner_address}] = binding;
            }
        }
        return out;
    }

    std::vector<primechain::Address> validatorWorkSponsorsFor(const primechain::Address& candidate) const {
        std::set<primechain::Address> sponsors;
        const auto chain = validatorWorkBindingsFromChain();
        for (const auto& entry : chain) {
            if (entry.second.candidate_address == candidate) sponsors.insert(entry.second.miner_address);
        }
        for (const auto& entry : pending_validator_work_bindings_) {
            if (entry.second.candidate_address == candidate) sponsors.insert(entry.second.miner_address);
        }
        return {sponsors.begin(), sponsors.end()};
    }

    ValidatorCandidateStats validatorSponsoredStatsFromChain(const primechain::Address& candidate) const {
        ValidatorCandidateStats stats = validatorCandidateStatsFromChain(candidate);
        for (const auto& sponsor : validatorWorkSponsorsFor(candidate)) {
            if (sponsor == candidate) continue;
            const auto sponsor_stats = validatorCandidateStatsFromChain(sponsor);
            stats.prime_records += sponsor_stats.prime_records;
            stats.composite_records += sponsor_stats.composite_records;
        }
        return stats;
    }

    bool validatorAdmissionEligible(
            const primechain::node::SequentialNode& node,
            const primechain::Address& candidate,
            std::string& error) const {
        const auto application = validatorApplicationFor(candidate);
        if (!application.has_value()) {
            error = "candidate has no validator application";
            return false;
        }
        primechain::protocol::ValidatorEligibilityPolicyV0 policy;
        policy.min_reserve_micro_units = node.validatorMinReserveMicroUnits();
        const ValidatorCandidateStats raw_stats = validatorSponsoredStatsFromChain(candidate);
        const primechain::protocol::ValidatorWorkStatsV0 work_stats{
            raw_stats.prime_records,
            raw_stats.composite_records,
            0};
        if (!primechain::protocol::validatorMeetsWorkMinimumV0(work_stats, policy)) {
            error = "candidate does not meet validator work minimum";
            return false;
        }
        if (!primechain::protocol::validatorMeetsReserveMinimumV0(
                node.lockedValidatorReserveMicroUnits(candidate), policy)) {
            error = "candidate does not meet validator reserve minimum";
            return false;
        }
        if (!primechain::protocol::validatorMeetsEndpointUptimeMinimumV0(
                application->observed_successful, application->observed_total, policy)) {
            error = "candidate does not meet endpoint observation minimum";
            return false;
        }
        return true;
    }

    std::vector<primechain::protocol::ValidatorApplicationV1> embeddedValidatorApplicationsForNextRecord(
            const primechain::node::SequentialNode& node) {
        std::vector<primechain::protocol::ValidatorApplicationV1> out;
        for (auto it = pending_validator_applications_.begin(); it != pending_validator_applications_.end();) {
            const auto& application = it->second;
            if (application.record_integer < node.status().frontier_integer + 1) {
                it = pending_validator_applications_.erase(it);
                continue;
            }
            if (application.record_integer == node.status().frontier_integer + 1) out.push_back(application);
            ++it;
        }
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
            return left.candidate_address < right.candidate_address;
        });
        return out;
    }

    void sendValidatorApplications(int fd) const {
        auto applications = validatorApplicationsFromChain();
        for (const auto& pending : pending_validator_applications_) applications[pending.first] = pending.second;
        writeAll(fd, "VALIDATOR_APPLICATIONS " + std::to_string(applications.size()) + "\n");
        for (const auto& entry : applications) {
            const auto& application = entry.second;
            writeCommand(fd, "VALIDATOR_APPLICATION " + application.candidate_address + " " +
                application.host + " " + std::to_string(application.port) + " " +
                std::to_string(application.record_integer) + " " + std::to_string(application.sequence) + " " +
                std::to_string(application.observed_successful) + " " +
                std::to_string(application.observed_total) + " " +
                bytesToHex(application.public_key) + " " + bytesToHex(application.signature) + "\n");
        }
        writeAll(fd, "END_VALIDATOR_APPLICATIONS\n");
    }


    std::vector<primechain::protocol::ValidatorWorkBindingV1> embeddedValidatorWorkBindingsForNextRecord(
            const primechain::node::SequentialNode& node) {
        std::vector<primechain::protocol::ValidatorWorkBindingV1> out;
        for (auto it = pending_validator_work_bindings_.begin(); it != pending_validator_work_bindings_.end();) {
            const auto& binding = it->second;
            if (binding.record_integer < node.status().frontier_integer + 1) {
                it = pending_validator_work_bindings_.erase(it);
                continue;
            }
            if (binding.record_integer == node.status().frontier_integer + 1) out.push_back(binding);
            ++it;
        }
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
            if (left.candidate_address != right.candidate_address) return left.candidate_address < right.candidate_address;
            return left.miner_address < right.miner_address;
        });
        return out;
    }

    void sendValidatorWorkBindings(int fd) const {
        auto bindings = validatorWorkBindingsFromChain();
        for (const auto& pending : pending_validator_work_bindings_) bindings[pending.first] = pending.second;
        writeAll(fd, "VALIDATOR_WORK_BINDINGS " + std::to_string(bindings.size()) + "\n");
        for (const auto& entry : bindings) {
            const auto& binding = entry.second;
            writeCommand(fd, "VALIDATOR_WORK_BINDING " + binding.candidate_address + " " +
                binding.miner_address + " " + std::to_string(binding.record_integer) + " " +
                std::to_string(binding.sequence) + " " + bytesToHex(binding.miner_public_key) +
                " " + bytesToHex(binding.miner_signature) + "\n");
        }
        writeAll(fd, "END_VALIDATOR_WORK_BINDINGS\n");
    }

    void submitValidatorWorkBinding(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command, previous_hex, public_hex, signature_hex, extra;
        primechain::protocol::ValidatorWorkBindingV1 binding;
        in >> command >> previous_hex >> binding.record_integer >> binding.candidate_address
           >> binding.miner_address >> binding.sequence >> public_hex >> signature_hex;
        const auto previous = parseHash(previous_hex);
        if (!in || (in >> extra) ||
            (command != "SUBMIT_VALIDATOR_WORK_BINDING" && command != "SUBMIT_VALIDATOR_WORK_BINDING_PEER") ||
            !previous.has_value()) {
            writeAll(fd, "ERROR invalid SUBMIT_VALIDATOR_WORK_BINDING\n");
            return;
        }
        binding.miner_public_key = hexToBytes(public_hex);
        binding.miner_signature = hexToBytes(signature_hex);
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) { writeAll(fd, "ERROR " + error + "\n"); return; }
        if (*previous != node.status().latest_record_hash ||
            binding.record_integer != node.status().frontier_integer + 1) {
            writeAll(fd, "ERROR validator work binding does not match current chain state\n");
            return;
        }
        if (std::binary_search(node.validatorSet().begin(), node.validatorSet().end(), binding.candidate_address)) {
            writeAll(fd, "ERROR candidate is already an active validator\n");
            return;
        }
        if (!primechain::protocol::verifyValidatorWorkBindings(
                std::vector<primechain::protocol::ValidatorWorkBindingV1>{binding},
                *previous, binding.record_integer, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const auto key = std::make_pair(binding.candidate_address, binding.miner_address);
        const auto found = pending_validator_work_bindings_.find(key);
        if (found != pending_validator_work_bindings_.end() && found->second.sequence > binding.sequence) {
            writeAll(fd, "ERROR stale validator work binding sequence\n");
            return;
        }
        pending_validator_work_bindings_[key] = binding;
        if (propagate) {
            for (const auto& peer : peers_) {
                std::ostringstream command_out;
                command_out << "SUBMIT_VALIDATOR_WORK_BINDING_PEER " << previous_hex << " "
                            << binding.record_integer << " " << binding.candidate_address << " "
                            << binding.miner_address << " " << binding.sequence << " "
                            << public_hex << " " << signature_hex << "\n";
                std::string peer_error;
                auto socket = connectToServer(peer.host, peer.port);
                if (!socket.has_value()) {
                    peer_error = "could not connect to peer";
                } else if (!writeCommand(socket->fd(), command_out.str())) {
                    peer_error = "could not submit validator work binding";
                } else {
                    const auto response = readLine(socket->fd());
                    if (!response.has_value() ||
                        response->rfind("VALIDATOR_WORK_BINDING_ACCEPTED ", 0) != 0) {
                        peer_error = response.has_value() ? *response : "peer did not return work binding response";
                    }
                }
                if (!peer_error.empty()) {
                    std::cerr << "validator work binding propagation warning to " << peer.host << ":"
                              << peer.port << ": " << peer_error << "\n";
                }
            }
        }
        writeAll(fd, "VALIDATOR_WORK_BINDING_ACCEPTED " + binding.candidate_address +
            " miner=" + binding.miner_address + " sequence=" + std::to_string(binding.sequence) + "\n");
    }

    void submitValidatorApplication(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command, previous_hex, public_hex, signature_hex, extra;
        primechain::protocol::ValidatorApplicationV1 application;
        in >> command >> previous_hex >> application.record_integer >> application.candidate_address
           >> application.host >> application.port >> application.sequence
           >> application.observed_successful >> application.observed_total
           >> public_hex >> signature_hex;
        const auto previous = parseHash(previous_hex);
        if (!in || (in >> extra) ||
            (command != "SUBMIT_VALIDATOR_APPLICATION" && command != "SUBMIT_VALIDATOR_APPLICATION_PEER") ||
            !previous.has_value()) {
            writeAll(fd, "ERROR invalid SUBMIT_VALIDATOR_APPLICATION\n");
            return;
        }
        application.public_key = hexToBytes(public_hex);
        application.signature = hexToBytes(signature_hex);
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) { writeAll(fd, "ERROR " + error + "\n"); return; }
        if (*previous != node.status().latest_record_hash ||
            application.record_integer != node.status().frontier_integer + 1) {
            writeAll(fd, "ERROR validator application does not match current chain state\n");
            return;
        }
        if (std::binary_search(node.validatorSet().begin(), node.validatorSet().end(), application.candidate_address)) {
            writeAll(fd, "ERROR candidate is already an active validator\n");
            return;
        }
        if (!primechain::protocol::verifyValidatorApplications(
                std::vector<primechain::protocol::ValidatorApplicationV1>{application},
                *previous, application.record_integer, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const auto found = pending_validator_applications_.find(application.candidate_address);
        if (found != pending_validator_applications_.end() && found->second.sequence > application.sequence) {
            writeAll(fd, "ERROR stale validator application sequence\n");
            return;
        }
        pending_validator_applications_[application.candidate_address] = application;
        if (propagate) {
            for (const auto& peer : peers_) {
                std::ostringstream command_out;
                command_out << "SUBMIT_VALIDATOR_APPLICATION_PEER " << previous_hex << " "
                            << application.record_integer << " " << application.candidate_address << " "
                            << application.host << " " << application.port << " "
                            << application.sequence << " " << application.observed_successful << " "
                            << application.observed_total << " " << public_hex << " " << signature_hex << "\n";
                std::string peer_error;
                auto socket = connectToServer(peer.host, peer.port);
                if (!socket.has_value()) {
                    peer_error = "could not connect to peer";
                } else if (!writeCommand(socket->fd(), command_out.str())) {
                    peer_error = "could not submit validator application";
                } else {
                    const auto response = readLine(socket->fd());
                    if (!response.has_value() ||
                        response->rfind("VALIDATOR_APPLICATION_ACCEPTED ", 0) != 0) {
                        peer_error = response.has_value() ? *response : "peer did not return application response";
                    }
                }
                if (!peer_error.empty()) {
                    std::cerr << "validator application propagation warning to " << peer.host << ":"
                              << peer.port << ": " << peer_error << "\n";
                }
            }
        }
        writeAll(fd, "VALIDATOR_APPLICATION_ACCEPTED " + application.candidate_address +
            " " + application.host + " " + std::to_string(application.port) +
            " sequence=" + std::to_string(application.sequence) + "\n");
    }

    void submitValidatorEndpoint(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command, previous_hex, public_hex, signature_hex, extra;
        primechain::protocol::ValidatorEndpointUpdateV1 update;
        primechain::PrimeValue record_integer = 0;
        in >> command >> previous_hex >> record_integer >> update.validator_address
           >> update.host >> update.port >> update.effective_integer >> update.sequence
           >> public_hex >> signature_hex;
        const auto previous = parseHash(previous_hex);
        if (!in || (in >> extra) ||
            (command != "SUBMIT_VALIDATOR_ENDPOINT" && command != "SUBMIT_VALIDATOR_ENDPOINT_PEER") ||
            !previous.has_value()) {
            writeAll(fd, "ERROR invalid SUBMIT_VALIDATOR_ENDPOINT\n");
            return;
        }
        update.public_key = hexToBytes(public_hex);
        update.signature = hexToBytes(signature_hex);
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) { writeAll(fd, "ERROR " + error + "\n"); return; }
        if (*previous != node.status().latest_record_hash ||
            record_integer != node.status().frontier_integer + 1) {
            writeAll(fd, "ERROR validator endpoint update does not match current chain state\n");
            return;
        }
        if (!primechain::protocol::verifyValidatorEndpointUpdates(
                std::vector<primechain::protocol::ValidatorEndpointUpdateV1>{update},
                node.validatorSet(), *previous, record_integer, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const auto found = pending_endpoint_updates_.find(update.validator_address);
        if (found != pending_endpoint_updates_.end() && found->second.sequence > update.sequence) {
            writeAll(fd, "ERROR stale validator endpoint sequence\n");
            return;
        }
        pending_endpoint_updates_[update.validator_address] = update;
        if (propagate) {
            for (const auto& peer : peers_) {
                std::ostringstream command_out;
                command_out << "SUBMIT_VALIDATOR_ENDPOINT_PEER " << previous_hex << " " << record_integer
                            << " " << update.validator_address << " " << update.host << " "
                            << update.port << " " << update.effective_integer << " "
                            << update.sequence << " " << public_hex << " " << signature_hex << "\n";
                std::string peer_error;
                auto socket = connectToServer(peer.host, peer.port);
                if (!socket.has_value()) {
                    peer_error = "could not connect to peer";
                } else if (!writeCommand(socket->fd(), command_out.str())) {
                    peer_error = "could not submit validator endpoint";
                } else {
                    const auto response = readLine(socket->fd());
                    if (!response.has_value() ||
                        response->rfind("VALIDATOR_ENDPOINT_ACCEPTED ", 0) != 0) {
                        peer_error = response.has_value() ? *response : "peer did not return endpoint response";
                    }
                }
                if (!peer_error.empty()) {
                    std::cerr << "validator endpoint propagation warning to " << peer.host << ":"
                              << peer.port << ": " << peer_error << "\n";
                }
            }
        }
        writeAll(fd, "VALIDATOR_ENDPOINT_ACCEPTED " + update.validator_address + " " +
            update.host + " " + std::to_string(update.port) + " sequence=" +
            std::to_string(update.sequence) + "\n");
    }

    void clearEpochVotesAfterRecord() {
        if (epoch_votes_.empty()) return;
        epoch_votes_.clear();
        std::string error;
        if (!persistEpochVotes(error)) std::cerr << "epoch vote cleanup warning: " << error << "\n";
    }

    void clearEndpointUpdatesAfterRecord() {
        pending_endpoint_updates_.clear();
    }

    void clearValidatorApplicationsAfterRecord() {
        pending_validator_applications_.clear();
    }

    void clearValidatorWorkBindingsAfterRecord() {
        pending_validator_work_bindings_.clear();
    }

    void clearPolicyVotesAfterRecord() {
        policy_votes_.clear();
    }

    void submitSignedCommit(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue g = 0;
        std::string commitment_hex;
        std::string provider_address;
        std::string public_key_hex;
        std::string signature_hex;
        std::string extra;
        in >> command >> g >> commitment_hex >> provider_address >> public_key_hex >> signature_hex;
        const auto commitment_hash = parseHash(commitment_hex);
        const auto public_key = hexToBytes(public_key_hex);
        const auto signature = hexToBytes(signature_hex);
        if (!in || (command != "SUBMIT_SIGNED_COMMIT" && command != "SUBMIT_SIGNED_COMMIT_PEER") ||
            !commitment_hash.has_value() ||
            public_key.size() != primechain::crypto::signaturePublicKeySize(primechain::crypto::kProtocolSignatureAlgorithm) ||
            signature.size() != primechain::crypto::signatureSize(primechain::crypto::kProtocolSignatureAlgorithm) || (in >> extra)) {
            writeAll(fd, "ERROR invalid SUBMIT_SIGNED_COMMIT\n");
            return;
        }
        if (provider_address != primechain::crypto::addressFromProtocolPublicKey(public_key)) {
            writeAll(fd, "ERROR signed commitment address does not match public key\n");
            return;
        }
        std::string error;
        if (!primechain::crypto::verifyProtocolMessageSignature(
                public_key,
                primechain::crypto::compositeCommitSigningPayload(
                    g, *commitment_hash, provider_address),
                signature,
                error)) {
            writeAll(fd, "ERROR invalid signed commitment signature\n");
            return;
        }

        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const primechain::PrimeValue frontier =
            node.status().has_genesis ? node.status().frontier_integer : 2;
        if (g != frontier + 1) {
            writeAll(fd, "ERROR SUBMIT_SIGNED_COMMIT must target next integer "
                + std::to_string(frontier + 1) + "\n");
            return;
        }
        const auto key = std::make_tuple(g, activeCommitPhaseRound(g), provider_address);
        const auto existing = commitments_.find(key);
        if (existing != commitments_.end()) {
            if (existing->second.commitment_hash == *commitment_hash &&
                existing->second.public_key == public_key) {
                writeAll(fd, "COMMIT_DUPLICATE " + std::to_string(g) + " " + commitment_hex + "\n");
            } else {
                writeAll(fd, "ERROR provider already committed a different hash for integer "
                    + std::to_string(g) + "\n");
            }
            return;
        }
        if (quorumEnabled() && phaseFrozen(g)) {
            writeAll(fd, "ERROR commit phase is closing or closed\n");
            return;
        }
        if (commitments_.size() >= kMaxCompositeCommitments) {
            writeAll(fd, "ERROR commitment pool full; max="
                + std::to_string(kMaxCompositeCommitments) + "\n");
            return;
        }

        primechain::storage::StoredCommitment stored;
        stored.integer = g;
        stored.commit_round = activeCommitPhaseRound(g);
        stored.provider_address = provider_address;
        stored.commitment_hash = *commitment_hash;
        stored.public_key = public_key;
        stored.signature = signature;
        commitments_[key] = stored;
        if (!persistCommitments(error)) {
            commitments_.erase(key);
            writeAll(fd, "ERROR could not persist commitment: " + error + "\n");
            return;
        }
        if (propagate) propagateCommit(stored);
        writeAll(fd, "COMMIT_ACCEPTED " + std::to_string(g) + " " + commitment_hex + "\n");
    }

    void submitCommit(int fd, const std::string& line) {
        if (quorumEnabled()) {
            writeAll(fd, "ERROR unsigned commitments disabled in quorum mode\n");
            return;
        }
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue g = 0;
        std::string commitment_hex;
        std::string provider_address;
        in >> command >> g >> commitment_hex >> provider_address;
        std::string extra;
        const auto commitment = parseHash(commitment_hex);
        if (!in ||
            command != "SUBMIT_COMMIT" ||
            !commitment.has_value() ||
            !primechain::protocol::isDevelopmentAddress(provider_address) ||
            (in >> extra)) {
            writeAll(fd, "ERROR invalid SUBMIT_COMMIT; expected SUBMIT_COMMIT g commitment_hash provider_address\n");
            return;
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const primechain::PrimeValue frontier =
            node.status().has_genesis ? node.status().frontier_integer : 2;
        if (g != frontier + 1) {
            writeAll(fd, "ERROR SUBMIT_COMMIT must target next integer "
                + std::to_string(frontier + 1) + "\n");
            return;
        }
        if (quorumEnabled() && phaseFrozen(g)) {
            writeAll(fd, "ERROR commit phase is closing or closed\n");
            return;
        }

        for (auto it = commitments_.begin(); it != commitments_.end();) {
            if (std::get<0>(it->first) < g) {
                it = commitments_.erase(it);
            } else {
                ++it;
            }
        }

        const auto key = std::make_tuple(g, activeCommitPhaseRound(g), provider_address);
        const auto existing = commitments_.find(key);
        if (existing != commitments_.end()) {
            if (existing->second.commitment_hash == *commitment) {
                writeAll(fd, "COMMIT_DUPLICATE " + std::to_string(g) + " " + commitment_hex + "\n");
            } else {
                writeAll(fd, "ERROR provider already committed a different hash for integer "
                    + std::to_string(g) + "\n");
            }
            return;
        }
        if (commitments_.size() >= kMaxCompositeCommitments) {
            writeAll(fd, "ERROR commitment pool full; max="
                + std::to_string(kMaxCompositeCommitments) + "\n");
            return;
        }

        primechain::storage::StoredCommitment stored_commitment;
        stored_commitment.integer = g;
        stored_commitment.commit_round = activeCommitPhaseRound(g);
        stored_commitment.provider_address = provider_address;
        stored_commitment.commitment_hash = *commitment;
        commitments_[key] = stored_commitment;
        error.clear();
        if (!persistCommitments(error)) {
            commitments_.erase(key);
            writeAll(fd, "ERROR could not persist commitment: " + error + "\n");
            return;
        }
        propagateCommit(stored_commitment);
        writeAll(fd, "COMMIT_ACCEPTED " + std::to_string(g) + " " + commitment_hex + "\n");
    }

    void sendCommitments(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue integer = 0;
        std::string extra;
        in >> command >> integer;
        if (!in || command != "GET_COMMITMENTS" || (in >> extra)) {
            writeAll(fd, "ERROR invalid GET_COMMITMENTS; expected GET_COMMITMENTS g\n");
            return;
        }

        const auto commit_round = activeCommitPhaseRound(integer);
        std::size_t count = 0;
        for (const auto& entry : commitments_) {
            if (std::get<0>(entry.first) == integer && std::get<1>(entry.first) == commit_round) {
                ++count;
            }
        }
        writeAll(fd, "COMMITMENTS " + std::to_string(integer) + " " + std::to_string(count) + "\n");
        for (const auto& entry : commitments_) {
            if (std::get<0>(entry.first) != integer || std::get<1>(entry.first) != commit_round) {
                continue;
            }
            const auto& commitment = entry.second;
            writeCommand(fd, commitmentWireLine(commitment) + "\n");
        }
        writeAll(fd, "END_COMMITMENTS\n");
    }

    std::optional<primechain::storage::StoredCommitment> selectedCommitment(
        primechain::PrimeValue g) const {
        const auto commit_round = activeCommitPhaseRound(g);
        std::optional<primechain::storage::StoredCommitment> selected;
        for (const auto& entry : commitments_) {
            if (std::get<0>(entry.first) != g || std::get<1>(entry.first) != commit_round) {
                continue;
            }
            const auto& candidate = entry.second;
            if (!selected.has_value() ||
                candidate.commitment_hash < selected->commitment_hash ||
                (candidate.commitment_hash == selected->commitment_hash &&
                 candidate.provider_address < selected->provider_address)) {
                selected = candidate;
            }
        }
        return selected;
    }

    void sendCommitWinner(int fd, const std::string& line) const {
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue g = 0;
        std::string extra;
        in >> command >> g;
        if (!in || command != "GET_COMMIT_WINNER" || (in >> extra)) {
            writeAll(fd, "ERROR invalid GET_COMMIT_WINNER; expected GET_COMMIT_WINNER g\n");
            return;
        }

        const auto selected = selectedCommitment(g);
        if (!selected.has_value()) {
            writeAll(fd, "COMMIT_WINNER_NONE " + std::to_string(g) + "\n");
            return;
        }
        writeAll(fd, "COMMIT_WINNER " + std::to_string(g) + " "
            + primechain::crypto::toHex(selected->commitment_hash) + " "
            + selected->provider_address + "\n");
    }

    bool parseSignedCompositeReveal(
        const std::string& line,
        SignedCompositeReveal& reveal,
        std::string& error) const {
        std::istringstream in(line);
        std::string command;
        std::string public_key_hex;
        std::string signature_hex;
        std::string extra;
        in >> command >> reveal.g >> reveal.d >> reveal.e >> reveal.nonce
           >> reveal.provider_address >> public_key_hex >> signature_hex;
        reveal.public_key = hexToBytes(public_key_hex);
        reveal.signature = hexToBytes(signature_hex);
        if (!in || (command != "SUBMIT_SIGNED_REVEAL" && command != "SUBMIT_SIGNED_REVEAL_PEER") ||
            reveal.public_key.size() != primechain::crypto::signaturePublicKeySize(primechain::crypto::kProtocolSignatureAlgorithm) ||
            reveal.signature.size() != primechain::crypto::signatureSize(primechain::crypto::kProtocolSignatureAlgorithm) ||
            (in >> extra)) {
            error = "invalid SUBMIT_SIGNED_REVEAL";
            return false;
        }
        if (reveal.provider_address != primechain::crypto::addressFromProtocolPublicKey(reveal.public_key)) {
            error = "signed reveal address does not match public key";
            return false;
        }
        if (!primechain::crypto::verifyProtocolMessageSignature(
                reveal.public_key,
                primechain::crypto::compositeRevealSigningPayload(
                    reveal.g, reveal.d, reveal.e, reveal.nonce, reveal.provider_address),
                reveal.signature,
                error)) {
            error = "invalid signed reveal signature";
            return false;
        }
        return true;
    }

    bool rememberPendingReveal(const SignedCompositeReveal& reveal, std::string& error) {
        const auto key = std::make_tuple(reveal.g, activeCommitPhaseRound(reveal.g), reveal.provider_address);
        const auto existing = pending_reveals_.find(key);
        if (existing != pending_reveals_.end()) {
            if (existing->second.d == reveal.d && existing->second.e == reveal.e &&
                existing->second.nonce == reveal.nonce &&
                existing->second.public_key == reveal.public_key &&
                existing->second.signature == reveal.signature) {
                return true;
            }
            error = "provider already revealed different composite evidence";
            return false;
        }
        pending_reveals_[key] = reveal;
        return true;
    }

    void propagateReveal(const SignedCompositeReveal& reveal) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitRevealToPeer(peer, reveal, error)) {
                std::cerr << "reveal propagation warning to " << peer.host << ":"
                          << peer.port << ": " << error << "\n";
            }
        }
    }

    void syncCommitmentsFromPeersFor(primechain::PrimeValue) {
        for (const auto& peer : peers_) {
            std::string error;
            if (!syncCommitmentsFromPeer(peer.host, peer.port, error)) {
                std::cerr << "commitment sync warning from " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
            }
        }
    }

    void submitSignedCompositeReveal(int fd, const std::string& line, bool propagate) {
        SignedCompositeReveal reveal;
        std::string error;
        if (!parseSignedCompositeReveal(line, reveal, error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }

        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const primechain::PrimeValue frontier =
            node.status().has_genesis ? node.status().frontier_integer : 2;
        if (reveal.g != frontier + 1) {
            writeAll(fd, "ERROR SUBMIT_SIGNED_REVEAL must target next integer "
                + std::to_string(frontier + 1) + "\n");
            return;
        }


        const auto packed_proof = primechain::crypto::packCompositeRevealProof(
            reveal.public_key, reveal.nonce, reveal.signature);
        std::ostringstream submission;
        submission << "SUBMIT_COMPOSITE " << reveal.g << " " << reveal.d << " " << reveal.e << " "
                   << reveal.provider_address << " " << bytesToHex(packed_proof);
        submitComposite(fd, submission.str(), true);
    }

    struct CompositeLotteryCandidate {
        primechain::Address provider_address;
        primechain::Hash256 candidate_hash{};
    };

    struct CompositeLotteryRoundState {
        std::uint64_t round{1};
        primechain::Hash256 previous_record_hash{};
        std::chrono::steady_clock::time_point opened_at{};
        std::vector<CompositeLotteryCandidate> candidates;
        bool decided{false};
        primechain::Hash256 winning_candidate_hash{};
    };

    bool compositeLotteryEnabled() const {
        return composite_lottery_window_ms_ > 0;
    }

    bool compositeLotteryCandidateEquals(
        const CompositeLotteryCandidate& candidate,
        const primechain::Hash256& candidate_hash) const {
        return candidate.candidate_hash == candidate_hash;
    }

    bool passCompositeLottery(
        primechain::protocol::CompositeRecordV0& record,
        std::string& error) {
        if (!compositeLotteryEnabled()) return true;
        if (!localValidatorActive()) {
            error = "composite lottery requires an active local validator identity";
            return false;
        }
        record.version = std::max<std::uint64_t>(record.version, primechain::node::kCompositeLotteryRecordVersion);
        record.composite_lottery = {};
        const auto subject_hash = primechain::protocol::compositeLotterySubjectHash(record);
        const auto assigned = primechain::protocol::assignedCompositeLotteryValidator(record, validator_set_, error);
        if (!assigned.has_value()) return false;
        if (*assigned != validator_identity_->address) {
            error = "composite lottery assigned to " + *assigned;
            return false;
        }

        const auto candidate_hash = subject_hash;
        const auto window = std::chrono::milliseconds(composite_lottery_window_ms_);
        const auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point deadline;
        std::uint64_t observed_round = 1;

        {
            std::lock_guard<std::mutex> lock(composite_lottery_mutex_);
            auto& state = composite_lottery_[record.integer];
            if (state.candidates.empty() && !state.decided) {
                state.round = 1;
                state.previous_record_hash = record.previous_record_hash;
                state.opened_at = now;
            }
            if (state.previous_record_hash != record.previous_record_hash) {
                state = CompositeLotteryRoundState{};
                state.round = 1;
                state.previous_record_hash = record.previous_record_hash;
                state.opened_at = now;
            }
            observed_round = state.round;
            if (state.decided) {
                if (record.version < primechain::protocol::kIntegerCompositeLotteryRecordVersion &&
                    state.winning_candidate_hash != candidate_hash) {
                    error = "composite lottery selected a different candidate";
                    return false;
                }
            }
            observed_round = state.round;
            if (state.candidates.size() >= kMaxCompositeLotteryCandidates &&
                std::none_of(state.candidates.begin(), state.candidates.end(),
                    [&](const auto& candidate) { return compositeLotteryCandidateEquals(candidate, candidate_hash); })) {
                error = "composite lottery candidate set is full";
                return false;
            }
            if (std::none_of(state.candidates.begin(), state.candidates.end(),
                    [&](const auto& candidate) { return compositeLotteryCandidateEquals(candidate, candidate_hash); })) {
                state.candidates.push_back({record.proof.provider_address, candidate_hash});
            }
            deadline = state.opened_at + window;
        }

        const auto after_registration = std::chrono::steady_clock::now();
        if (after_registration < deadline) {
            std::this_thread::sleep_until(deadline);
        }

        std::lock_guard<std::mutex> lock(composite_lottery_mutex_);
        auto found = composite_lottery_.find(record.integer);
        if (found == composite_lottery_.end()) {
            error = "composite lottery state disappeared";
            return false;
        }
        auto& state = found->second;
        if (state.previous_record_hash != record.previous_record_hash) {
            error = "composite lottery previous hash changed";
            return false;
        }
        if (!state.decided) {
            state.decided = true;
            if (state.candidates.empty()) {
                error = "composite lottery candidate set is empty";
                return false;
            }

            const auto resolved_round = state.round;
            std::random_device device;
            std::mt19937_64 rng(device());
            std::uniform_int_distribution<std::uint32_t> win_dist(1, 10000);
            const auto draw = win_dist(rng);
            if (draw > composite_lottery_win_bps_) {
                std::cerr << "composite lottery lost integer " << record.integer
                          << " round " << resolved_round << " candidates " << state.candidates.size()
                          << " draw " << draw << " win_bps " << composite_lottery_win_bps_ << "\n";
                state.decided = false;
                state.candidates.clear();
                state.winning_candidate_hash = {};
                state.opened_at = std::chrono::steady_clock::now();
                state.round = resolved_round + 1;
                error = "composite lottery lost round " + std::to_string(resolved_round);
                return false;
            }

            if (record.version >= primechain::protocol::kIntegerCompositeLotteryRecordVersion) {
                state.winning_candidate_hash = {};
                std::cerr << "composite lottery winner integer " << record.integer
                          << " round " << state.round << " candidates " << state.candidates.size()
                          << " draw " << draw << " win_bps " << composite_lottery_win_bps_
                          << " scope integer\n";
            } else {
                std::uniform_int_distribution<std::size_t> pick_dist(0, state.candidates.size() - 1);
                state.winning_candidate_hash = state.candidates[pick_dist(rng)].candidate_hash;
                std::cerr << "composite lottery winner integer " << record.integer
                          << " round " << state.round << " candidates " << state.candidates.size()
                          << " draw " << draw << " win_bps " << composite_lottery_win_bps_
                          << " hash " << primechain::crypto::toHex(state.winning_candidate_hash) << "\n";
            }
        }
        if (record.version < primechain::protocol::kIntegerCompositeLotteryRecordVersion &&
            state.winning_candidate_hash != candidate_hash) {
            error = "composite lottery selected a different candidate";
            return false;
        }

        record.composite_lottery.round = state.round;
        record.composite_lottery.win_bps = composite_lottery_win_bps_;
        record.composite_lottery.subject_hash = subject_hash;
        record.composite_lottery.assigned_validator = validator_identity_->address;
        record.composite_lottery.public_key = validator_identity_->public_key;
        record.composite_lottery.signature = primechain::crypto::signProtocolMessage(
            validator_identity_->private_key,
            primechain::crypto::compositeLotteryWinSigningPayload(
                record.previous_record_hash, record.integer, subject_hash,
                state.round, composite_lottery_win_bps_, validator_identity_->address),
            error).value_or(primechain::crypto::Bytes{});
        if (record.composite_lottery.signature.empty()) {
            if (error.empty()) error = "could not sign composite lottery win";
            return false;
        }
        return true;
    }

    bool ensureCompositeLottery(
        primechain::protocol::CompositeRecordV0& record,
        std::string& error) {
        if (!compositeLotteryEnabled()) return true;
        record.version = std::max<std::uint64_t>(record.version, primechain::node::kCompositeLotteryRecordVersion);
        record.composite_lottery = {};
        const auto assigned = primechain::protocol::assignedCompositeLotteryValidator(record, validator_set_, error);
        if (!assigned.has_value()) return false;
        if (localValidatorActive() && *assigned == validator_identity_->address) {
            return passCompositeLottery(record, error);
        }
        for (const auto& peer : peers_) {
            std::string peer_error;
            const auto proof = requestCompositeLotteryWin(peer, record, peer_error);
            if (!proof.has_value()) {
                std::cerr << "composite lottery peer warning from " << peer.host << ":"
                          << peer.port << ": " << peer_error << "\n";
                continue;
            }
            record.composite_lottery = *proof;
            if (primechain::protocol::verifyCompositeLotteryProof(record, validator_set_, peer_error)) {
                return true;
            }
            std::cerr << "composite lottery peer proof rejected from " << peer.host << ":"
                      << peer.port << ": " << peer_error << "\n";
            record.composite_lottery = {};
        }
        error = "could not collect assigned composite lottery win from " + *assigned;
        return false;
    }

    void clearCompositeLottery(primechain::PrimeValue integer) {
        std::lock_guard<std::mutex> lock(composite_lottery_mutex_);
        composite_lottery_.erase(integer);
    }

    void submitCompositeReveal(int fd, const std::string& line) {
        if (quorumEnabled()) {
            writeAll(fd, "ERROR unsigned reveals disabled in quorum mode\n");
            return;
        }
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue g = 0;
        primechain::PrimeValue d = 0;
        primechain::PrimeValue e = 0;
        std::uint64_t nonce = 0;
        std::string provider_address;
        in >> command >> g >> d >> e >> nonce >> provider_address;
        std::string extra;
        if (!in ||
            command != "SUBMIT_COMPOSITE_REVEAL" ||
            !primechain::protocol::isDevelopmentAddress(provider_address) ||
            (in >> extra)) {
            writeAll(fd, "ERROR invalid SUBMIT_COMPOSITE_REVEAL; expected SUBMIT_COMPOSITE_REVEAL g d e nonce provider_address\n");
            return;
        }
        if (quorumEnabled() && !phaseClosed(g)) {
            writeAll(fd, "ERROR commit phase is not closed by validator quorum\n");
            return;
        }

        const auto key = std::make_tuple(g, activeCommitPhaseRound(g), provider_address);
        const auto existing = commitments_.find(key);
        if (existing == commitments_.end()) {
            writeAll(fd, "ERROR no prior commitment for reveal\n");
            return;
        }
        const auto revealed = primechain::crypto::compositeCommitment(
            g, d, e, nonce, provider_address);
        if (revealed != existing->second.commitment_hash) {
            writeAll(fd, "ERROR reveal does not match prior commitment\n");
            return;
        }

        const auto selected = selectedCommitment(g);
        if (!selected.has_value() ||
            selected->provider_address != provider_address ||
            selected->commitment_hash != existing->second.commitment_hash) {
            writeAll(fd, "ERROR commitment not selected for reveal; winner="
                + (selected.has_value() ? selected->provider_address : std::string("none")) + "\n");
            return;
        }

        std::ostringstream legacy_submission;
        legacy_submission << "SUBMIT_COMPOSITE " << g << " " << d << " " << e << " "
                          << provider_address;
        submitComposite(fd, legacy_submission.str(), true);
    }

    void submitComposite(int fd, const std::string& line, bool authorized_by_phase = false) {
        if (quorumEnabled() && !authorized_by_phase) {
            writeAll(fd, "ERROR direct SUBMIT_COMPOSITE disabled in quorum mode; use signed commit-reveal\n");
            return;
        }
        std::istringstream in(line);
        std::string command;
        primechain::PrimeValue g = 0;
        primechain::PrimeValue d = 0;
        primechain::PrimeValue e = 0;
        std::string provider_address;
        std::string proof_signature_hex;
        in >> command >> g >> d >> e >> provider_address;
        if (!in ||
            command != "SUBMIT_COMPOSITE" ||
            !primechain::protocol::isProtocolAddress(provider_address)) {
            writeAll(fd, "ERROR invalid SUBMIT_COMPOSITE; expected SUBMIT_COMPOSITE g d e provider_address [proof_signature]\n");
            return;
        }
        std::string extra;
        if (in >> proof_signature_hex) {
            if (in >> extra) {
                writeAll(fd, "ERROR invalid SUBMIT_COMPOSITE trailing data\n");
                return;
            }
        } else {
            in.clear();
        }

        primechain::CompositeProof proof;
        proof.m = g;
        proof.d = d;
        proof.e = e;
        proof.provider_address = provider_address;
        if (!proof_signature_hex.empty()) {
            proof.signature = hexToBytes(proof_signature_hex);
        }
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
            if (!node.initializeGenesis(validator_set_, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            propagateRecord(
                primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0(validator_set_)));
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
            if (existing->kind == primechain::storage::StoredRecordKind::Composite) {
                error.clear();
                const auto existing_record = primechain::protocol::deserializeCompositeRecord(
                    existing->payload, error);
                if (!existing_record.has_value()) {
                    writeAll(fd, "ERROR " + error + "\n");
                    return;
                }
                if (existing_record->proof.g == proof.m &&
                    existing_record->proof.d == proof.d &&
                    existing_record->proof.e == proof.e &&
                    existing_record->proof.provider_address == provider_address) {
                    writeAll(fd, "RECORD_DUPLICATE "
                        + primechain::crypto::toHex(existing->record_hash) + "\n");
                    return;
                }
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
        std::vector<primechain::protocol::TransactionV0> included_transactions = mempoolSnapshot();
        record.transactions = included_transactions;
        if (quorumEnabled()) {
            record.version = primechain::node::kTransactionMerkleRecordVersion;
            auto validator_epoch = embeddedValidatorEpochForNextRecord(node);
            if (validator_epoch.epoch != 0) {
                record.version = std::max<std::uint64_t>(record.version, 2);
                record.validator_epoch = std::move(validator_epoch);
            }
            auto endpoint_updates = embeddedValidatorEndpointsForNextRecord(node);
            if (!endpoint_updates.empty()) {
                record.version = std::max<std::uint64_t>(record.version, 3);
                record.validator_endpoints = std::move(endpoint_updates);
            }
            auto economic_policy = embeddedEconomicPolicyForNextRecord(node);
            if (economic_policy.transfer_fee_micro_units != 0) {
                record.version = std::max<std::uint64_t>(record.version, economic_policy.validator_min_reserve_micro_units != 0 ? 7 : 4);
                record.economic_policy = std::move(economic_policy);
            }
            auto validator_applications = embeddedValidatorApplicationsForNextRecord(node);
            if (!validator_applications.empty()) {
                record.version = std::max<std::uint64_t>(record.version, 5);
                record.validator_applications = std::move(validator_applications);
            }
            auto validator_work_bindings = embeddedValidatorWorkBindingsForNextRecord(node);
            if (!validator_work_bindings.empty()) {
                record.version = std::max<std::uint64_t>(record.version, 6);
                record.validator_work_bindings = std::move(validator_work_bindings);
            }
            primechain::protocol::updateTransactionBatch(record);
            error.clear();
            if (!ensureCompositeLottery(record, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            if (!finalizeRecordCandidate(
                    record, primechain::storage::StoredRecordKind::Composite, error)) {
                std::string sync_error;
                if (syncFromPeersPastInteger(record.integer, sync_error)) {
                    writeAll(fd, "RECORD_DUPLICATE " + std::to_string(record.integer) + " synced\n");
                    return;
                }
                std::string timeout_error;
                if (finalization_timeout_ms_ > 0 &&
                    advanceCommitPhaseRound(record.previous_record_hash, record.integer, timeout_error)) {
                    writeAll(fd, "ERROR commit phase timed out; retry integer "
                        + std::to_string(record.integer) + "\n");
                    return;
                }
                writeAll(fd, "ERROR could not finalize composite record: " + error + "\n");
                return;
            }
        }
        if (!quorumEnabled()) {
            primechain::protocol::applyDevelopmentFinalization(record);
        } else {
            primechain::protocol::updateTransactionBatch(record);
            error.clear();
            if (!ensureCompositeLottery(record, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
        }
        error.clear();
        if (!node.appendComposite(record, error)) {
            writeAll(fd, "ERROR could not append composite record: " + error + "\n");
            return;
        }
        validator_set_ = node.validatorSet();
        clearEpochVotesAfterRecord();
        clearEndpointUpdatesAfterRecord();
        clearPolicyVotesAfterRecord();
        clearValidatorApplicationsAfterRecord();
        clearValidatorWorkBindingsAfterRecord();
        removeMempoolTransactions(included_transactions);
        revalidateMempool();

        const auto stored = primechain::storage::makeStoredRecord(record);
        clearSignedCandidate(record.integer);
        clearCompositeLottery(record.integer);
        propagateRecord(stored);
        writeAll(fd, "COMPOSITE_ACCEPTED "
            + std::to_string(g)
            + " "
            + primechain::crypto::toHex(stored.record_hash)
            + "\n");
    }

    void propagateSignedPrime(const std::string& line) const {
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitSignedPrimeToPeer(peer, line, error)) {
                std::cerr << "prime evidence propagation warning to " << peer.host << ":"
                          << peer.port << ": " << error << "\n";
            }
        }
    }

    void submitPrime(int fd, const std::string& line, bool propagate) {
        std::istringstream in(line);
        std::string command;
        primechain::math::PrattProof proof;
        std::uint64_t factor_count = 0;
        std::string provider_address;
        std::string public_key_hex;
        std::string signature_hex;
        in >> command >> proof.p >> proof.witness >> factor_count;
        const bool peer_signed_submission = command == "SUBMIT_SIGNED_PRIME_PEER";
        const bool signed_submission = command == "SUBMIT_SIGNED_PRIME" || peer_signed_submission;
        if (!in || (command != "SUBMIT_PRIME" && !signed_submission) || factor_count > 64) {
            writeAll(fd, "ERROR invalid prime submission; expected SUBMIT_SIGNED_PRIME p witness factor_count factor exponent ... provider_address public_key signature\n");
            return;
        }
        for (std::uint64_t i = 0; i < factor_count; ++i) {
            primechain::math::PrimePowerFactor factor;
            in >> factor.prime >> factor.exponent;
            if (!in) {
                writeAll(fd, "ERROR invalid prime submission factor list\n");
                return;
            }
            proof.factors_of_p_minus_1.factors.push_back(factor);
        }
        in >> provider_address;
        if (signed_submission) in >> public_key_hex >> signature_hex;
        if (!in || (signed_submission
                ? !primechain::crypto::isProtocolSignatureAddress(provider_address)
                : !primechain::protocol::isDevelopmentAddress(provider_address))) {
            writeAll(fd, "ERROR invalid prime submission provider address\n");
            return;
        }
        std::string extra;
        if (in >> extra) {
            writeAll(fd, "ERROR invalid prime submission trailing data\n");
            return;
        }
        if (!signed_submission) {
            writeAll(fd, "ERROR unsigned prime submissions are disabled\n");
            return;
        }

        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        if (node.status().has_genesis && proof.p != node.status().frontier_integer + 1 &&
            !(proof.p == node.status().frontier_integer && node.status().frontier_integer > 2)) {
            std::ostringstream out;
            out << "ERROR prime submission must target current frontier "
                << node.status().frontier_integer
                << " or next integer "
                << (node.status().frontier_integer + 1) << "\n";
            writeAll(fd, out.str());
            return;
        }

        if (!primechain::math::verifyPrattProof(proof)) {
            writeAll(fd, "ERROR invalid Pratt proof\n");
            return;
        }

        primechain::protocol::Bytes authentication;
        if (signed_submission) {
            const auto public_key = hexToBytes(public_key_hex);
            const auto signature = hexToBytes(signature_hex);
            if (public_key.size() != primechain::crypto::signaturePublicKeySize(primechain::crypto::kProtocolSignatureAlgorithm) ||
                signature.size() != primechain::crypto::signatureSize(primechain::crypto::kProtocolSignatureAlgorithm) ||
                provider_address != primechain::crypto::addressFromProtocolPublicKey(public_key)) {
                writeAll(fd, "ERROR invalid signed prime identity\n");
                return;
            }
            authentication = primechain::crypto::packPrimeProofAuthentication(public_key, signature);
        }

        if (!node.status().has_genesis) {
            error.clear();
            if (!node.initializeGenesis(validator_set_, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            propagateRecord(
                primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0(validator_set_)));
        }

        if (proof.p == node.status().frontier_integer && node.status().frontier_integer > 2) {
            const auto existing = store_.findByInteger(proof.p, error);
            if (!error.empty() || !existing.has_value()) {
                writeAll(fd, "ERROR current frontier record not found\n");
                return;
            }
            error.clear();
            const auto previous_hash = previousRecordHash(*existing, error);
            if (!previous_hash.has_value()) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            if (signed_submission) {
                std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors;
                for (const auto& factor : proof.factors_of_p_minus_1.factors) {
                    factors.push_back({factor.prime, factor.exponent});
                }
                if (!primechain::crypto::verifyPackedPrimeProofAuthentication(
                        *previous_hash, proof.p, proof.witness, factors,
                        provider_address, authentication, error)) {
                    writeAll(fd, "ERROR invalid prime provider signature: " + error + "\n");
                    return;
                }
            }
            primechain::protocol::PrimeRecordV0 record;
            record.version = 0;
            record.height = node.status().height;
            record.previous_record_hash = *previous_hash;
            record.integer = proof.p;
            record.proof.p = proof.p;
            record.proof.witness = proof.witness;
            for (const auto& factor : proof.factors_of_p_minus_1.factors) {
                record.proof.factors_of_p_minus_1.push_back({factor.prime, factor.exponent});
            }
            record.proof.provider_address = provider_address;
            record.proof.signature = authentication;
            primechain::protocol::applyDevelopmentFinalization(record);
            const auto stored = primechain::storage::makeStoredRecord(record);
            handleExistingOrConflictingRecord(fd, stored, node.status().frontier_integer);
            return;
        }

        if (proof.p != node.status().frontier_integer + 1) {
            std::ostringstream out;
            out << "ERROR prime submission must extend frontier "
                << node.status().frontier_integer << " with integer "
                << (node.status().frontier_integer + 1) << "\n";
            writeAll(fd, out.str());
            return;
        }

        if (signed_submission) {
            std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors;
            for (const auto& factor : proof.factors_of_p_minus_1.factors) {
                factors.push_back({factor.prime, factor.exponent});
            }
            if (!primechain::crypto::verifyPackedPrimeProofAuthentication(
                    node.status().latest_record_hash, proof.p, proof.witness, factors,
                    provider_address, authentication, error)) {
                writeAll(fd, "ERROR invalid prime provider signature: " + error + "\n");
                return;
            }
        }

        if (peer_signed_submission) {
            writeAll(fd, "PRIME_EVIDENCE_ACCEPTED " + std::to_string(proof.p) + "\n");
            return;
        }

        if (quorumEnabled() && propagate && signed_submission) {
            propagateSignedPrime(line);
        }

        auto record = makePrimeRecord(
            node.status(), proof.p, proof, provider_address, authentication);
        std::vector<primechain::protocol::TransactionV0> included_transactions = mempoolSnapshot();
        record.transactions = included_transactions;
        if (quorumEnabled()) record.version = primechain::node::kTransactionMerkleRecordVersion;
        auto validator_epoch = embeddedValidatorEpochForNextRecord(node);
        if (validator_epoch.epoch != 0) {
            record.version = std::max<std::uint64_t>(record.version, 2);
            record.validator_epoch = std::move(validator_epoch);
        }
        auto endpoint_updates = embeddedValidatorEndpointsForNextRecord(node);
        if (!endpoint_updates.empty()) {
            record.version = std::max<std::uint64_t>(record.version, 3);
            record.validator_endpoints = std::move(endpoint_updates);
        }
        auto economic_policy = embeddedEconomicPolicyForNextRecord(node);
        if (economic_policy.transfer_fee_micro_units != 0) {
            record.version = std::max<std::uint64_t>(record.version, economic_policy.validator_min_reserve_micro_units != 0 ? 7 : 4);
            record.economic_policy = std::move(economic_policy);
        }
        auto validator_applications = embeddedValidatorApplicationsForNextRecord(node);
        if (!validator_applications.empty()) {
            record.version = std::max<std::uint64_t>(record.version, 5);
            record.validator_applications = std::move(validator_applications);
        }
        auto validator_work_bindings = embeddedValidatorWorkBindingsForNextRecord(node);
        if (!validator_work_bindings.empty()) {
            record.version = std::max<std::uint64_t>(record.version, 6);
            record.validator_work_bindings = std::move(validator_work_bindings);
        }
        if (quorumEnabled()) {
            primechain::protocol::updateTransactionBatch(record);
            if (!finalizeRecordCandidate(
                    record, primechain::storage::StoredRecordKind::Prime, error)) {
                std::string sync_error;
                if (syncFromPeersPastInteger(record.integer, sync_error)) {
                    writeAll(fd, "RECORD_DUPLICATE " + std::to_string(record.integer) + " synced\n");
                    return;
                }
                writeAll(fd, "ERROR could not finalize prime record: " + error + "\n");
                return;
            }
        }
        if (!quorumEnabled()) {
            primechain::protocol::applyDevelopmentFinalization(record);
        } else {
            primechain::protocol::updateTransactionBatch(record);
        }
        error.clear();
        if (!node.appendPrime(record, error)) {
            writeAll(fd, "ERROR could not append prime record: " + error + "\n");
            return;
        }
        validator_set_ = node.validatorSet();
        clearEpochVotesAfterRecord();
        clearEndpointUpdatesAfterRecord();
        clearPolicyVotesAfterRecord();
        clearValidatorApplicationsAfterRecord();
        clearValidatorWorkBindingsAfterRecord();
        removeMempoolTransactions(included_transactions);
        revalidateMempool();

        const auto stored = primechain::storage::makeStoredRecord(record);
        clearSignedCandidate(record.integer);
        propagateRecord(stored);
        writeAll(fd, "PRIME_ACCEPTED " + std::to_string(proof.p) + " "
            + primechain::crypto::toHex(stored.record_hash) + "\n");
    }

    void propagateRecord(const primechain::storage::StoredRecord& record) {
        pruneFinalizedCommitments(record.integer);
        for (const auto& peer : peers_) {
            std::string error;
            if (!submitRecordToPeer(peer, record, error)) {
                std::cerr << "record propagation warning to " << peer.host << ":" << peer.port
                          << ": " << error << "\n";
            }
        }
    }

    void sendMempoolSummary(int fd) {
        std::vector<primechain::protocol::TransactionV0> snapshot;
        std::map<std::string, std::uint64_t> first_seen_snapshot;
        {
            std::lock_guard<std::mutex> lock(mempool_mutex_);
            revalidateMempoolLocked();
            snapshot = mempool_;
            first_seen_snapshot = mempool_first_seen_;
        }
        std::map<primechain::Address, std::size_t> by_sender;
        std::uint64_t total_inputs = 0;
        std::uint64_t total_outputs = 0;
        std::uint64_t total_fees = 0;
        std::uint64_t oldest_age = 0;
        std::uint64_t newest_age = 0;
        const auto now = currentUnixTime();
        bool saw_age = false;
        for (const auto& tx : snapshot) {
            ++by_sender[tx.sender_address];
            for (const auto& input : tx.inputs) total_inputs += input.amount.numerator;
            for (const auto& output : tx.outputs) total_outputs += output.amount.numerator;
            total_fees += tx.fee.amount.numerator;
            const auto hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
            const auto found = first_seen_snapshot.find(hash);
            const auto first_seen = found == first_seen_snapshot.end() ? now : found->second;
            const auto age = first_seen > now ? 0 : now - first_seen;
            if (!saw_age || age > oldest_age) oldest_age = age;
            if (!saw_age || age < newest_age) newest_age = age;
            saw_age = true;
        }
        std::ostringstream out;
        out << "MEMPOOL_SUMMARY"
            << " transactions=" << snapshot.size()
            << " max_transactions=" << kMaxMempoolTransactions
            << " max_per_sender=" << kMaxMempoolTransactionsPerSender
            << " max_age_seconds=" << kMempoolMaxTransactionAgeSeconds
            << " unique_senders=" << by_sender.size()
            << " total_input_micro_units=" << total_inputs
            << " total_output_micro_units=" << total_outputs
            << " total_fee_micro_units=" << total_fees
            << " oldest_age_seconds=" << oldest_age
            << " newest_age_seconds=" << newest_age
            << " active_peers=" << activeKnownPeers().size()
            << "\n";
        for (const auto& entry : by_sender) {
            out << "MEMPOOL_SENDER address=" << entry.first
                << " transactions=" << entry.second << "\n";
        }
        out << "END_MEMPOOL_SUMMARY\n";
        writeAll(fd, out.str());
    }

    void sendMempool(int fd) {
        const auto snapshot = mempoolSnapshot();
        std::ostringstream header;
        header << "MEMPOOL " << snapshot.size() << "\n";
        writeAll(fd, header.str());
        for (const auto& tx : snapshot) {
            const auto bytes = primechain::protocol::serializeTransaction(tx, true);
            writeCommand(fd, "TX "
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
        std::size_t remaining = 0;
        {
            std::lock_guard<std::mutex> lock(mempool_mutex_);
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
                    mempool_first_seen_.erase(tx_hash);
                } else {
                    retained.push_back(tx);
                }
            }
            mempool_ = std::move(retained);
            remaining = mempool_.size();
        }

        std::ostringstream out;
        out << "MEMPOOL_ACKED " << removed << " " << remaining << "\n";
        writeAll(fd, out.str());
    }

    void removeMempoolTransactions(const std::vector<primechain::protocol::TransactionV0>& included) {
        if (included.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mempool_mutex_);
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
            } else {
                mempool_first_seen_.erase(primechain::crypto::toHex(tx_hash));
            }
        }
        mempool_ = std::move(retained);
    }

    std::size_t mempoolSize() const {
        std::lock_guard<std::mutex> lock(mempool_mutex_);
        return mempool_.size();
    }

    std::vector<primechain::protocol::TransactionV0> mempoolSnapshot() {
        std::lock_guard<std::mutex> lock(mempool_mutex_);
        revalidateMempoolLocked();
        return mempool_;
    }

    void revalidateMempool() {
        std::lock_guard<std::mutex> lock(mempool_mutex_);
        revalidateMempoolLocked();
    }

    void revalidateMempoolLocked() {
        primechain::node::SequentialNode node(store_path_);
        std::string error;
        if (!node.load(error)) {
            return;
        }
        const auto now = currentUnixTime();
        std::vector<primechain::protocol::TransactionV0> retained;
        retained.reserve(mempool_.size());
        for (const auto& tx : mempool_) {
            const auto tx_hash = primechain::crypto::toHex(primechain::protocol::transactionHash(tx));
            auto first_seen = mempool_first_seen_.find(tx_hash);
            if (first_seen == mempool_first_seen_.end()) {
                first_seen = mempool_first_seen_.emplace(tx_hash, now).first;
            }
            if (first_seen->second <= now && now - first_seen->second > kMempoolMaxTransactionAgeSeconds) {
                mempool_first_seen_.erase(first_seen);
                continue;
            }
            auto candidate = retained;
            candidate.push_back(tx);
            error.clear();
            if (node.validatePendingTransactions(candidate, error)) {
                retained.push_back(tx);
            } else {
                mempool_first_seen_.erase(tx_hash);
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
            !primechain::protocol::isProtocolAddress(prime_miner_address) ||
            !primechain::protocol::isProtocolAddress(composite_miner_address)) {
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
            if (!node.initializeGenesis(validator_set_, error)) {
                writeAll(fd, "ERROR " + error + "\n");
                return;
            }
            appended_records.push_back(
                primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0(validator_set_)));
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
                    included_transactions = mempoolSnapshot();
                    record.transactions = included_transactions;
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
                included_transactions = mempoolSnapshot();
                record.transactions = included_transactions;
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
        revalidateMempool();
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
        if (!in || !primechain::protocol::isProtocolAddress(address)) {
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

    void sendNonce(int fd, const std::string& line) {
        std::istringstream in(line);
        std::string command;
        std::string address;
        std::string extra;
        in >> command >> address;
        if (!in || command != "GET_NONCE" || !primechain::protocol::isProtocolAddress(address) ||
            (in >> extra)) {
            writeAll(fd, "ERROR invalid GET_NONCE\n");
            return;
        }

        const auto snapshot = mempoolSnapshot();
        std::string error;
        primechain::node::SequentialNode node(store_path_);
        if (!node.load(error)) {
            writeAll(fd, "ERROR " + error + "\n");
            return;
        }
        const std::uint64_t confirmed = node.accountNonce(address);
        std::uint64_t next = confirmed + 1;
        for (const auto& tx : snapshot) {
            if (tx.sender_address == address && tx.nonce == next) {
                ++next;
            }
        }
        writeAll(fd, "NONCE " + address + " " + std::to_string(confirmed) + " " +
            std::to_string(next) + "\n");
    }

    std::string store_path_;
    std::string bind_address_;
    int listen_port_{0};
    std::vector<PeerEndpoint> peers_;
    std::map<std::string, PeerRuntimeState> peer_state_;
    bool advance_enabled_{false};
    bool ack_mempool_enabled_{false};
    bool factorization_helper_enabled_{false};
    int finalization_timeout_ms_{0};
    int composite_lottery_window_ms_{0};
    std::uint32_t composite_lottery_win_bps_{kDefaultCompositeLotteryWinBps};
    primechain::storage::RecordStore store_;
    primechain::storage::CommitmentStore commitment_store_;
    primechain::storage::PhaseStore phase_store_;
    primechain::storage::ValidatorEpochStore epoch_store_;
    primechain::storage::FinalizationStore finalization_store_;
    primechain::storage::RoundChangeStore round_change_store_;
    std::vector<primechain::Address> genesis_validator_set_;
    std::vector<primechain::Address> validator_set_;
    std::optional<primechain::wallet::MinerIdentity> validator_identity_;
    bool use_chain_endpoints_{false};
    bool allow_remote_admin_{false};
    mutable std::mutex mempool_mutex_;
    std::vector<primechain::protocol::TransactionV0> mempool_;
    std::map<std::string, std::uint64_t> mempool_first_seen_;
    std::map<
        std::tuple<primechain::PrimeValue, std::uint64_t, std::string>,
        primechain::storage::StoredCommitment> commitments_;
    std::map<
        std::tuple<primechain::PrimeValue, std::uint64_t, std::string>,
        primechain::storage::CommitPhaseVote> phase_votes_;
    std::map<
        std::tuple<primechain::PrimeValue, std::uint64_t, std::string>,
        SignedCompositeReveal> pending_reveals_;
    std::map<primechain::Address, primechain::storage::ValidatorEpochVoteRecord> epoch_votes_;
    std::map<primechain::Address, primechain::protocol::ValidatorEndpointUpdateV1> pending_endpoint_updates_;
    std::map<primechain::Address, primechain::protocol::ValidatorApplicationV1> pending_validator_applications_;
    std::map<std::pair<primechain::Address, primechain::Address>, primechain::protocol::ValidatorWorkBindingV1> pending_validator_work_bindings_;
    std::map<primechain::Address, EconomicPolicyVoteRecord> policy_votes_;
    std::mutex finalization_mutex_;
    mutable std::mutex client_penalty_mutex_;
    mutable std::map<std::uint32_t, ClientPenaltyState> client_penalties_;
    std::mutex composite_lottery_mutex_;
    std::map<primechain::PrimeValue, CompositeLotteryRoundState> composite_lottery_;
    std::map<std::pair<primechain::PrimeValue, std::uint64_t>,
        primechain::storage::SignedCandidateRecord> signed_candidates_;
    std::map<std::tuple<primechain::PrimeValue, std::uint64_t, primechain::Address>,
        primechain::protocol::RoundChangeVoteV1> round_changes_;
    std::map<std::tuple<primechain::PrimeValue, std::uint64_t, std::uint64_t, primechain::Address>,
        CommitPhaseTimeoutVote> commit_phase_timeouts_;
};

struct Options {
    int port{kDefaultPort};
    std::string bind_address{"127.0.0.1"};
    std::string store_path{kDefaultStorePath};
    std::vector<PeerEndpoint> peers;
    int sync_interval_seconds{0};
    bool enable_advance{false};
    bool enable_ack_mempool{false};
    bool enable_factorization_helper{false};
    int finalization_timeout_ms{0};
    int composite_lottery_window_ms{0};
    std::uint32_t composite_lottery_win_bps{kDefaultCompositeLotteryWinBps};
    std::vector<primechain::Address> validator_set;
    std::string validator_identity_path;
    bool use_chain_endpoints{false};
    bool allow_remote_admin{false};
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
        if (flag == "--peer" || flag == "--bootstrap-peer") {
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
        if (flag == "--enable-factorization-helper") {
            options.enable_factorization_helper = true;
            continue;
        }
        if (flag == "--finalization-timeout-ms") {
            if (index >= argc) return std::nullopt;
            options.finalization_timeout_ms = std::stoi(argv[index++]);
            if (options.finalization_timeout_ms < 0) return std::nullopt;
            continue;
        }
        if (flag == "--composite-lottery-window-ms") {
            if (index >= argc) return std::nullopt;
            options.composite_lottery_window_ms = std::stoi(argv[index++]);
            if (options.composite_lottery_window_ms < 0) return std::nullopt;
            continue;
        }
        if (flag == "--composite-lottery-win-bps") {
            if (index >= argc) return std::nullopt;
            const auto value = std::stoul(argv[index++]);
            if (value > 10000) return std::nullopt;
            options.composite_lottery_win_bps = static_cast<std::uint32_t>(value);
            continue;
        }
        if (flag == "--validator-set" || flag == "--genesis-validator-set") {
            options.validator_set.clear();
            while (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
                options.validator_set.push_back(argv[index++]);
            }
            if (options.validator_set.empty()) return std::nullopt;
            continue;
        }
        if (flag == "--validator-identity") {
            if (index >= argc) return std::nullopt;
            options.validator_identity_path = argv[index++];
            continue;
        }
        if (flag == "--use-chain-endpoints") {
            options.use_chain_endpoints = true;
            continue;
        }
        if (flag == "--allow-remote-admin") {
            options.allow_remote_admin = true;
            continue;
        }
        {
            return std::nullopt;
        }
    }
    return options;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [port] [record_store_path] [--bind address] [--peer host port] [--bootstrap-peer host port] [--sync-interval seconds] [--enable-advance] [--enable-ack-mempool] [--enable-factorization-helper] [--finalization-timeout-ms ms] [--composite-lottery-window-ms ms] [--composite-lottery-win-bps bps] [--validator-set addr1 addr2 addr3 | --genesis-validator-set addr1 addr2 addr3] [--validator-identity file] [--use-chain-endpoints] [--allow-remote-admin]\n"
              << "       " << argv0 << " --version\n"
              << "example:\n"
              << "  " << argv0 << " 18889 ./data/sequential-500.dat\n"
              << "  " << argv0 << " 18890 ./data/node-b.dat --peer 127.0.0.1 18889 --sync-interval 5\n"
              << "  " << argv0 << " 18889 ./data/public-node.dat --bind 0.0.0.0\n"
              << "  " << argv0 << " 18889 ./data/dev-node.dat --enable-advance --enable-ack-mempool --enable-factorization-helper\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::cout << versionLine() << "\n";
        return 0;
    }

    const auto parsed = parseOptions(argc, argv);
    if (!parsed.has_value()) {
        printUsage(argv[0]);
        return 1;
    }
    Options options = *parsed;

    std::optional<primechain::wallet::MinerIdentity> validator_identity;
    if (options.validator_set.empty() && !options.validator_identity_path.empty()) {
        std::cerr << "validator identity requires --validator-set or --genesis-validator-set\n";
        return 1;
    }
    if (options.validator_set.empty() && options.use_chain_endpoints) {
        std::cerr << "--use-chain-endpoints requires --validator-set or --genesis-validator-set\n";
        return 1;
    }
    if (!options.validator_set.empty()) {
        const std::set<primechain::Address> unique_validators(
            options.validator_set.begin(), options.validator_set.end());
        if (unique_validators.size() != options.validator_set.size() ||
            !primechain::core::validValidatorSetSize(options.validator_set.size()) ||
            !std::all_of(options.validator_set.begin(), options.validator_set.end(),
                [](const primechain::Address& address) {
                    return primechain::crypto::isProtocolSignatureAddress(address);
                })) {
            std::cerr << "validator set must contain distinct pcpq1_ addresses\n";
            return 1;
        }
        primechain::wallet::MinerIdentity loaded;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(
                options.validator_identity_path, loaded, error)) {
            std::cerr << "validator identity load failed: " << error << "\n";
            return 1;
        }
        std::sort(options.validator_set.begin(), options.validator_set.end());
        validator_identity = std::move(loaded);
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    auto server = listenOnPort(options.bind_address, options.port);
    if (!server.has_value()) {
        return 1;
    }

    SyncServer sync_server(
        options.store_path,
        options.bind_address,
        options.port,
        options.peers,
        options.enable_advance,
        options.enable_ack_mempool,
        options.enable_factorization_helper,
        options.finalization_timeout_ms,
        options.composite_lottery_window_ms,
        options.composite_lottery_win_bps,
        options.validator_set,
        validator_identity,
        options.use_chain_endpoints,
        options.allow_remote_admin);
    if (!options.peers.empty()) {
        std::string error;
        if (sync_server.peerDiscoveryEnabled()) {
            sync_server.discoverPeersFromKnown();
        }
        if (!sync_server.syncFromKnownPeers(error)) {
            std::cerr << "peer sync failed: " << error << "\n";
            return 1;
        }
        std::cout << "peer sync complete from " << options.peers.size() << " configured peer(s)\n";
    }
    {
        std::string error;
        if (!sync_server.ensureValidatorAnchor(error)) {
            std::cerr << "validator genesis anchor failed: " << error << "\n";
            return 1;
        }

        auto load_recoverable_sidecar = [&](const std::string& name,
                                            const std::vector<std::string>& paths,
                                            auto&& loader) {
            error.clear();
            if (loader(error)) return true;

            std::cerr << name << " load warning: " << error
                      << "; discarding volatile consensus cache and retrying\n";
            for (const auto& path : paths) {
                std::string cleanup_error;
                if (!removeIfPresent(path, cleanup_error) ||
                    !removeIfPresent(path + ".tmp", cleanup_error)) {
                    std::cerr << name << " cache cleanup failed: " << cleanup_error << "\n";
                    return false;
                }
            }

            error.clear();
            if (loader(error)) return true;
            std::cerr << name << " load failed after cache reset: " << error << "\n";
            return false;
        };

        if (!load_recoverable_sidecar(
                "commitment store",
                std::vector<std::string>{options.store_path + ".commitments"},
                [&](std::string& load_error) { return sync_server.loadCommitments(load_error); })) {
            return 1;
        }
        if (!load_recoverable_sidecar(
                "phase store",
                std::vector<std::string>{options.store_path + ".phases"},
                [&](std::string& load_error) { return sync_server.loadPhaseVotes(load_error); })) {
            return 1;
        }
        if (!load_recoverable_sidecar(
                "validator epoch store",
                std::vector<std::string>{options.store_path + ".epochs"},
                [&](std::string& load_error) { return sync_server.loadEpochVotes(load_error); })) {
            return 1;
        }
        if (!load_recoverable_sidecar(
                "finalization store",
                std::vector<std::string>{options.store_path + ".finalization", options.store_path + ".rounds"},
                [&](std::string& load_error) { return sync_server.loadFinalizationVotes(load_error); })) {
            return 1;
        }
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
    if (options.enable_factorization_helper) {
        std::cout << "development helper enabled: GET_FACTORIZATION\n";
    }
    if (options.allow_remote_admin) {
        std::cout << "remote admin commands enabled\n";
    } else {
        std::cout << "admin commands restricted to loopback clients\n";
    }
    if (options.finalization_timeout_ms > 0) {
        std::cout << "finalization timeout: " << options.finalization_timeout_ms << " ms\n";
    }
    if (options.composite_lottery_window_ms > 0) {
        std::cout << "composite lottery: window_ms=" << options.composite_lottery_window_ms
                  << " win_bps=" << options.composite_lottery_win_bps << "\n";
    }
    if (validator_identity.has_value()) {
        const bool active = std::binary_search(
            sync_server.activeValidatorSet().begin(),
            sync_server.activeValidatorSet().end(),
            validator_identity->address);
        std::cout << "validator identity configured: " << validator_identity->address
                  << (active ? " active" : " inactive") << "\n";
    }
    if (!options.validator_set.empty()) {
        std::cout << "validator quorum active: "
                  << primechain::core::requiredValidatorQuorum(sync_server.activeValidatorSet().size())
                  << "-of-" << sync_server.activeValidatorSet().size()
                  << "; genesis validators=" << options.validator_set.size() << "\n";
    }
    if (options.use_chain_endpoints) {
        std::cout << "chain endpoint peer discovery enabled\n";
    }

    auto next_sync = std::chrono::steady_clock::now()
        + std::chrono::seconds(options.sync_interval_seconds > 0 ? options.sync_interval_seconds : 1);
    auto next_mempool_rebroadcast = std::chrono::steady_clock::now()
        + std::chrono::seconds(kMempoolRebroadcastIntervalSeconds);
    auto runPeriodicSync = [&]() {
        const auto now = std::chrono::steady_clock::now();
        if (options.sync_interval_seconds > 0 && sync_server.hasKnownPeers() && now >= next_sync) {
            std::string error;
            if (sync_server.peerDiscoveryEnabled()) {
                sync_server.discoverPeersFromKnown();
            }
            if (!sync_server.syncFromKnownPeers(error) && !error.empty()) {
                std::cerr << "periodic peer sync warning: " << error << "\n";
            }
            next_sync = now + std::chrono::seconds(options.sync_interval_seconds);
        }
        if (sync_server.hasKnownPeers() && now >= next_mempool_rebroadcast) {
            if (sync_server.hasPendingMempool()) {
                sync_server.rebroadcastMempool();
            }
            next_mempool_rebroadcast = now + std::chrono::seconds(kMempoolRebroadcastIntervalSeconds);
        }
    };

    std::thread periodic_sync_thread([&]() {
        while (g_running) {
            runPeriodicSync();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

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

        const bool client_loopback = isLoopbackClient(client_addr);
        const std::uint32_t client_ip = clientIpKey(client_addr);
        if (sync_server.clientBanned(client_ip, client_loopback)) {
            writeAll(client_fd, "ERROR client temporarily banned for repeated invalid commands\n");
            close(client_fd);
            continue;
        }
        if (!client_loopback) {
            std::lock_guard<std::mutex> lock(g_client_connection_mutex);
            auto& active = g_active_remote_connections[client_ip];
            if (g_active_remote_connection_total >= kMaxActiveRemoteConnectionsTotal) {
                writeAll(client_fd, "ERROR connection limit exceeded\n");
                close(client_fd);
                continue;
            }
            if (active >= kMaxActiveRemoteConnectionsPerIp) {
                writeAll(client_fd, "ERROR connection limit exceeded for client IP\n");
                close(client_fd);
                continue;
            }
            ++active;
            ++g_active_remote_connection_total;
        }
        Socket client(client_fd);
        std::thread([&sync_server, client = std::move(client), client_ip, client_loopback]() mutable {
            setSocketTimeouts(client.fd(), kPeerReadTimeoutMs);
            sync_server.handleClient(client.fd(), client_ip, client_loopback);
            if (!client_loopback) {
                std::lock_guard<std::mutex> lock(g_client_connection_mutex);
                auto found = g_active_remote_connections.find(client_ip);
                if (found != g_active_remote_connections.end()) {
                    if (found->second > 1) {
                        --found->second;
                    } else {
                        g_active_remote_connections.erase(found);
                    }
                }
                if (g_active_remote_connection_total > 0) {
                    --g_active_remote_connection_total;
                }
            }
        }).detach();
    }

    g_running = 0;
    if (periodic_sync_thread.joinable()) periodic_sync_thread.join();

    std::cout << "sync server stopped\n";
    return 0;
}
