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
#include <sys/socket.h>
#include <unistd.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

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

    int fd() const { return fd_; }

private:
    int fd_{-1};
};

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

void handleClient(int fd, const primechain::storage::RecordStore& store) {
    const auto line = readLine(fd);
    if (!line.has_value()) {
        return;
    }

    std::string error;
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        writeAll(fd, "ERROR " + error + "\n");
        return;
    }
    if (records.empty()) {
        writeAll(fd, "ERROR empty corrupt store\n");
        return;
    }

    if (*line == "GET_STATUS") {
        std::uint64_t prime_records = 0;
        std::uint64_t composite_records = 0;
        for (const auto& record : records) {
            if (record.kind == primechain::storage::StoredRecordKind::Prime) {
                ++prime_records;
            } else {
                ++composite_records;
            }
        }
        const auto& latest = records.back();
        std::ostringstream out;
        out << "STATUS " << records.size() << " "
            << prime_records << " "
            << composite_records << " "
            << "1 "
            << latest.height << " "
            << latest.integer << " "
            << primechain::crypto::toHex(latest.record_hash) << "\n";
        writeAll(fd, out.str());
        return;
    }

    if (line->rfind("GET_RECORD_RANGE ", 0) == 0) {
        std::istringstream in(*line);
        std::string command;
        primechain::PrimeValue start = 0;
        primechain::PrimeValue end = 0;
        in >> command >> start >> end;
        if (!in) {
            writeAll(fd, "ERROR invalid GET_RECORD_RANGE\n");
            return;
        }

        std::vector<primechain::storage::StoredRecord> range;
        for (const auto& record : records) {
            if (record.integer >= start && record.integer <= end) {
                range.push_back(record);
            }
        }
        std::ostringstream header;
        header << "RECORD_RANGE " << start << " " << end << " " << range.size() << "\n";
        writeAll(fd, header.str());
        for (const auto& record : range) {
            writeAll(fd, recordLine(record));
        }
        writeAll(fd, "END_RECORD_RANGE\n");
        return;
    }

    writeAll(fd, "ERROR unsupported corrupt test command\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <port> <record_store_path>\n";
        return 1;
    }

    const int port = std::stoi(argv[1]);
    primechain::storage::RecordStore store(argv[2]);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    auto server = listenOnPort(port);
    if (!server.has_value()) {
        return 1;
    }

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = accept(server->fd(), reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        Socket client(client_fd);
        handleClient(client.fd(), store);
    }

    return 0;
}
