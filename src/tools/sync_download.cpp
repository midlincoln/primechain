#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

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
        if (received == 0) {
            if (line.empty()) {
                return std::nullopt;
            }
            return line;
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
        if (line.size() > 1024 * 1024) {
            return std::nullopt;
        }
        line.push_back(ch);
    }
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::optional<std::vector<std::uint8_t>> hexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hexValue(hex[i]);
        const int low = hexValue(hex[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

std::optional<primechain::Hash256> parseHash(const std::string& hex) {
    const auto bytes = hexToBytes(hex);
    if (!bytes.has_value() || bytes->size() != 32) {
        return std::nullopt;
    }
    primechain::Hash256 hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = (*bytes)[i];
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
    if (!kind.has_value() || !hash.has_value() || !payload.has_value()) {
        return std::nullopt;
    }
    if (payload->size() != payload_size) {
        return std::nullopt;
    }
    if (primechain::crypto::devHash256(*payload) != *hash) {
        return std::nullopt;
    }

    record.kind = *kind;
    record.record_hash = *hash;
    record.payload = *payload;
    return record;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [host] [port] [start] [end] [output_store]\n"
              << "example:\n"
              << "  " << argv0 << " 127.0.0.1 18889 2 500 ./downloaded.dat\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6 || std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 1;
    }

    const std::string host = argv[1];
    const int port = std::stoi(argv[2]);
    const primechain::PrimeValue start = std::stoull(argv[3]);
    const primechain::PrimeValue end = std::stoull(argv[4]);
    const std::string output_store = argv[5];

    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        return 1;
    }

    std::ostringstream command;
    command << "GET_RECORD_RANGE " << start << " " << end << "\n";
    if (!writeAll(socket->fd(), command.str())) {
        std::cerr << "could not send range request\n";
        return 1;
    }
    shutdown(socket->fd(), SHUT_WR);

    auto header = readLine(socket->fd());
    if (!header.has_value()) {
        std::cerr << "missing range response\n";
        return 1;
    }

    std::istringstream header_in(*header);
    std::string header_tag;
    primechain::PrimeValue response_start = 0;
    primechain::PrimeValue response_end = 0;
    std::uint64_t expected_count = 0;
    header_in >> header_tag >> response_start >> response_end >> expected_count;
    if (!header_in || header_tag != "RECORD_RANGE" || response_start != start || response_end != end) {
        std::cerr << "invalid range response header: " << *header << "\n";
        return 1;
    }

    primechain::storage::RecordStore output(output_store);
    std::string error;
    std::uint64_t downloaded = 0;
    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_RECORD_RANGE") {
            break;
        }
        const auto record = parseRecordLine(*line);
        if (!record.has_value()) {
            std::cerr << "invalid record line\n";
            return 1;
        }
        if (!output.append(*record, error)) {
            std::cerr << "could not append downloaded record: " << error << "\n";
            return 1;
        }
        ++downloaded;
    }

    if (downloaded != expected_count) {
        std::cerr << "downloaded record count mismatch\n";
        return 1;
    }

    primechain::node::SequentialNode node(output_store);
    error.clear();
    if (!node.load(error)) {
        std::cerr << "downloaded store did not replay: " << error << "\n";
        return 1;
    }

    std::cout << "sync download complete\n";
    std::cout << "output_store: " << output_store << "\n";
    std::cout << "records: " << downloaded << "\n";
    std::cout << "frontier_integer: " << node.status().frontier_integer << "\n";
    return 0;
}
