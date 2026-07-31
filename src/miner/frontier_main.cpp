#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "primechain/core/consensus.hpp"
#include "primechain/core/consensus.hpp"
#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/wallet/miner_identity.hpp"
#include "primechain/math/number_theory.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/storage/record_store.hpp"
#include "primechain/types.hpp"

namespace {

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 18889;
constexpr primechain::PrimeValue kDefaultLimit = 20;
constexpr std::size_t kMaxStatusProbeValidators = 5;
constexpr const char* kDefaultPrimeMiner = "pcdev1_prime_miner";
constexpr const char* kDefaultCompositeMiner = "pcdev1_composite_miner";

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

    int fd() const { return fd_; }

private:
    int fd_{-1};
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

primechain::CompositeProof toCompositeProof(const primechain::protocol::CompositeProofV0& proof) {
    primechain::CompositeProof out;
    out.m = proof.g;
    out.d = proof.d;
    out.e = proof.e;
    out.provider_address = proof.provider_address;
    out.signature = proof.signature;
    return out;
}

bool loadProofStore(const std::string& path, MapProofIndex& proofs, std::string& error) {
    primechain::storage::RecordStore store(path);
    const auto records = store.loadAll(error);
    if (!error.empty()) return false;
    for (const auto& stored : records) {
        if (stored.kind != primechain::storage::StoredRecordKind::Composite) continue;
        const auto decoded = primechain::protocol::deserializeCompositeRecord(stored.payload, error);
        if (!decoded.has_value()) return false;
        const auto proof = toCompositeProof(decoded->proof);
        if (!primechain::math::verifyCompositeProof(proof)) {
            error = "proof store contains invalid composite proof";
            return false;
        }
        proofs.add(proof);
    }
    return true;
}

primechain::PrimeValue loadProofStoreFrontier(const std::string& path) {
    primechain::node::SequentialNode node(path);
    std::string error;
    if (!node.load(error)) return 0;
    const auto& status = node.status();
    return status.has_genesis ? status.frontier_integer : 0;
}

struct Status {
    std::uint64_t record_count{0};
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    bool has_genesis{false};
    std::uint64_t height{0};
    primechain::PrimeValue frontier{0};
    std::string latest_hash;
};

struct PeerEndpoint {
    std::string host;
    int port{0};
};

struct CommitPhaseStatus {
    PeerEndpoint peer;
    std::string state;
    std::size_t votes{0};
    std::string snapshot_hash;
    std::string winner;
};

enum class CommitPhaseState {
    Unknown,
    Open,
    Closing,
    Closed,
};

struct MiningView {
    PeerEndpoint peer;
    primechain::PrimeValue frontier{0};
    primechain::PrimeValue target{0};
    bool has_genesis{false};
    std::string latest_hash;
    std::string phase_state;
    std::size_t phase_votes{0};
    std::string snapshot_hash;
    std::string winner;
    std::size_t commitment_count{0};
    std::uint64_t commit_round{0};
    std::size_t validator_count{0};
    std::size_t peer_count{0};
};

struct PendingComposite {
    primechain::PrimeValue integer{0};
    primechain::PrimeValue d{0};
    primechain::PrimeValue e{0};
    std::uint64_t nonce{0};
    std::string provider;
};

struct PeerStatus {
    PeerEndpoint peer;
    Status status;
};

std::optional<PendingComposite> loadPendingComposite(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    PendingComposite pending;
    std::string key;
    while (in >> key) {
        if (key == "integer") in >> pending.integer;
        else if (key == "d") in >> pending.d;
        else if (key == "e") in >> pending.e;
        else if (key == "nonce") in >> pending.nonce;
        else if (key == "provider") in >> pending.provider;
        else {
            std::string ignored;
            in >> ignored;
        }
    }
    if (!in.eof() || pending.integer < 2 || pending.d < 2 || pending.e < 2 ||
        pending.nonce == 0 || pending.provider.empty()) {
        return std::nullopt;
    }
    return pending;
}

bool writePendingComposite(const std::string& path, const PendingComposite& pending) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::cerr << "could not write pending composite state " << path << "\n";
        return false;
    }
    out << "integer " << pending.integer << "\n"
        << "d " << pending.d << "\n"
        << "e " << pending.e << "\n"
        << "nonce " << pending.nonce << "\n"
        << "provider " << pending.provider << "\n";
    return static_cast<bool>(out);
}

void clearPendingComposite(const std::optional<std::string>& path) {
    if (path.has_value()) unlink(path->c_str());
}

std::string factorizationString(const primechain::math::Factorization& factorization) {
    std::ostringstream out;
    if (factorization.factors.empty()) {
        out << "none";
        return out.str();
    }
    for (std::size_t i = 0; i < factorization.factors.size(); ++i) {
        if (i != 0) out << ",";
        const auto& factor = factorization.factors[i];
        out << factor.prime << "^" << factor.exponent;
    }
    return out.str();
}

std::string prattProofSummary(const primechain::math::PrattProof& proof) {
    std::ostringstream out;
    out << "PROOF_PRATT prime=" << proof.p
        << " witness=" << proof.witness
        << " factors_p_minus_1=" << factorizationString(proof.factors_of_p_minus_1);
    return out.str();
}

std::string compositeProofSummary(const primechain::CompositeProof& proof) {
    std::ostringstream out;
    out << "PROOF_COMPOSITE integer=" << proof.m
        << " divisor=" << proof.d
        << " cofactor=" << proof.e;
    return out.str();
}

std::optional<Socket> connectToNode(const std::string& host, int port) {
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

bool writeCommand(int fd, std::string command) {
    if (!command.empty() && command.back() == '\n') command.pop_back();
    if (command.size() <= 4096) return writeAll(fd, command + "\n");
    return writeAll(fd, "FRAME " + std::to_string(command.size()) + "\n") &&
        writeAll(fd, command);
}

std::optional<std::string> readRawLine(int fd) {
    std::string line;
    char ch = '\0';
    while (true) {
        const ssize_t received = recv(fd, &ch, 1, 0);
        if (received <= 0) {
            return std::nullopt;
        }
        if (ch == '\n') {
            return line;
        }
        line.push_back(ch);
    }
}

std::optional<std::string> readLine(int fd) {
    auto line = readRawLine(fd);
    if (!line.has_value() || line->rfind("FRAME ", 0) != 0) return line;

    std::istringstream in(*line);
    std::string tag, extra;
    std::size_t size = 0;
    in >> tag >> size;
    if (!in || tag != "FRAME" || size == 0 || size > 1024 * 1024 || (in >> extra)) {
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

std::optional<std::string> requestLine(const std::string& host, int port, const std::string& request) {
    auto socket = connectToNode(host, port);
    if (!socket.has_value()) {
        return std::nullopt;
    }
    if (!writeCommand(socket->fd(), request)) {
        return std::nullopt;
    }
    shutdown(socket->fd(), SHUT_WR);
    return readLine(socket->fd());
}

std::vector<std::string> requestLines(const std::string& host, int port, const std::string& request) {
    std::vector<std::string> lines;
    auto socket = connectToNode(host, port);
    if (!socket.has_value()) {
        return lines;
    }
    if (!writeCommand(socket->fd(), request)) {
        return lines;
    }
    shutdown(socket->fd(), SHUT_WR);
    while (true) {
        auto line = readLine(socket->fd());
        if (!line.has_value()) break;
        lines.push_back(*line);
    }
    return lines;
}

void addUniquePeer(std::vector<PeerEndpoint>& peers, PeerEndpoint peer) {
    for (const auto& existing : peers) {
        if (existing.host == peer.host && existing.port == peer.port) {
            return;
        }
    }
    peers.push_back(std::move(peer));
}

std::vector<PeerEndpoint> requestPeerList(const std::string& host, int port) {
    std::vector<PeerEndpoint> peers;
    const auto lines = requestLines(host, port, "GET_PEERS\n");
    if (lines.empty()) {
        return peers;
    }
    std::istringstream header(lines.front());
    std::string tag;
    std::size_t expected = 0;
    header >> tag >> expected;
    if (!header || tag != "PEERS") {
        return peers;
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i] == "END_PEERS") break;
        std::istringstream in(lines[i]);
        PeerEndpoint peer;
        in >> tag >> peer.host >> peer.port;
        if (in && tag == "PEER" && peer.port > 0) {
            addUniquePeer(peers, std::move(peer));
        }
    }
    if (peers.size() > expected) {
        peers.resize(expected);
    }
    return peers;
}

std::vector<PeerEndpoint> requestValidatorEndpointList(const std::string& host, int port) {
    std::vector<PeerEndpoint> peers;
    const auto lines = requestLines(host, port, "GET_VALIDATOR_ENDPOINTS\n");
    if (lines.empty()) {
        return peers;
    }
    std::istringstream header(lines.front());
    std::string tag;
    std::size_t expected = 0;
    header >> tag >> expected;
    if (!header || tag != "VALIDATOR_ENDPOINTS") {
        return peers;
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i] == "END_VALIDATOR_ENDPOINTS") break;
        std::istringstream in(lines[i]);
        std::string validator_address;
        PeerEndpoint peer;
        primechain::PrimeValue effective_integer = 0;
        std::uint64_t sequence = 0;
        in >> tag >> validator_address >> peer.host >> peer.port >> effective_integer >> sequence;
        if (in && tag == "VALIDATOR_ENDPOINT" && peer.port > 0) {
            addUniquePeer(peers, std::move(peer));
        }
    }
    if (peers.size() > expected) {
        peers.resize(expected);
    }
    return peers;
}

std::size_t phaseVoteCount(const std::string& response) {
    const auto marker = response.find("votes=");
    if (marker == std::string::npos) return 0;
    try {
        return static_cast<std::size_t>(std::stoull(response.substr(marker + 6)));
    } catch (...) {
        return 0;
    }
}

std::size_t remoteValidatorCount(const std::string& host, int port) {
    const auto response = requestLine(host, port, "GET_VALIDATORS\n");
    if (!response.has_value()) return 0;
    std::istringstream in(*response);
    std::string tag;
    std::size_t count = 0;
    in >> tag >> count;
    if (!in || tag != "VALIDATORS") return 0;
    return count;
}

std::vector<PeerEndpoint> quorumEndpoints(
    const std::string& host,
    int port,
    const std::vector<PeerEndpoint>& configured_validator_endpoints) {
    std::vector<PeerEndpoint> peers;
    addUniquePeer(peers, {host, port});
    for (const auto& peer : configured_validator_endpoints) {
        addUniquePeer(peers, peer);
    }
    for (const auto& peer : requestValidatorEndpointList(host, port)) {
        addUniquePeer(peers, peer);
    }
    for (const auto& peer : requestPeerList(host, port)) {
        addUniquePeer(peers, peer);
    }
    return peers;
}

CommitPhaseState parseCommitPhaseState(const std::string& state) {
    if (state == "CLOSED") return CommitPhaseState::Closed;
    if (state == "CLOSING") return CommitPhaseState::Closing;
    if (state == "OPEN") return CommitPhaseState::Open;
    return CommitPhaseState::Unknown;
}

int phaseRank(CommitPhaseState state) {
    if (state == CommitPhaseState::Closed) return 3;
    if (state == CommitPhaseState::Closing) return 2;
    if (state == CommitPhaseState::Open) return 1;
    return 0;
}

int phaseRank(const std::string& state) {
    return phaseRank(parseCommitPhaseState(state));
}

std::optional<MiningView> requestMiningView(
    const PeerEndpoint& peer,
    primechain::PrimeValue integer) {
    std::ostringstream command;
    command << "GET_MINING_VIEW " << integer << "\n";
    const auto response = requestLine(peer.host, peer.port, command.str());
    if (!response.has_value()) return std::nullopt;

    std::istringstream in(*response);
    std::string tag;
    std::uint64_t has_genesis = 0;
    MiningView view;
    view.peer = peer;
    in >> tag
       >> view.frontier
       >> view.target
       >> has_genesis
       >> view.latest_hash
       >> view.phase_state
       >> view.phase_votes
       >> view.snapshot_hash
       >> view.winner
       >> view.commitment_count
       >> view.commit_round
       >> view.validator_count
       >> view.peer_count;
    if (!in || tag != "MINING_VIEW" || view.target != integer ||
        phaseRank(view.phase_state) == 0) {
        return std::nullopt;
    }
    view.has_genesis = has_genesis != 0;
    return view;
}

CommitPhaseState viewPhaseState(const MiningView& view) {
    return parseCommitPhaseState(view.phase_state);
}

std::vector<MiningView> requestMiningViews(
    const std::vector<PeerEndpoint>& peers,
    primechain::PrimeValue integer) {
    std::vector<MiningView> views;
    for (const auto& peer : peers) {
        const auto view = requestMiningView(peer, integer);
        if (view.has_value()) views.push_back(*view);
    }
    return views;
}

std::optional<MiningView> strongestMiningView(const std::vector<MiningView>& views) {
    std::optional<MiningView> best;
    for (const auto& view : views) {
        if (!best.has_value() ||
            phaseRank(view.phase_state) > phaseRank(best->phase_state) ||
            (phaseRank(view.phase_state) == phaseRank(best->phase_state) &&
             view.phase_votes > best->phase_votes)) {
            best = view;
        }
    }
    return best;
}

std::optional<CommitPhaseStatus> requestCommitPhase(
    const PeerEndpoint& peer,
    primechain::PrimeValue integer) {
    std::ostringstream command;
    command << "GET_COMMIT_PHASE " << integer << "\n";
    const auto response = requestLine(peer.host, peer.port, command.str());
    if (!response.has_value()) return std::nullopt;

    std::istringstream in(*response);
    std::string tag;
    primechain::PrimeValue response_integer = 0;
    CommitPhaseStatus status;
    status.peer = peer;
    in >> tag >> response_integer >> status.state >> status.votes
       >> status.snapshot_hash >> status.winner;
    if (!in || tag != "COMMIT_PHASE" || response_integer != integer) {
        return std::nullopt;
    }
    return status;
}

std::optional<CommitPhaseStatus> closedCommitPhase(
    const std::vector<PeerEndpoint>& peers,
    primechain::PrimeValue integer,
    std::size_t required_votes) {
    for (const auto& peer : peers) {
        const auto status = requestCommitPhase(peer, integer);
        if (status.has_value() && status->state == "CLOSED" && status->votes >= required_votes) {
            return status;
        }
    }
    return std::nullopt;
}

std::string withPeerPrefix(const PeerEndpoint& peer, const std::string& line) {
    return "VALIDATOR " + peer.host + ":" + std::to_string(peer.port) + " " + line;
}

std::optional<PeerEndpoint> closeCommitPhaseQuorum(
    const std::vector<PeerEndpoint>& peers,
    primechain::PrimeValue integer,
    std::size_t required_votes) {
    std::optional<PeerEndpoint> quorum_peer;
    for (const auto& peer : peers) {
        std::ostringstream command;
        command << "CLOSE_COMMIT_PHASE " << integer << "\n";
        const auto response = requestLine(peer.host, peer.port, command.str());
        if (!response.has_value()) {
            std::cerr << "commit phase close warning from " << peer.host << ":"
                      << peer.port << ": no response\n";
            continue;
        }
        std::cout << withPeerPrefix(peer, *response) << "\n";
        if (phaseVoteCount(*response) >= required_votes) {
            quorum_peer = peer;
        }
    }
    return quorum_peer;
}

std::optional<Status> getStatus(const std::string& host, int port) {
    const auto response = requestLine(host, port, "GET_STATUS\n");
    if (!response.has_value()) {
        return std::nullopt;
    }

    std::istringstream in(*response);
    std::string tag;
    std::uint64_t has_genesis = 0;
    Status status;
    in >> tag
       >> status.record_count
       >> status.prime_records
       >> status.composite_records
       >> has_genesis
       >> status.height
       >> status.frontier
       >> status.latest_hash;
    if (!in || tag != "STATUS") {
        std::cerr << "unexpected status response: " << *response << "\n";
        return std::nullopt;
    }
    status.has_genesis = has_genesis != 0;
    return status;
}

std::vector<PeerEndpoint> sampledStatusProbePeers(
    const std::string& host,
    int port,
    const std::vector<PeerEndpoint>& configured_validator_endpoints) {
    auto peers = quorumEndpoints(host, port, configured_validator_endpoints);
    if (peers.size() <= kMaxStatusProbeValidators) return peers;

    std::vector<PeerEndpoint> sample;
    addUniquePeer(sample, {host, port});

    std::vector<PeerEndpoint> candidates;
    for (const auto& peer : peers) {
        if (peer.host == host && peer.port == port) continue;
        candidates.push_back(peer);
    }

    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::shuffle(candidates.begin(), candidates.end(), rng);
    for (const auto& peer : candidates) {
        if (sample.size() >= kMaxStatusProbeValidators) break;
        addUniquePeer(sample, peer);
    }
    return sample;
}

std::optional<PeerStatus> freshestPeerStatus(
    const std::string& host,
    int port,
    const std::vector<PeerEndpoint>& configured_validator_endpoints) {
    std::optional<PeerStatus> best;
    for (const auto& peer : sampledStatusProbePeers(host, port, configured_validator_endpoints)) {
        const auto status = getStatus(peer.host, peer.port);
        if (!status.has_value()) continue;
        if (!best.has_value() || status->frontier > best->status.frontier) {
            best = PeerStatus{peer, *status};
        }
    }
    return best;
}

void appendUint64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void appendString(std::vector<std::uint8_t>& out, const std::string& value) {
    appendUint64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

std::uint64_t stableCompositeNonce(
    const primechain::CompositeProof& proof,
    const std::string& provider) {
    std::vector<std::uint8_t> bytes;
    appendString(bytes, "primechain-frontier-miner-composite-nonce-v1");
    appendUint64(bytes, proof.m);
    appendUint64(bytes, proof.d);
    appendUint64(bytes, proof.e);
    appendString(bytes, provider);
    const auto hash = primechain::crypto::sha3_256(bytes);
    std::uint64_t nonce = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        nonce |= static_cast<std::uint64_t>(hash[i]) << (i * 8);
    }
    return nonce == 0 ? 1 : nonce;
}

std::optional<primechain::Hash256> hashFromHex(const std::string& hex) {
    if (hex.size() != 64) return std::nullopt;
    primechain::Hash256 hash{};
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    };
    for (std::size_t i = 0; i < hash.size(); ++i) {
        const int high = digit(hex[i * 2]);
        const int low = digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        hash[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return hash;
}

std::string primeSubmission(
    const primechain::math::PrattProof& proof,
    const std::string& provider) {
    std::ostringstream out;
    out << "SUBMIT_PRIME "
        << proof.p << " "
        << proof.witness << " "
        << proof.factors_of_p_minus_1.factors.size();
    for (const auto& factor : proof.factors_of_p_minus_1.factors) {
        out << " " << factor.prime << " " << factor.exponent;
    }
    out << " " << provider << "\n";
    return out.str();
}

std::optional<std::string> signedPrimeSubmission(
    const primechain::math::PrattProof& proof,
    const primechain::Hash256& previous_record_hash,
    const primechain::wallet::MinerIdentity& identity,
    std::string& error) {
    std::vector<std::pair<primechain::PrimeValue, std::uint64_t>> factors;
    for (const auto& factor : proof.factors_of_p_minus_1.factors) {
        factors.push_back({factor.prime, factor.exponent});
    }
    const auto signature = primechain::crypto::signProtocolMessage(
        identity.private_key,
        primechain::crypto::primeProofSigningPayload(
            previous_record_hash, proof.p, proof.witness, factors, identity.address),
        error);
    if (!signature.has_value()) return std::nullopt;
    std::ostringstream out;
    out << "SUBMIT_SIGNED_PRIME " << proof.p << " " << proof.witness << " "
        << proof.factors_of_p_minus_1.factors.size();
    for (const auto& factor : proof.factors_of_p_minus_1.factors) {
        out << " " << factor.prime << " " << factor.exponent;
    }
    out << " " << identity.address << " "
        << primechain::wallet::bytesToHex(identity.public_key) << " "
        << primechain::wallet::bytesToHex(*signature) << "\n";
    return out.str();
}

std::string compositeCommitSubmission(
    const primechain::CompositeProof& proof,
    std::uint64_t nonce,
    const std::string& provider) {
    const auto commitment = primechain::crypto::compositeCommitment(
        proof.m, proof.d, proof.e, nonce, provider);
    std::ostringstream out;
    out << "SUBMIT_COMMIT "
        << proof.m << " "
        << primechain::crypto::toHex(commitment) << " "
        << provider << "\n";
    return out.str();
}

std::string compositeRevealSubmission(
    const primechain::CompositeProof& proof,
    std::uint64_t nonce,
    const std::string& provider) {
    std::ostringstream out;
    out << "SUBMIT_COMPOSITE_REVEAL "
        << proof.m << " "
        << proof.d << " "
        << proof.e << " "
        << nonce << " "
        << provider << "\n";
    return out.str();
}

std::optional<std::string> signedCompositeCommitSubmission(
    const primechain::CompositeProof& proof,
    std::uint64_t nonce,
    const primechain::wallet::MinerIdentity& identity,
    std::string& error) {
    const auto commitment = primechain::crypto::compositeCommitment(
        proof.m, proof.d, proof.e, nonce, identity.address);
    const auto signature = primechain::crypto::signProtocolMessage(
        identity.private_key,
        primechain::crypto::compositeCommitSigningPayload(
            proof.m, commitment, identity.address),
        error);
    if (!signature.has_value()) return std::nullopt;
    std::ostringstream out;
    out << "SUBMIT_SIGNED_COMMIT " << proof.m << " "
        << primechain::crypto::toHex(commitment) << " " << identity.address << " "
        << primechain::wallet::bytesToHex(identity.public_key) << " "
        << primechain::wallet::bytesToHex(*signature) << "\n";
    return out.str();
}

std::optional<std::string> signedCompositeRevealSubmission(
    const primechain::CompositeProof& proof,
    std::uint64_t nonce,
    const primechain::wallet::MinerIdentity& identity,
    std::string& error) {
    const auto signature = primechain::crypto::signProtocolMessage(
        identity.private_key,
        primechain::crypto::compositeRevealSigningPayload(
            proof.m, proof.d, proof.e, nonce, identity.address),
        error);
    if (!signature.has_value()) return std::nullopt;
    std::ostringstream out;
    out << "SUBMIT_SIGNED_REVEAL " << proof.m << " " << proof.d << " " << proof.e
        << " " << nonce << " " << identity.address << " "
        << primechain::wallet::bytesToHex(identity.public_key) << " "
        << primechain::wallet::bytesToHex(*signature) << "\n";
    return out.str();
}

bool accepted(const std::string& response) {
    return response.rfind("PRIME_ACCEPTED ", 0) == 0 ||
           response.rfind("COMPOSITE_ACCEPTED ", 0) == 0 ||
           response.rfind("RECORD_DUPLICATE ", 0) == 0 ||
           response.rfind("RECORD_REPLACED ", 0) == 0;
}

bool staleOrTransient(const std::string& response) {
    return response.find("must target next integer") != std::string::npos ||
           response.find("must extend frontier") != std::string::npos ||
           response.find("wrong frontier") != std::string::npos ||
           response.find("current frontier record not found") != std::string::npos ||
           response.find("commit phase is closing or closed") != std::string::npos ||
           response.find("commit phase is not closed by validator quorum") != std::string::npos ||
           response.find("could not close commit phase with validator quorum") != std::string::npos ||
           response.find("could not finalize composite record") != std::string::npos ||
           response.find("could not collect validator-quorum round-change signatures") != std::string::npos ||
           response.find("provider is in winner cooldown") != std::string::npos ||
           response.find("no prior commitment for reveal") != std::string::npos ||
           response.find("commitment not selected for reveal") != std::string::npos ||
           response.find("provider already committed a different hash") != std::string::npos ||
           response.find("provider already revealed different composite evidence") != std::string::npos ||
           response.rfind("REVEAL_PENDING ", 0) == 0 ||
           response.rfind("RECORD_CONFLICT", 0) == 0;
}

bool resetsCompositeCommitState(const std::string& response) {
    return response.find("reveal does not match prior commitment") != std::string::npos ||
           response.find("commitment not selected for reveal") != std::string::npos;
}

bool commitAcceptedOrDuplicate(const std::string& response) {
    return response.rfind("COMMIT_ACCEPTED ", 0) == 0 ||
           response.rfind("COMMIT_DUPLICATE ", 0) == 0;
}

void warmQuorumCommitments(
    const std::vector<PeerEndpoint>& peers,
    const std::string& commit_request) {
    for (const auto& peer : peers) {
        const auto response = requestLine(peer.host, peer.port, commit_request);
        if (!response.has_value()) {
            std::cerr << "commit propagation warmup warning from " << peer.host << ":"
                      << peer.port << ": no response\n";
            continue;
        }
        if (!commitAcceptedOrDuplicate(*response) &&
            response->find("commit phase is closing or closed") == std::string::npos) {
            std::cerr << "commit propagation warmup warning from " << peer.host << ":"
                      << peer.port << ": " << *response << "\n";
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [host] [port] [limit] --prime-identity <file> --composite-identity <file> [--validator-endpoint host port...]\n"
              << "legacy development: " << argv0
              << " [host] [port] [limit] [prime_miner] [composite_miner]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const std::string host = argc > 1 ? argv[1] : kDefaultHost;
    const int port = argc > 2 ? std::stoi(argv[2]) : kDefaultPort;
    const primechain::PrimeValue limit = argc > 3 ? std::stoull(argv[3]) : kDefaultLimit;
    std::string prime_miner = kDefaultPrimeMiner;
    std::string composite_miner = kDefaultCompositeMiner;
    std::optional<primechain::wallet::MinerIdentity> prime_identity;
    std::optional<primechain::wallet::MinerIdentity> composite_identity;
    std::optional<std::string> proof_store_path;
    std::optional<std::string> pending_composite_path;
    std::vector<PeerEndpoint> configured_validator_endpoints;
    int argument = 4;
    while (argument < argc) {
        const std::string option = argv[argument++];
        if (option == "--proof-store") {
            if (argument >= argc) { printUsage(argv[0]); return 1; }
            proof_store_path = argv[argument++];
        } else if (option == "--pending-composite") {
            if (argument >= argc) { printUsage(argv[0]); return 1; }
            pending_composite_path = argv[argument++];
        } else if (option == "--validator-endpoint") {
            if (argument + 1 >= argc) { printUsage(argv[0]); return 1; }
            PeerEndpoint peer;
            peer.host = argv[argument++];
            peer.port = std::stoi(argv[argument++]);
            if (peer.port <= 0) { printUsage(argv[0]); return 1; }
            addUniquePeer(configured_validator_endpoints, std::move(peer));
        } else if (option == "--prime-identity" || option == "--composite-identity") {
            if (argument >= argc) { printUsage(argv[0]); return 1; }
            primechain::wallet::MinerIdentity identity;
            std::string error;
            if (!primechain::wallet::loadMinerIdentity(argv[argument++], identity, error)) {
                std::cerr << error << "\n";
                return 1;
            }
            if (option == "--prime-identity") {
                prime_miner = identity.address;
                prime_identity = std::move(identity);
            } else {
                composite_miner = identity.address;
                composite_identity = std::move(identity);
            }
        } else if (prime_miner == kDefaultPrimeMiner) {
            prime_miner = option;
        } else if (composite_miner == kDefaultCompositeMiner) {
            composite_miner = option;
        } else {
            printUsage(argv[0]);
            return 1;
        }
    }

    MapProofIndex proofs;
    std::optional<primechain::PrimeValue> proof_store_frontier;
    if (proof_store_path.has_value()) {
        std::string error;
        if (!loadProofStore(*proof_store_path, proofs, error)) {
            std::cerr << "could not load proof store: " << error << "\n";
            return 1;
        }
        proof_store_frontier = loadProofStoreFrontier(*proof_store_path);
    }
    std::size_t submitted = 0;
    std::map<primechain::PrimeValue, std::size_t> retry_counts;

    while (true) {
        const auto peer_status = freshestPeerStatus(host, port, configured_validator_endpoints);
        if (!peer_status.has_value()) {
            std::cerr << "could not query any validator status\n";
            return 1;
        }
        const auto active_peer = peer_status->peer;
        const auto status = peer_status->status;
        const primechain::PrimeValue effective_frontier =
            status.has_genesis ? status.frontier : 2;
        if (effective_frontier >= limit) {
            std::cout << "frontier miner complete frontier=" << effective_frontier
                      << " submitted=" << submitted << "\n";
            return 0;
        }

        if (proof_store_frontier.has_value() && effective_frontier > *proof_store_frontier) {
            std::cerr << "local proof store behind validator frontier; sync required"
                      << " local_frontier=" << *proof_store_frontier
                      << " validator=" << active_peer.host << ":" << active_peer.port
                      << " validator_frontier=" << effective_frontier << "\n";
            return 1;
        }

        const primechain::PrimeValue next = effective_frontier + 1;
        auto retryCurrentInteger = [&](const std::string& reason) -> bool {
            auto& attempts = retry_counts[next];
            if (attempts >= 5) {
                std::cerr << "retry limit reached for " << next << ": " << reason << "\n";
                return false;
            }
            ++attempts;
            std::cerr << "frontier changed while mining " << next << "; retrying: "
                      << reason << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return true;
        };
        std::string request;
        std::string proof_summary;
        std::optional<std::string> commit_request;
        bool reused_pending_composite = false;
        bool skip_commit_request = false;
        if (primechain::math::isPrime(next)) {
            const auto proof = primechain::math::makePrattProof(next, proofs);
            if (!proof.has_value() || !primechain::math::verifyPrattProof(*proof)) {
                std::cerr << "could not construct Pratt proof for " << next
                          << "; start from a fresh node or add proof-index bootstrap\n";
                return 1;
            }
            if (prime_identity.has_value()) {
                auto previous_hash = hashFromHex(status.latest_hash);
                std::string error;
                if (!status.has_genesis) {
                    previous_hash = primechain::storage::makeStoredRecord(
                        primechain::node::makeGenesisPrimeRecordV0()).record_hash;
                }
                if (!previous_hash.has_value()) {
                    std::cerr << "node returned invalid latest record hash\n";
                    return 1;
                }
                const auto signed_request = signedPrimeSubmission(
                    *proof, *previous_hash, *prime_identity, error);
                if (!signed_request.has_value()) {
                    std::cerr << "could not sign prime submission: " << error << "\n";
                    return 1;
                }
                request = *signed_request;
            } else {
                request = primeSubmission(*proof, prime_miner);
            }
            proof_summary = prattProofSummary(*proof);
        } else {
            auto proof = primechain::math::makeCompositeProof(next, composite_miner);
            if (!proof.has_value() || !primechain::math::verifyCompositeProof(*proof)) {
                std::cerr << "could not construct composite proof for " << next << "\n";
                return 1;
            }
            const auto local_provider = composite_identity.has_value()
                ? composite_identity->address : composite_miner;
            std::uint64_t nonce = stableCompositeNonce(*proof, local_provider);
            if (pending_composite_path.has_value()) {
                const auto pending = loadPendingComposite(*pending_composite_path);
                if (pending.has_value() && pending->integer == next &&
                    pending->provider == local_provider) {
                    primechain::CompositeProof pending_proof;
                    pending_proof.m = pending->integer;
                    pending_proof.d = pending->d;
                    pending_proof.e = pending->e;
                    if (primechain::math::verifyCompositeProof(pending_proof)) {
                        proof = pending_proof;
                        nonce = pending->nonce;
                        reused_pending_composite = true;
                    }
                }
                if (!reused_pending_composite) {
                    PendingComposite replacement;
                    replacement.integer = next;
                    replacement.d = proof->d;
                    replacement.e = proof->e;
                    replacement.nonce = nonce;
                    replacement.provider = local_provider;
                    if (!writePendingComposite(*pending_composite_path, replacement)) return 1;
                }
            }
            proofs.add(*proof);
            if (composite_identity.has_value()) {
                std::string error;
                const auto signed_reveal = signedCompositeRevealSubmission(
                    *proof, nonce, *composite_identity, error);
                if (!signed_reveal.has_value()) {
                    std::cerr << "could not sign composite submission: " << error << "\n";
                    return 1;
                }
                request = *signed_reveal;
            } else {
                commit_request = compositeCommitSubmission(*proof, nonce, composite_miner);
                request = compositeRevealSubmission(*proof, nonce, composite_miner);
            }
            proof_summary = compositeProofSummary(*proof);
        }

        const auto local_provider = composite_identity.has_value()
            ? composite_identity->address : composite_miner;

        if (commit_request.has_value()) {
            const auto view = requestMiningView(active_peer, next);
            if (view.has_value()) {
                switch (viewPhaseState(*view)) {
                case CommitPhaseState::Closed:
                    if (view->winner != local_provider || !reused_pending_composite) {
                        if (view->winner != local_provider) clearPendingComposite(pending_composite_path);
                        if (retryCurrentInteger("commit phase already won by " + view->winner)) continue;
                        return 1;
                    }
                    skip_commit_request = true;
                    break;
                case CommitPhaseState::Closing:
                    if (retryCurrentInteger("commit phase is closing on " + active_peer.host + ":" +
                                            std::to_string(active_peer.port))) continue;
                    return 1;
                case CommitPhaseState::Open:
                case CommitPhaseState::Unknown:
                    break;
                }
            }
        }

        if (commit_request.has_value() && !skip_commit_request) {
            const auto commit_response = requestLine(active_peer.host, active_peer.port, *commit_request);
            if (!commit_response.has_value()) {
                if (retryCurrentInteger("node closed connection while committing")) continue;
                return 1;
            }
            std::cout << withPeerPrefix(active_peer, *commit_response) << "\n";
            if (!commitAcceptedOrDuplicate(*commit_response)) {
                if (staleOrTransient(*commit_response) && retryCurrentInteger(*commit_response)) continue;
                return 1;
            }
        }

        bool submitted_ok = false;
        bool got_response = false;
        std::string last_rejection;
        const auto response = requestLine(active_peer.host, active_peer.port, request);
        if (!response.has_value()) {
            if (retryCurrentInteger("node closed connection while submitting")) continue;
            return 1;
        }
        got_response = true;
        std::cout << withPeerPrefix(active_peer, *response) << "\n";
        if ((response->rfind("PRIME_ACCEPTED ", 0) == 0 ||
             response->rfind("COMPOSITE_ACCEPTED ", 0) == 0) &&
            !proof_summary.empty()) {
            std::cout << withPeerPrefix(active_peer, proof_summary) << "\n";
        }
        if (accepted(*response)) {
            submitted_ok = true;
        } else {
            last_rejection = *response;
        }
        if (!got_response) {
            if (retryCurrentInteger("node closed connection while submitting")) continue;
            return 1;
        }
        if (!submitted_ok) {
            if (commit_request.has_value() && resetsCompositeCommitState(last_rejection)) {
                clearPendingComposite(pending_composite_path);
            }
            if (staleOrTransient(last_rejection) && retryCurrentInteger(last_rejection)) continue;
            return 1;
        }
        retry_counts.erase(next);
        if (commit_request.has_value()) {
            clearPendingComposite(pending_composite_path);
        }
        if (proof_store_frontier.has_value() && next > *proof_store_frontier) {
            *proof_store_frontier = next;
        }
        ++submitted;
    }
}
