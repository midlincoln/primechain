#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include <arpa/inet.h>
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

struct Work {
    std::uint64_t height{0};
    primechain::PrimeValue frontier{2};
    primechain::PrimeValue start{3};
    primechain::PrimeValue end{0};
};

std::optional<Work> getWork(int fd) {
    if (!writeAll(fd, "GET_WORK\n")) {
        return std::nullopt;
    }

    const auto response = readLine(fd);
    if (!response.has_value()) {
        return std::nullopt;
    }

    std::istringstream in(*response);
    std::string tag;
    Work work;
    in >> tag >> work.height >> work.frontier >> work.start >> work.end;
    if (tag != "WORK" || !in) {
        std::cerr << "unexpected work response: " << *response << "\n";
        return std::nullopt;
    }
    return work;
}

bool submitProof(int fd, const primechain::CompositeProof& proof) {
    std::ostringstream out;
    out << "SUBMIT_PROOF " << proof.m << " " << proof.d << " "
        << proof.e << " " << proof.provider_address << "\n";
    if (!writeAll(fd, out.str())) {
        return false;
    }
    const auto response = readLine(fd);
    if (!response.has_value()) {
        return false;
    }
    std::cout << *response << "\n";
    return response->rfind("ACCEPTED_PROOF", 0) == 0 || response->rfind("KNOWN_PROOF", 0) == 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : kDefaultHost;
    const int port = argc > 2 ? std::stoi(argv[2]) : kDefaultPort;
    const std::string provider = argc > 3 ? argv[3] : "composite-miner";
    const int rounds = argc > 4 ? std::stoi(argv[4]) : 1;
    const int shard_count = argc > 5 ? std::stoi(argv[5]) : 1;
    const int shard_index = argc > 6 ? std::stoi(argv[6]) : 0;

    if (shard_count <= 0 || shard_index < 0 || shard_index >= shard_count) {
        std::cerr << "invalid shard arguments\n";
        return 1;
    }

    auto socket = connectToNode(host, port);
    if (!socket.has_value()) {
        return 1;
    }

    for (int round = 0; round < rounds; ++round) {
        auto work = getWork(socket->fd());
        if (!work.has_value()) {
            std::cerr << "could not get work\n";
            return 1;
        }

        std::cout << "work height " << work->height
                  << " frontier " << work->frontier
                  << " window " << work->start << ".." << work->end << "\n";

        std::size_t submitted = 0;
        for (primechain::PrimeValue m = work->start; m <= work->end; ++m) {
            if (static_cast<int>(m % static_cast<primechain::PrimeValue>(shard_count)) != shard_index) {
                continue;
            }
            auto proof = primechain::math::makeCompositeProof(m, provider);
            if (!proof.has_value()) {
                continue;
            }
            if (!submitProof(socket->fd(), *proof)) {
                return 1;
            }
            ++submitted;
        }
        std::cout << "submitted_or_confirmed " << submitted << " composite proofs\n";
    }

    return 0;
}
