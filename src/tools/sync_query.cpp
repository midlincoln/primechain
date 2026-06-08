#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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
        if (received == 0) return line.empty() ? std::nullopt : std::optional(line);
        if (received < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (ch == '\n') return line;
        if (line.size() >= 1024 * 1024) return std::nullopt;
        line.push_back(ch);
    }
}

std::optional<std::string> readMessage(int fd) {
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

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [host] [port] [command...]\n"
              << "example:\n"
              << "  " << argv0 << " 127.0.0.1 18889 GET_STATUS\n"
              << "  " << argv0 << " 127.0.0.1 18889 GET_RECORD 500\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 1;
    }

    const std::string host = argv[1];
    const int port = std::stoi(argv[2]);

    std::ostringstream command;
    for (int i = 3; i < argc; ++i) {
        if (i > 3) {
            command << " ";
        }
        command << argv[i];
    }

    auto socket = connectToServer(host, port);
    if (!socket.has_value()) {
        return 1;
    }
    if (!writeCommand(socket->fd(), command.str())) {
        std::cerr << "could not send command\n";
        return 1;
    }
    shutdown(socket->fd(), SHUT_WR);

    while (const auto message = readMessage(socket->fd())) {
        std::cout << *message << "\n";
    }

    return 0;
}
