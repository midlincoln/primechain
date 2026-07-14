#include "primechain/wallet/miner_identity.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

namespace primechain::wallet {
namespace {

constexpr const char* kPlainWalletVersion = "pc-miner-mldsa65-v2";
constexpr const char* kEncryptedWalletVersion = "pc-miner-mldsa65-v3";
constexpr const char* kPassphraseEnv = "PRIMECHAIN_WALLET_PASSPHRASE";
constexpr std::uint64_t kScryptN = 32768;
constexpr std::uint64_t kScryptR = 8;
constexpr std::uint64_t kScryptP = 1;
constexpr std::uint64_t kScryptMaxMemory = 64ull * 1024ull * 1024ull;
constexpr std::size_t kSaltBytes = 32;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kGcmTagBytes = 16;

using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

std::map<std::string, std::string> readKeyValues(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "could not open miner identity file";
        return {};
    }
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(in, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return values;
}

std::optional<std::string> valueFor(
    const std::map<std::string, std::string>& values,
    const std::string& key) {
    const auto found = values.find(key);
    if (found == values.end()) return std::nullopt;
    return found->second;
}

bool randomBytes(crypto::Bytes& out, std::size_t size, std::string& error) {
    out.assign(size, 0);
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        error = "secure random generation failed";
        return false;
    }
    return true;
}

std::optional<std::string> walletPassphrase(std::string& error) {
    const char* env = std::getenv(kPassphraseEnv);
    if (env != nullptr && env[0] != '\0') return std::string(env);

    if (isatty(STDIN_FILENO)) {
        char* entered = getpass("Primechain wallet passphrase: ");
        if (entered != nullptr && entered[0] != '\0') return std::string(entered);
    }

    error = std::string("wallet passphrase required; set ") + kPassphraseEnv;
    return std::nullopt;
}

bool deriveWalletKey(
    const std::string& passphrase,
    const crypto::Bytes& salt,
    std::uint64_t n,
    std::uint64_t r,
    std::uint64_t p,
    crypto::Bytes& key,
    std::string& error) {
    key.assign(kKeyBytes, 0);
    if (EVP_PBE_scrypt(
            passphrase.data(), passphrase.size(),
            salt.data(), salt.size(),
            n, r, p, kScryptMaxMemory,
            key.data(), key.size()) != 1) {
        error = "wallet key derivation failed";
        return false;
    }
    return true;
}

std::string encryptionAad(
    const Address& address,
    const crypto::Bytes& public_key,
    const crypto::Bytes& salt,
    const crypto::Bytes& nonce,
    std::uint64_t n,
    std::uint64_t r,
    std::uint64_t p) {
    std::ostringstream out;
    out << "version=" << kEncryptedWalletVersion << "\n";
    out << "algorithm=" << crypto::signatureAlgorithmName(crypto::kProtocolSignatureAlgorithm) << "\n";
    out << "address=" << address << "\n";
    out << "public_key=" << bytesToHex(public_key) << "\n";
    out << "encryption=aes-256-gcm\n";
    out << "kdf=scrypt\n";
    out << "scrypt_n=" << n << "\n";
    out << "scrypt_r=" << r << "\n";
    out << "scrypt_p=" << p << "\n";
    out << "salt=" << bytesToHex(salt) << "\n";
    out << "nonce=" << bytesToHex(nonce) << "\n";
    return out.str();
}

bool encryptPrivateKey(
    const crypto::Bytes& private_key,
    const std::string& passphrase,
    const Address& address,
    const crypto::Bytes& public_key,
    crypto::Bytes& salt,
    crypto::Bytes& nonce,
    crypto::Bytes& ciphertext,
    crypto::Bytes& tag,
    std::string& error) {
    if (!randomBytes(salt, kSaltBytes, error) || !randomBytes(nonce, kNonceBytes, error)) return false;

    crypto::Bytes key;
    if (!deriveWalletKey(passphrase, salt, kScryptN, kScryptR, kScryptP, key, error)) return false;

    const std::string plaintext = bytesToHex(private_key);
    const std::string aad_text = encryptionAad(address, public_key, salt, nonce, kScryptN, kScryptR, kScryptP);
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        error = "wallet encryption initialization failed";
        return false;
    }

    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len,
            reinterpret_cast<const unsigned char*>(aad_text.data()),
            static_cast<int>(aad_text.size())) != 1) {
        error = "wallet encryption aad failed";
        return false;
    }

    ciphertext.assign(plaintext.size(), 0);
    int out_len = 0;
    if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &out_len,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            static_cast<int>(plaintext.size())) != 1) {
        error = "wallet encryption failed";
        return false;
    }
    int total = out_len;
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + total, &out_len) != 1) {
        error = "wallet encryption finalization failed";
        return false;
    }
    total += out_len;
    ciphertext.resize(static_cast<std::size_t>(total));

    tag.assign(kGcmTagBytes, 0);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) {
        error = "wallet encryption tag failed";
        return false;
    }
    return true;
}

bool decryptPrivateKey(
    const std::map<std::string, std::string>& values,
    const std::string& passphrase,
    const Address& address,
    const crypto::Bytes& public_key,
    crypto::Bytes& private_key,
    std::string& error) {
    const auto salt = hexToBytes(valueFor(values, "salt").value_or(""));
    const auto nonce = hexToBytes(valueFor(values, "nonce").value_or(""));
    const auto ciphertext = hexToBytes(valueFor(values, "ciphertext").value_or(""));
    const auto tag = hexToBytes(valueFor(values, "tag").value_or(""));
    const auto n_text = valueFor(values, "scrypt_n").value_or("0");
    const auto r_text = valueFor(values, "scrypt_r").value_or("0");
    const auto p_text = valueFor(values, "scrypt_p").value_or("0");
    if (salt.size() != kSaltBytes || nonce.size() != kNonceBytes || tag.size() != kGcmTagBytes || ciphertext.empty()) {
        error = "invalid encrypted miner identity file";
        return false;
    }

    const auto n = static_cast<std::uint64_t>(std::stoull(n_text));
    const auto r = static_cast<std::uint64_t>(std::stoull(r_text));
    const auto p = static_cast<std::uint64_t>(std::stoull(p_text));

    crypto::Bytes key;
    if (!deriveWalletKey(passphrase, salt, n, r, p, key, error)) return false;

    const std::string aad_text = encryptionAad(address, public_key, salt, nonce, n, r, p);
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        error = "wallet decryption initialization failed";
        return false;
    }

    int len = 0;
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len,
            reinterpret_cast<const unsigned char*>(aad_text.data()),
            static_cast<int>(aad_text.size())) != 1) {
        error = "wallet decryption aad failed";
        return false;
    }

    crypto::Bytes plaintext(ciphertext.size(), 0);
    int out_len = 0;
    if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len,
            ciphertext.data(), static_cast<int>(ciphertext.size())) != 1) {
        error = "wallet decryption failed";
        return false;
    }
    int total = out_len;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), const_cast<std::uint8_t*>(tag.data())) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + total, &out_len) != 1) {
        error = "wallet passphrase or authentication failed";
        return false;
    }
    total += out_len;
    plaintext.resize(static_cast<std::size_t>(total));

    private_key = hexToBytes(std::string(plaintext.begin(), plaintext.end()));
    return true;
}

bool validateMinerIdentity(const MinerIdentity& identity, std::string& error) {
    if (identity.private_key.size() != crypto::signaturePrivateKeySize(crypto::kProtocolSignatureAlgorithm) ||
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

} // namespace

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
    if (identity.address != crypto::addressFromProtocolPublicKey(identity.public_key)) {
        error = "miner identity address/public key mismatch";
        return false;
    }
    const auto passphrase = walletPassphrase(error);
    if (!passphrase.has_value()) return false;

    crypto::Bytes salt;
    crypto::Bytes nonce;
    crypto::Bytes ciphertext;
    crypto::Bytes tag;
    if (!encryptPrivateKey(identity.private_key, *passphrase, identity.address, identity.public_key,
            salt, nonce, ciphertext, tag, error)) {
        return false;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not open miner identity file";
        return false;
    }
    out << "version=" << kEncryptedWalletVersion << "\n";
    out << "algorithm=" << crypto::signatureAlgorithmName(crypto::kProtocolSignatureAlgorithm) << "\n";
    out << "address=" << identity.address << "\n";
    out << "public_key=" << bytesToHex(identity.public_key) << "\n";
    out << "encryption=aes-256-gcm\n";
    out << "kdf=scrypt\n";
    out << "scrypt_n=" << kScryptN << "\n";
    out << "scrypt_r=" << kScryptR << "\n";
    out << "scrypt_p=" << kScryptP << "\n";
    out << "salt=" << bytesToHex(salt) << "\n";
    out << "nonce=" << bytesToHex(nonce) << "\n";
    out << "ciphertext=" << bytesToHex(ciphertext) << "\n";
    out << "tag=" << bytesToHex(tag) << "\n";
    if (!out) {
        error = "could not write miner identity file";
        return false;
    }
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

bool loadMinerIdentity(const std::string& path, MinerIdentity& identity, std::string& error) {
    const auto values = readKeyValues(path, error);
    if (!error.empty()) return false;

    const auto version = valueFor(values, "version").value_or("");
    identity = MinerIdentity{};
    identity.address = valueFor(values, "address").value_or("");
    identity.public_key = hexToBytes(valueFor(values, "public_key").value_or(""));

    if (version == kPlainWalletVersion) {
        identity.private_key = hexToBytes(valueFor(values, "private_key").value_or(""));
        return validateMinerIdentity(identity, error);
    }

    if (version != kEncryptedWalletVersion ||
        valueFor(values, "encryption").value_or("") != "aes-256-gcm" ||
        valueFor(values, "kdf").value_or("") != "scrypt" ||
        valueFor(values, "algorithm").value_or("") != crypto::signatureAlgorithmName(crypto::kProtocolSignatureAlgorithm)) {
        error = "invalid miner identity file";
        return false;
    }

    if (identity.address != crypto::addressFromProtocolPublicKey(identity.public_key)) {
        error = "invalid miner identity address";
        return false;
    }

    const auto passphrase = walletPassphrase(error);
    if (!passphrase.has_value()) return false;
    if (!decryptPrivateKey(values, *passphrase, identity.address, identity.public_key, identity.private_key, error)) {
        return false;
    }
    return validateMinerIdentity(identity, error);
}

bool loadMinerIdentityAddress(const std::string& path, Address& address, std::string& error) {
    const auto values = readKeyValues(path, error);
    if (!error.empty()) return false;
    const auto version = valueFor(values, "version").value_or("");
    const auto public_key = hexToBytes(valueFor(values, "public_key").value_or(""));
    address = valueFor(values, "address").value_or("");
    if ((version != kPlainWalletVersion && version != kEncryptedWalletVersion) ||
        public_key.size() != crypto::signaturePublicKeySize(crypto::kProtocolSignatureAlgorithm) ||
        address != crypto::addressFromProtocolPublicKey(public_key)) {
        error = "invalid miner identity file";
        return false;
    }
    return true;
}

} // namespace primechain::wallet
