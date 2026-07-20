#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "primechain/core/consensus.hpp"
#include "primechain/crypto/hash.hpp"
#include "primechain/crypto/signature.hpp"
#include "primechain/wallet/miner_identity.hpp"

namespace {

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " g d e nonce provider_address\n"
              << "  " << argv0 << " sign-commit <miner-identity> g d e nonce\n"
              << "  " << argv0 << " sign-reveal <miner-identity> g d e nonce\n"
              << "  " << argv0 << " sign-phase <validator-identity> g snapshot_hash\n"
              << "  " << argv0 << " sign-epoch <validator-identity> previous_hash record_integer epoch next_validator...\n"
              << "  " << argv0 << " sign-endpoint <validator-identity> previous_hash record_integer host port sequence [effective_integer]\n"
              << "  " << argv0 << " sign-policy <validator-identity> previous_hash record_integer transfer_fee_micro_units sequence [effective_integer]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) != "--help") {
        const auto g = static_cast<primechain::PrimeValue>(std::stoull(argv[1]));
        const auto d = static_cast<primechain::PrimeValue>(std::stoull(argv[2]));
        const auto e = static_cast<primechain::PrimeValue>(std::stoull(argv[3]));
        const auto nonce = static_cast<std::uint64_t>(std::stoull(argv[4]));
        const std::string provider = argv[5];
        std::cout << primechain::crypto::toHex(
            primechain::crypto::compositeCommitment(g, d, e, nonce, provider))
                  << "\n";
        return 0;
    }

    if ((argc == 8 || argc == 9) && std::string(argv[1]) == "sign-endpoint") {
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const auto previous_bytes = primechain::wallet::hexToBytes(argv[3]);
        if (previous_bytes.size() != 32) {
            std::cerr << "invalid previous record hash\n";
            return 1;
        }
        primechain::Hash256 previous_hash{};
        std::copy(previous_bytes.begin(), previous_bytes.end(), previous_hash.begin());
        const auto record_integer = static_cast<primechain::PrimeValue>(std::stoull(argv[4]));
        const std::string host = argv[5];
        const auto port = static_cast<std::uint64_t>(std::stoull(argv[6]));
        const auto sequence = static_cast<std::uint64_t>(std::stoull(argv[7]));
        const auto effective_integer = argc == 9
            ? static_cast<primechain::PrimeValue>(std::stoull(argv[8]))
            : record_integer;
        const auto signature = primechain::crypto::signProtocolMessage(
            identity.private_key,
            primechain::crypto::validatorEndpointSigningPayload(
                previous_hash, record_integer, identity.address, host, port,
                effective_integer, sequence),
            error);
        if (!signature.has_value()) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "SUBMIT_VALIDATOR_ENDPOINT "
                  << primechain::crypto::toHex(previous_hash) << " "
                  << record_integer << " " << identity.address << " "
                  << host << " " << port << " " << effective_integer << " "
                  << sequence << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
        return 0;
    }

    if ((argc == 7 || argc == 8) && std::string(argv[1]) == "sign-policy") {
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const auto previous_bytes = primechain::wallet::hexToBytes(argv[3]);
        if (previous_bytes.size() != 32) {
            std::cerr << "invalid previous record hash\n";
            return 1;
        }
        primechain::Hash256 previous_hash{};
        std::copy(previous_bytes.begin(), previous_bytes.end(), previous_hash.begin());
        const auto record_integer = static_cast<primechain::PrimeValue>(std::stoull(argv[4]));
        const auto transfer_fee_micro_units = static_cast<std::uint64_t>(std::stoull(argv[5]));
        const auto sequence = static_cast<std::uint64_t>(std::stoull(argv[6]));
        const auto effective_integer = argc == 8
            ? static_cast<primechain::PrimeValue>(std::stoull(argv[7]))
            : record_integer + 1;
        const auto signature = primechain::crypto::signProtocolMessage(
            identity.private_key,
            primechain::crypto::economicPolicySigningPayload(
                previous_hash, record_integer, transfer_fee_micro_units,
                effective_integer, sequence, identity.address),
            error);
        if (!signature.has_value()) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "SUBMIT_POLICY_VOTE "
                  << primechain::crypto::toHex(previous_hash) << " "
                  << record_integer << " " << transfer_fee_micro_units << " "
                  << effective_integer << " " << sequence << " "
                  << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
        return 0;
    }

    if (argc >= 7 && std::string(argv[1]) == "sign-epoch") {
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const auto previous_bytes = primechain::wallet::hexToBytes(argv[3]);
        if (previous_bytes.size() != 32) {
            std::cerr << "invalid previous record hash\n";
            return 1;
        }
        primechain::Hash256 previous_hash{};
        std::copy(previous_bytes.begin(), previous_bytes.end(), previous_hash.begin());
        const auto record_integer = static_cast<primechain::PrimeValue>(std::stoull(argv[4]));
        const auto epoch = static_cast<std::uint64_t>(std::stoull(argv[5]));
        std::vector<primechain::Address> next_set;
        for (int i = 6; i < argc; ++i) next_set.push_back(argv[i]);
        std::sort(next_set.begin(), next_set.end());
        if (!primechain::core::validValidatorSetSize(next_set.size()) ||
            std::adjacent_find(next_set.begin(), next_set.end()) != next_set.end() ||
            !std::all_of(next_set.begin(), next_set.end(), primechain::crypto::isProtocolSignatureAddress)) {
            std::cerr << "next validator set must contain distinct pcpq1_ addresses\n";
            return 1;
        }
        const auto activation_integer = record_integer + 1;
        const auto signature = primechain::crypto::signProtocolMessage(
            identity.private_key,
            primechain::crypto::validatorEpochVoteSigningPayload(
                previous_hash, record_integer, epoch, activation_integer, next_set, identity.address),
            error);
        if (!signature.has_value()) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "SUBMIT_EPOCH_VOTE "
                  << primechain::crypto::toHex(previous_hash) << " "
                  << record_integer << " " << epoch << " " << activation_integer;
        for (const auto& validator : next_set) std::cout << " " << validator;
        std::cout << " "
                  << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
        return 0;
    }

    if (argc == 5 && std::string(argv[1]) == "sign-phase") {
        primechain::wallet::MinerIdentity identity;
        std::string error;
        if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const auto g = static_cast<primechain::PrimeValue>(std::stoull(argv[3]));
        const auto snapshot_bytes = primechain::wallet::hexToBytes(argv[4]);
        if (snapshot_bytes.size() != 32) {
            std::cerr << "invalid snapshot hash\n";
            return 1;
        }
        primechain::Hash256 snapshot{};
        std::copy(snapshot_bytes.begin(), snapshot_bytes.end(), snapshot.begin());
        const auto signature = primechain::crypto::signProtocolMessage(
            identity.private_key,
            primechain::crypto::commitPhaseVoteSigningPayload(g, snapshot, identity.address),
            error);
        if (!signature.has_value()) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "SUBMIT_PHASE_VOTE " << g << " " << argv[4] << " "
                  << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
        return 0;
    }

    if (argc != 7 ||
        (std::string(argv[1]) != "sign-commit" && std::string(argv[1]) != "sign-reveal")) {
        printUsage(argv[0]);
        return 1;
    }

    primechain::wallet::MinerIdentity identity;
    std::string error;
    if (!primechain::wallet::loadMinerIdentity(argv[2], identity, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto g = static_cast<primechain::PrimeValue>(std::stoull(argv[3]));
    const auto d = static_cast<primechain::PrimeValue>(std::stoull(argv[4]));
    const auto e = static_cast<primechain::PrimeValue>(std::stoull(argv[5]));
    const auto nonce = static_cast<std::uint64_t>(std::stoull(argv[6]));

    primechain::crypto::Bytes payload;
    primechain::Hash256 commitment{};
    if (std::string(argv[1]) == "sign-commit") {
        commitment = primechain::crypto::compositeCommitment(
            g, d, e, nonce, identity.address);
        payload = primechain::crypto::compositeCommitSigningPayload(
            g, commitment, identity.address);
    } else {
        payload = primechain::crypto::compositeRevealSigningPayload(
            g, d, e, nonce, identity.address);
    }
    const auto signature = primechain::crypto::signProtocolMessage(identity.private_key, payload, error);
    if (!signature.has_value()) {
        std::cerr << error << "\n";
        return 1;
    }

    if (std::string(argv[1]) == "sign-commit") {
        std::cout << "SUBMIT_SIGNED_COMMIT " << g << " "
                  << primechain::crypto::toHex(commitment) << " "
                  << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
    } else {
        std::cout << "SUBMIT_SIGNED_REVEAL " << g << " " << d << " " << e << " "
                  << nonce << " " << identity.address << " "
                  << primechain::wallet::bytesToHex(identity.public_key) << " "
                  << primechain::wallet::bytesToHex(*signature) << "\n";
    }
    return 0;
}
