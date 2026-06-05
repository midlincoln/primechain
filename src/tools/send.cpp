#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "primechain/protocol/records.hpp"

namespace {

struct DevWallet {
    std::string address;
};

bool loadWalletAddress(const std::string& path, DevWallet& wallet) {
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
        }
    }
    return primechain::protocol::isDevelopmentAddress(wallet.address);
}

std::string siblingExecutable(const char* argv0, const std::string& executable_name) {
    const std::string self = argv0;
    const auto slash = self.find_last_of('/');
    if (slash == std::string::npos) {
        return executable_name;
    }
    return self.substr(0, slash + 1) + executable_name;
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " <limit> <text_log_path> <record_store_path> <sender.wallet> <receiver_address> <prime> <amount> <target_integer> [--composite-miner address]\n"
              << "example:\n"
              << "  " << argv0 << " 20 ./data/tx.log ./data/tx.dat ./wallets/miner.wallet pcdev1_alice 3 250000 4\n";
}

} // namespace

int main(int argc, char** argv) {
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
    if (!loadWalletAddress(sender_wallet_path, sender)) {
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
