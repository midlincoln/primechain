#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

bool currentFileSize(const std::string& path, std::uint64_t& size, std::string& error) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            size = 0;
            return true;
        }
        error = "could not stat output store: " + std::string(std::strerror(errno));
        return false;
    }
    if (info.st_size < 0) {
        error = "output store has invalid size";
        return false;
    }
    size = static_cast<std::uint64_t>(info.st_size);
    return true;
}

bool truncateOutputStore(const std::string& path, std::uint64_t size, std::string& error) {
    if (truncate(path.c_str(), static_cast<off_t>(size)) != 0) {
        error = "could not roll back output store: " + std::string(std::strerror(errno));
        return false;
    }
    std::remove((path + ".idx").c_str());
    return true;
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
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << "\n";
        close(fd);
        return std::nullopt;
    }
    if (!setSocketTimeouts(fd, 20000)) {
        std::cerr << "could not set socket timeouts: " << std::strerror(errno) << "\n";
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

std::optional<std::string> readRawLine(int fd) {
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
    if (!kind.has_value() || !hash.has_value() || !payload.has_value()) {
        return std::nullopt;
    }
    if (payload->size() != payload_size) {
        return std::nullopt;
    }
    const auto canonical_hash = canonicalRecordHashFromPayload(*kind, *payload);
    if (!canonical_hash.has_value() || *canonical_hash != *hash) {
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
    if (start > end) {
        std::cerr << "invalid range: start is greater than end\n";
        return 1;
    }

    std::string error;
    primechain::node::SequentialNode existing_node(output_store);
    if (!existing_node.load(error)) {
        std::cerr << "existing output store did not replay: " << error << "\n";
        return 1;
    }
    if (!existing_node.status().has_genesis) {
        if (start != 2) {
            std::cerr << "empty output store must start syncing at integer 2\n";
            return 1;
        }
    } else if (start != existing_node.status().frontier_integer + 1) {
        std::cerr << "resume start must be exactly frontier + 1; current frontier is "
                  << existing_node.status().frontier_integer << "\n";
        return 1;
    }

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

    std::uint64_t rollback_size = 0;
    if (!currentFileSize(output_store, rollback_size, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    auto failDownload = [&](const std::string& message) {
        std::string rollback_error;
        if (!truncateOutputStore(output_store, rollback_size, rollback_error)) {
            std::cerr << message << "; " << rollback_error << "\n";
            return 1;
        }
        std::cerr << message << "\n";
        return 1;
    };

    primechain::storage::RecordStore output(output_store);
    std::uint64_t downloaded = 0;
    while (const auto line = readLine(socket->fd())) {
        if (*line == "END_RECORD_RANGE") {
            break;
        }
        const auto record = parseRecordLine(*line);
        if (!record.has_value()) {
            return failDownload("invalid record line");
        }
        const primechain::PrimeValue expected_integer = start + downloaded;
        const std::uint64_t expected_height = expected_integer - 2;
        if (record->integer != expected_integer) {
            std::ostringstream message;
            message << "downloaded record integer sequence mismatch: expected "
                    << expected_integer << " got " << record->integer;
            return failDownload(message.str());
        }
        if (record->height != expected_height) {
            std::ostringstream message;
            message << "downloaded record height mismatch for integer "
                    << record->integer << ": expected " << expected_height
                    << " got " << record->height;
            return failDownload(message.str());
        }
        if (!output.append(*record, error)) {
            return failDownload("could not append downloaded record: " + error);
        }
        ++downloaded;
    }

    if (downloaded != expected_count) {
        std::ostringstream message;
        message << "downloaded record count mismatch: expected " << expected_count
                << " got " << downloaded;
        return failDownload(message.str());
    }

    primechain::node::SequentialNode node(output_store);
    error.clear();
    if (!node.load(error)) {
        return failDownload("downloaded store did not replay: " + error);
    }
    if (!node.status().has_genesis || node.status().frontier_integer != end) {
        std::ostringstream message;
        message << "downloaded store did not reach requested end: expected "
                << end << " got " << node.status().frontier_integer;
        return failDownload(message.str());
    }

    std::cout << "sync download complete\n";
    std::cout << "output_store: " << output_store << "\n";
    std::cout << "records: " << downloaded << "\n";
    std::cout << "start: " << start << "\n";
    std::cout << "end: " << end << "\n";
    std::cout << "frontier_integer: " << node.status().frontier_integer << "\n";
    return 0;
}
