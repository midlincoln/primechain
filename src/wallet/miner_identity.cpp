#include "primechain/wallet/miner_identity.hpp"

#include <fstream>

namespace primechain::wallet {

std::string bytesToHex(const crypto::Bytes& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

crypto::Bytes hexToBytes(const std::string& hex) {
    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    };
    if (hex.size() % 2 != 0) {
        return {};
    }
    crypto::Bytes out;
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

bool createMinerIdentity(MinerIdentity& identity, std::string& error) {
    const auto pair = crypto::generateProtocolSignatureKeyPair(error);
    if (!pair.has_value()) {
        return false;
    }
    identity.private_key = pair->private_key;
    identity.public_key = pair->public_key;
    identity.address = crypto::addressFromProtocolPublicKey(identity.public_key);
    return true;
}

bool saveMinerIdentity(const std::string& path, const MinerIdentity& identity, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not open miner identity file";
        return false;
    }
    out << "version=pc-miner-mldsa65-v2\n";
    out << "algorithm=" << crypto::signatureAlgorithmName(crypto::kProtocolSignatureAlgorithm) << "\n";
    out << "address=" << identity.address << "\n";
    out << "private_key=" << bytesToHex(identity.private_key) << "\n";
    out << "public_key=" << bytesToHex(identity.public_key) << "\n";
    if (!out) {
        error = "could not write miner identity file";
        return false;
    }
    return true;
}

bool loadMinerIdentity(const std::string& path, MinerIdentity& identity, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "could not open miner identity file";
        return false;
    }
    std::string version;
    std::string line;
    while (std::getline(in, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "version") version = value;
        else if (key == "address") identity.address = value;
        else if (key == "private_key") identity.private_key = hexToBytes(value);
        else if (key == "public_key") identity.public_key = hexToBytes(value);
    }
    if (version != "pc-miner-mldsa65-v2" ||
        identity.private_key.size() != crypto::signaturePrivateKeySize(crypto::kProtocolSignatureAlgorithm) ||
        identity.public_key.size() != crypto::signaturePublicKeySize(crypto::kProtocolSignatureAlgorithm) ||
        identity.address != crypto::addressFromProtocolPublicKey(identity.public_key)) {
        error = "invalid miner identity file";
        return false;
    }
    const crypto::Bytes challenge{'p', 'r', 'i', 'm', 'e', 'c', 'h', 'a', 'i', 'n'};
    const auto signature = crypto::signProtocolMessage(identity.private_key, challenge, error);
    if (!signature.has_value() ||
        !crypto::verifyProtocolMessageSignature(identity.public_key, challenge, *signature, error)) {
        error = "miner identity private/public key mismatch";
        return false;
    }
    return true;
}

} // namespace primechain::wallet
