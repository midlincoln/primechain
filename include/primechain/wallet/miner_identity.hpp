#pragma once

#include <string>

#include "primechain/crypto/signature.hpp"

namespace primechain::wallet {

struct MinerIdentity {
    Address address;
    crypto::Bytes private_key;
    crypto::Bytes public_key;
};

bool createMinerIdentity(MinerIdentity& identity, std::string& error);
bool saveMinerIdentity(const std::string& path, const MinerIdentity& identity, std::string& error);
bool loadMinerIdentity(const std::string& path, MinerIdentity& identity, std::string& error);
bool loadMinerIdentityAddress(const std::string& path, Address& address, std::string& error);
std::string bytesToHex(const crypto::Bytes& bytes);
crypto::Bytes hexToBytes(const std::string& hex);

} // namespace primechain::wallet
