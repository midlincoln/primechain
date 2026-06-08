#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "primechain/crypto/signature.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/wallet/miner_identity.hpp"

namespace {

struct DevWallet {
    std::string address;
    std::vector<std::uint8_t> public_key;
};

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
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

bool loadWallet(const std::string& path, DevWallet& wallet) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        if (line.substr(0, pos) == "address") {
            wallet.address = line.substr(pos + 1);
        } else if (line.substr(0, pos) == "public_key") {
            wallet.public_key = hexToBytes(line.substr(pos + 1));
        }
    }
    return primechain::protocol::isDevelopmentAddress(wallet.address) &&
           !wallet.public_key.empty() &&
           wallet.address == primechain::protocol::developmentAddressFromPublicKey(wallet.public_key);
}

std::string siblingExecutable(const char* argv0, const std::string& executable_name) {
    const std::string self = argv0;
    const auto slash = self.find_last_of('/');
    if (slash == std::string::npos) {
        return executable_name;
    }
    return self.substr(0, slash + 1) + executable_name;
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

primechain::protocol::TransactionV0 makeTransferTransaction(
    const DevWallet& sender,
    const std::string& receiver_address,
    primechain::PrimeValue prime,
    std::uint64_t amount,
    std::uint64_t nonce) {
    primechain::protocol::TransactionV0 tx;
    tx.version = 0;
    tx.inputs.push_back({prime, {amount, 1}});
    tx.outputs.push_back({prime, {amount, 1}, receiver_address});
    tx.fee = {prime, {0, 1}};
    tx.nonce = nonce;
    tx.sender_address = sender.address;
    tx.sender_public_key = sender.public_key;
    tx.signature = primechain::protocol::developmentTransactionSignature(tx);
    return tx;
}

std::optional<primechain::protocol::TransactionV0> makeAuthenticatedTransferTransaction(
    const primechain::wallet::MinerIdentity& sender,
    const std::string& receiver_address,
    primechain::PrimeValue prime,
    std::uint64_t amount,
    std::uint64_t fee,
    std::uint64_t nonce,
    std::string& error) {
    if (fee > std::numeric_limits<std::uint64_t>::max() - amount) {
        error = "amount plus fee overflows micro-units";
        return std::nullopt;
    }
    primechain::protocol::TransactionV0 tx;
    tx.version = 2;
    tx.inputs.push_back({prime, {amount + fee, 1}});
    tx.outputs.push_back({prime, {amount, 1}, receiver_address});
    tx.fee = {prime, {fee, 1}};
    tx.nonce = nonce;
    tx.sender_address = sender.address;
    tx.sender_public_key = sender.public_key;
    const auto signature = primechain::crypto::signProtocolMessage(
        sender.private_key,
        primechain::crypto::transactionSigningPayload(
            primechain::protocol::serializeTransaction(tx, false)),
        error);
    if (!signature.has_value()) return std::nullopt;
    tx.signature = *signature;
    return tx;
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " <limit> <text_log_path> <record_store_path> <sender.wallet> <receiver_address> <prime> <amount> <target_integer> [--composite-miner address]\n"
              << "  " << argv0 << " submit <host> <port> <sender.wallet> <receiver_address> <prime> <amount> <nonce>\n"
              << "  " << argv0 << " submit <host> <port> <sender.wallet> <receiver_address> <prime> <amount> <fee> <nonce>\n"
              << "example:\n"
              << "  " << argv0 << " 20 ./data/tx.log ./data/tx.dat ./wallets/miner.wallet pcdev1_alice 3 250000 4\n"
              << "  " << argv0 << " submit 127.0.0.1 18889 ./wallets/sender-mldsa65.wallet pcpq1_receiver 3 250000 1000 1\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "submit") {
        if (argc != 9 && argc != 10) {
            printUsage(argv[0]);
            return 1;
        }
        const std::string host = argv[2];
        const int port = std::stoi(argv[3]);
        const std::string sender_wallet_path = argv[4];
        const std::string receiver_address = argv[5];
        const auto prime = static_cast<primechain::PrimeValue>(std::stoull(argv[6]));
        const auto amount = static_cast<std::uint64_t>(std::stoull(argv[7]));
        const auto fee = argc == 10
            ? static_cast<std::uint64_t>(std::stoull(argv[8]))
            : 0;
        const auto nonce = static_cast<std::uint64_t>(std::stoull(argv[argc - 1]));

        primechain::wallet::MinerIdentity sender;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(sender_wallet_path, sender, error)) {
            std::cerr << "could not load authenticated sender wallet: " << error << "\n";
            return 1;
        }
        if (!primechain::protocol::isProtocolAddress(receiver_address) || prime < 2 || amount == 0) {
            std::cerr << "invalid transfer arguments\n";
            return 1;
        }

        const auto tx = makeAuthenticatedTransferTransaction(
            sender, receiver_address, prime, amount, fee, nonce, error);
        if (!tx.has_value()) {
            std::cerr << "could not sign transaction: " << error << "\n";
            return 1;
        }
        const std::string tx_hex = bytesToHex(primechain::protocol::serializeTransaction(*tx, true));
        auto socket = connectToServer(host, port);
        if (!socket.has_value()) {
            return 1;
        }
        if (!writeAll(socket->fd(), "SUBMIT_TX " + tx_hex + "\n")) {
            std::cerr << "could not send transaction\n";
            return 1;
        }
        shutdown(socket->fd(), SHUT_WR);

        char buffer[4096];
        while (true) {
            const ssize_t received = recv(socket->fd(), buffer, sizeof(buffer), 0);
            if (received == 0) {
                break;
            }
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "recv failed: " << std::strerror(errno) << "\n";
                return 1;
            }
            std::cout.write(buffer, received);
        }
        return 0;
    }

    if (argc != 9 && argc != 11) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string limit = argv[1];
    const std::string log_path = argv[2];
    const std::string store_path = argv[3];
    const std::string sender_wallet_path = argv[4];
    const std::string receiver_address = argv[5];
    const std::string prime = argv[6];
    const std::string amount = argv[7];
    const std::string target_integer = argv[8];
    std::string composite_miner = "pcdev1_composite_miner";

    if (argc == 11) {
        if (std::string(argv[9]) != "--composite-miner") {
            printUsage(argv[0]);
            return 1;
        }
        composite_miner = argv[10];
    }

    DevWallet sender;
    if (!loadWallet(sender_wallet_path, sender)) {
        std::cerr << "could not load sender wallet address\n";
        return 1;
    }
    if (!primechain::protocol::isDevelopmentAddress(receiver_address) ||
        !primechain::protocol::isDevelopmentAddress(composite_miner)) {
        std::cerr << "invalid development address\n";
        return 1;
    }

    const std::string sequential = siblingExecutable(argv[0], "primechain-sequential");
    std::vector<std::string> args{
        sequential,
        limit,
        log_path,
        store_path,
        "--prime-miner",
        sender.address,
        "--composite-miner",
        composite_miner,
        "--transfer",
        sender_wallet_path,
        receiver_address,
        prime,
        amount,
        target_integer,
    };

    std::vector<char*> exec_args;
    exec_args.reserve(args.size() + 1);
    for (auto& arg : args) {
        exec_args.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_args.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "could not fork\n";
        return 1;
    }
    if (child == 0) {
        execv(sequential.c_str(), exec_args.data());
        std::cerr << "could not execute " << sequential << "\n";
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::cerr << "could not wait for primechain-sequential\n";
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    return 0;
}
