#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "primechain/crypto/hash.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/wallet/miner_identity.hpp"
#include "primechain/protocol/records.hpp"

namespace {

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
        return out;
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

std::vector<std::uint8_t> randomBytes(std::size_t size) {
    std::vector<std::uint8_t> out(size);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.read(reinterpret_cast<char*>(out.data()), out.size())) {
        return out;
    }

    const std::vector<std::uint8_t> seed{'p', 'r', 'i', 'm', 'e', 'c', 'h', 'a', 'i', 'n'};
    const auto fallback = primechain::crypto::sha3_256(seed);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = fallback[i % fallback.size()];
    }
    return out;
}

struct DevWallet {
    std::string address;
    std::vector<std::uint8_t> private_key;
    std::vector<std::uint8_t> public_key;
};

bool saveWallet(const std::string& path, const DevWallet& wallet) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "version=pcdev-wallet-v0\n";
    out << "address=" << wallet.address << "\n";
    out << "private_key=" << bytesToHex(wallet.private_key) << "\n";
    out << "public_key=" << bytesToHex(wallet.public_key) << "\n";
    return static_cast<bool>(out);
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
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "address") {
            wallet.address = value;
        } else if (key == "private_key") {
            wallet.private_key = hexToBytes(value);
        } else if (key == "public_key") {
            wallet.public_key = hexToBytes(value);
        }
    }
    return !wallet.address.empty() &&
           !wallet.public_key.empty() &&
           wallet.address == primechain::protocol::developmentAddressFromPublicKey(wallet.public_key);
}

DevWallet createWallet() {
    DevWallet wallet;
    wallet.private_key = randomBytes(32);
    const auto public_hash = primechain::crypto::sha3_256(wallet.private_key);
    wallet.public_key.assign(public_hash.begin(), public_hash.end());
    wallet.address = primechain::protocol::developmentAddressFromPublicKey(wallet.public_key);
    return wallet;
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " new <development-wallet-file>\n"
              << "  " << argv0 << " new-miner <ml-dsa-65-wallet-file>\n"
              << "  " << argv0 << " address <wallet-file>\n"
              << "  " << argv0 << " miner-address <ml-dsa-65-wallet-file>\n"
              << "  " << argv0 << " balance <record-store.dat> <wallet-file>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];
    if (command == "new") {
        const auto wallet = createWallet();
        if (!saveWallet(argv[2], wallet)) {
            std::cerr << "could not write wallet file\n";
            return 1;
        }
        std::cout << wallet.address << "\n";
        return 0;
    }

    if (command == "new-miner") {
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::createMinerIdentity(identity, error) ||
            !primechain::wallet::saveMinerIdentity(argv[2], identity, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << identity.address << "\n";
        return 0;
    }

    if (command == "miner-address") {
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << identity.address << "\n";
        return 0;
    }

    if (command == "address") {
        DevWallet wallet;
        if (loadWallet(argv[2], wallet)) {
            std::cout << wallet.address << "\n";
            return 0;
        }
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
            std::cerr << "could not load wallet\n";
            return 1;
        }
        std::cout << identity.address << "\n";
        return 0;
    }

    if (command == "balance") {
        if (argc != 4) {
            printUsage(argv[0]);
            return 1;
        }
        primechain::Address wallet_address;
        DevWallet wallet;
        if (loadWallet(argv[3], wallet)) {
            wallet_address = wallet.address;
        } else {
            primechain::wallet::MinerIdentity identity;
            std::string identity_error;
            if (!primechain::wallet::loadMinerIdentity(argv[3], identity, identity_error)) {
                std::cerr << "could not load wallet\n";
                return 1;
            }
            wallet_address = identity.address;
        }
        primechain::node::SequentialNode node(argv[2]);
        std::string error;
        if (!node.load(error)) {
            std::cerr << "could not load record store: " << error << "\n";
            return 1;
        }
        const auto holdings = node.holdingsForAddress(wallet_address);
        std::cout << "address: " << wallet_address << "\n";
        std::cout << "holdings: " << holdings.size() << "\n";
        for (const auto& holding : holdings) {
            std::cout << holding.first << " " << holding.second << "\n";
        }
        return 0;
    }

    printUsage(argv[0]);
    return 1;
}
