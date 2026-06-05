#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
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
