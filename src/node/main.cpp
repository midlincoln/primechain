#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "primechain/core/consensus.hpp"
#include "primechain/crypto/hash.hpp"
#include "primechain/types.hpp"

namespace {

constexpr int kDefaultPort = 18888;
constexpr const char* kDefaultDataDir = "data";
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
    bool valid() const { return fd_ >= 0; }

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

bool ensureDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
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

primechain::Block parseSubmittedBlock(const std::string& line, const primechain::ChainState& state) {
    std::istringstream in(line);
    std::string command;
    primechain::PrimeValue prime = 0;
    std::string miner_address;
    std::size_t proof_count = 0;
    in >> command >> prime >> miner_address >> proof_count;

    primechain::Block block;
    block.header.previous_block_hash = state.last_block_hash;
    block.header.prime_value = prime;
    block.header.composite_range_start = state.frontier_prime + 1;
    block.header.composite_range_end = prime > 0 ? prime - 1 : 0;
    block.header.timestamp = state.height + 1;
    block.header.miner_address = miner_address;
    block.prime_certificate.data = {'T', 'C', 'P', '-', 'M', 'V', 'P'};

    for (std::size_t i = 0; i < proof_count; ++i) {
        primechain::CompositeProof proof;
        in >> proof.m >> proof.d >> proof.e;
        proof.provider_address = miner_address;
        block.composite_proofs.push_back(proof);
    }

    return block;
}

class PrimeNode {
public:
    explicit PrimeNode(std::string data_dir)
        : data_dir_(std::move(data_dir)),
          chain_log_path_(data_dir_ + "/chain.log") {}

    bool loadChainLog() {
        if (!ensureDirectory(data_dir_)) {
            std::cerr << "could not create data directory: " << data_dir_ << "\n";
            return false;
        }

        std::ifstream in(chain_log_path_);
        if (!in) {
            return true;
        }

        std::string line;
        std::uint64_t replayed = 0;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }

            primechain::Block block = parseSubmittedBlock(line, state_);
            std::string error;
            if (!consensus_.validateBlock(block, state_, error)) {
                std::cerr << "invalid persisted block at replay height "
                          << (state_.height + 1) << ": " << error << "\n";
                return false;
            }

            state_ = consensus_.applyBlock(block, state_);
            ++replayed;
        }

        if (replayed > 0) {
            std::cout << "restored " << replayed << " blocks from " << chain_log_path_
                      << "; frontier prime " << state_.frontier_prime << "\n";
        }
        return true;
    }

    void handleClient(int fd) {
        while (auto line = readLine(fd)) {
            if (*line == "GET_TIP") {
                sendTip(fd);
                continue;
            }
            if (line->rfind("SUBMIT_BLOCK ", 0) == 0) {
                handleBlockSubmission(fd, *line);
                continue;
            }
            writeAll(fd, "ERROR unknown command\n");
        }
    }

    const primechain::ChainState& state() const {
        return state_;
    }

private:
    void sendTip(int fd) const {
        std::ostringstream out;
        out << "TIP " << state_.height << " " << state_.frontier_prime << " "
            << primechain::crypto::toHex(state_.last_block_hash) << "\n";
        writeAll(fd, out.str());
    }

    void handleBlockSubmission(int fd, const std::string& line) {
        primechain::Block block = parseSubmittedBlock(line, state_);
        std::string error;
        if (!consensus_.validateBlock(block, state_, error)) {
            writeAll(fd, "REJECTED " + error + "\n");
            return;
        }

        if (!appendAcceptedBlock(line)) {
            writeAll(fd, "REJECTED could not persist block\n");
            return;
        }

        state_ = consensus_.applyBlock(block, state_);
        std::ostringstream out;
        out << "ACCEPTED height=" << state_.height
            << " prime=" << state_.frontier_prime
            << " hash=" << primechain::crypto::toHex(state_.last_block_hash).substr(0, 16)
            << "\n";
        writeAll(fd, out.str());

        std::cout << "accepted block height " << state_.height
                  << " prime " << state_.frontier_prime
                  << " miner " << block.header.miner_address << "\n";
    }

    bool appendAcceptedBlock(const std::string& line) const {
        std::ofstream out(chain_log_path_, std::ios::app);
        if (!out) {
            return false;
        }
        out << line << "\n";
        return static_cast<bool>(out);
    }

    primechain::core::ConsensusEngine consensus_;
    primechain::ChainState state_;
    std::string data_dir_;
    std::string chain_log_path_;
};

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const int port = argc > 1 ? std::stoi(argv[1]) : kDefaultPort;
    const std::string data_dir = argc > 2 ? argv[2] : kDefaultDataDir;
    auto server = listenOnPort(port);
    if (!server.has_value()) {
        return 1;
    }

    PrimeNode node(data_dir);
    if (!node.loadChainLog()) {
        return 1;
    }

    std::cout << "Prime Mining TCP node listening on 127.0.0.1:" << port << "\n";
    std::cout << "data directory: " << data_dir << "\n";
    std::cout << "current height: " << node.state().height << "\n";
    std::cout << "current frontier prime: " << node.state().frontier_prime << "\n";

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
        node.handleClient(client.fd());
    }

    std::cout << "node stopped\n";
    return 0;
}
