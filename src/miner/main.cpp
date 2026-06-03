#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "primechain/math/number_theory.hpp"
#include "primechain/types.hpp"

namespace {

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 18888;

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
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

struct Tip {
    std::uint64_t height{0};
    primechain::PrimeValue frontier{2};
};

std::optional<Tip> getTip(int fd) {
    if (!writeAll(fd, "GET_TIP\n")) {
        return std::nullopt;
    }

    const auto response = readLine(fd);
    if (!response.has_value()) {
        return std::nullopt;
    }

    std::istringstream in(*response);
    std::string tag;
    Tip tip;
    std::string ignored_hash;
    in >> tag >> tip.height >> tip.frontier >> ignored_hash;
    if (tag != "TIP" || !in) {
        std::cerr << "unexpected tip response: " << *response << "\n";
        return std::nullopt;
    }
    return tip;
}

std::vector<primechain::CompositeProof> getProofs(int fd, primechain::PrimeValue start, primechain::PrimeValue end) {
    if (start > end) {
        return {};
    }

    std::ostringstream request;
    request << "GET_PROOFS " << start << " " << end << "\n";
    if (!writeAll(fd, request.str())) {
        return {};
    }

    const auto response = readLine(fd);
    if (!response.has_value()) {
        return {};
    }

    std::istringstream in(*response);
    std::string tag;
    std::size_t count = 0;
    in >> tag >> count;
    if (tag != "PROOFS" || !in) {
        std::cerr << "unexpected proof response: " << *response << "\n";
        return {};
    }

    std::vector<primechain::CompositeProof> proofs;
    for (std::size_t i = 0; i < count; ++i) {
        primechain::CompositeProof proof;
        in >> proof.m >> proof.d >> proof.e >> proof.provider_address;
        if (!in) {
            return {};
        }
        proofs.push_back(proof);
    }
    return proofs;
}

std::string buildSubmission(const Tip& tip, const std::string& miner_address) {
    const primechain::PrimeValue next_prime = primechain::math::nextPrimeAfter(tip.frontier);

    std::ostringstream out;
    out << "SUBMIT_BLOCK " << next_prime << " " << miner_address << " 0";
    out << "\n";
    return out.str();
}

std::string buildSubmission(
    const Tip& tip,
    const std::string& miner_address,
    const std::vector<primechain::CompositeProof>& proofs) {
    const primechain::PrimeValue next_prime = primechain::math::nextPrimeAfter(tip.frontier);
    std::ostringstream out;
    out << "SUBMIT_BLOCK " << next_prime << " " << miner_address << " " << proofs.size();
    for (const auto& proof : proofs) {
        out << " " << proof.m << " " << proof.d << " " << proof.e << " " << proof.provider_address;
    }
    out << "\n";
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : kDefaultHost;
    const int port = argc > 2 ? std::stoi(argv[2]) : kDefaultPort;
    const int blocks = argc > 3 ? std::stoi(argv[3]) : 1;
    const std::string miner_address = argc > 4 ? argv[4] : "terminal-miner";

    auto socket = connectToNode(host, port);
    if (!socket.has_value()) {
        return 1;
    }

    for (int i = 0; i < blocks; ++i) {
        auto tip = getTip(socket->fd());
        if (!tip.has_value()) {
            std::cerr << "could not read node tip\n";
            return 1;
        }

        const primechain::PrimeValue next_prime = primechain::math::nextPrimeAfter(tip->frontier);
        std::cout << "mining height " << (tip->height + 1)
                  << " from frontier " << tip->frontier
                  << " to prime " << next_prime << "\n";

        const auto proofs = getProofs(socket->fd(), tip->frontier + 1, next_prime - 1);
        if (proofs.size() != static_cast<std::size_t>(next_prime - tip->frontier - 1)) {
            std::cerr << "missing pooled proofs for interval "
                      << (tip->frontier + 1) << ".." << (next_prime - 1)
                      << " got " << proofs.size()
                      << " expected " << (next_prime - tip->frontier - 1) << "\n";
            return 1;
        }

        const std::string submission = buildSubmission(*tip, miner_address, proofs);
        if (!writeAll(socket->fd(), submission)) {
            std::cerr << "could not submit block\n";
            return 1;
        }

        auto response = readLine(socket->fd());
        if (!response.has_value()) {
            std::cerr << "node closed connection\n";
            return 1;
        }
        std::cout << *response << "\n";
        if (response->rfind("ACCEPTED", 0) != 0) {
            return 1;
        }
    }

    return 0;
}
