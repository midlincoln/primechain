#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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

struct Status {
    std::uint64_t record_count{0};
    std::uint64_t prime_records{0};
    std::uint64_t composite_records{0};
    bool has_genesis{false};
    std::uint64_t height{0};
    primechain::PrimeValue frontier{0};
    std::string latest_hash;
};

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

std::optional<std::string> readLine(int fd) {
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

std::optional<std::string> requestLine(const std::string& host, int port, const std::string& request) {
    auto socket = connectToNode(host, port);
    if (!socket.has_value()) {
        return std::nullopt;
    }
    if (!writeAll(socket->fd(), request)) {
        return std::nullopt;
    }
    shutdown(socket->fd(), SHUT_WR);
    return readLine(socket->fd());
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

std::uint64_t randomNonce() {
    std::random_device source;
    return (static_cast<std::uint64_t>(source()) << 32) ^
           static_cast<std::uint64_t>(source());
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
           response.rfind("RECORD_REPLACED ", 0) == 0 ||
           response.rfind("RECORD_CONFLICT_WORSE ", 0) == 0;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [host] [port] [limit] --prime-identity <file> --composite-identity <file>\n"
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
    int argument = 4;
    while (argument < argc) {
        const std::string option = argv[argument++];
        if (option == "--prime-identity" || option == "--composite-identity") {
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
    std::size_t submitted = 0;

    while (true) {
        const auto status = getStatus(host, port);
        if (!status.has_value()) {
            std::cerr << "could not query node status\n";
            return 1;
        }
        const primechain::PrimeValue effective_frontier =
            status->has_genesis ? status->frontier : 2;
        if (effective_frontier >= limit) {
            std::cout << "frontier miner complete frontier=" << effective_frontier
                      << " submitted=" << submitted << "\n";
            return 0;
        }

        const primechain::PrimeValue next = effective_frontier + 1;
        std::string request;
        std::optional<std::string> commit_request;
        if (primechain::math::isPrime(next)) {
            const auto proof = primechain::math::makePrattProof(next, proofs);
            if (!proof.has_value() || !primechain::math::verifyPrattProof(*proof)) {
                std::cerr << "could not construct Pratt proof for " << next
                          << "; start from a fresh node or add proof-index bootstrap\n";
                return 1;
            }
            if (prime_identity.has_value()) {
                auto previous_hash = hashFromHex(status->latest_hash);
                std::string error;
                if (!status->has_genesis) {
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
        } else {
            const auto proof = primechain::math::makeCompositeProof(next, composite_miner);
            if (!proof.has_value() || !primechain::math::verifyCompositeProof(*proof)) {
                std::cerr << "could not construct composite proof for " << next << "\n";
                return 1;
            }
            proofs.add(*proof);
            const std::uint64_t nonce = randomNonce();
            if (composite_identity.has_value()) {
                std::string error;
                commit_request = signedCompositeCommitSubmission(
                    *proof, nonce, *composite_identity, error);
                const auto signed_reveal = signedCompositeRevealSubmission(
                    *proof, nonce, *composite_identity, error);
                if (!commit_request.has_value() || !signed_reveal.has_value()) {
                    std::cerr << "could not sign composite submission: " << error << "\n";
                    return 1;
                }
                request = *signed_reveal;
            } else {
                commit_request = compositeCommitSubmission(*proof, nonce, composite_miner);
                request = compositeRevealSubmission(*proof, nonce, composite_miner);
            }
        }

        if (commit_request.has_value()) {
            const auto commit_response = requestLine(host, port, *commit_request);
            if (!commit_response.has_value()) {
                std::cerr << "node closed connection while committing " << next << "\n";
                return 1;
            }
            std::cout << *commit_response << "\n";
            if (commit_response->rfind("COMMIT_ACCEPTED ", 0) != 0 &&
                commit_response->rfind("COMMIT_DUPLICATE ", 0) != 0) {
                return 1;
            }
        }

        const auto response = requestLine(host, port, request);
        if (!response.has_value()) {
            std::cerr << "node closed connection while submitting " << next << "\n";
            return 1;
        }
        std::cout << *response << "\n";
        if (!accepted(*response)) {
            return 1;
        }
        ++submitted;
    }
}
