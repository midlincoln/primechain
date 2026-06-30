#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

std::string directoryName(const std::string& path) {
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) return ".";
    if (separator == 0) return "/";
    return path.substr(0, separator);
}

int runTool(const std::string& argv0, const std::string& tool, std::vector<std::string> args) {
    const std::string path = directoryName(argv0) + "/" + tool;
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(path);
    for (auto& arg : args) owned.push_back(std::move(arg));

    std::vector<char*> raw;
    raw.reserve(owned.size() + 1);
    for (auto& arg : owned) raw.push_back(&arg[0]);
    raw.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "could not start " << tool << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0) {
        execv(path.c_str(), raw.data());
        std::cerr << "could not execute " << path << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        std::cerr << "could not wait for " << tool << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        std::cerr << tool << " terminated by signal " << WTERMSIG(status) << "\n";
    }
    return 1;
}

std::vector<std::string> tail(int argc, char** argv, int first) {
    std::vector<std::string> out;
    for (int i = first; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void printUsage(const char* argv0) {
    std::cerr << "usage:\n"
              << "  " << argv0 << " status <host> <port>\n"
              << "  " << argv0 << " query <host> <port> <command...>\n"
              << "  " << argv0 << " sync <host> <port> <start> <end> <output-store>\n"
              << "  " << argv0 << " inspect <record-store> [integer]\n"
              << "  " << argv0 << " inspect <record-store> --range <start> <end>\n"
              << "  " << argv0 << " new-miner <wallet-file>\n"
              << "  " << argv0 << " address <wallet-file>\n"
              << "  " << argv0 << " balance <record-store> <wallet-file>\n"
              << "  " << argv0 << " mine <host> <port> <limit> --prime-identity <file> --composite-identity <file>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    const std::string command = argv[1];
    if (command == "status") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-sync-query", {argv[2], argv[3], "GET_STATUS"});
    }
    if (command == "query") {
        if (argc < 5) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-sync-query", tail(argc, argv, 2));
    }
    if (command == "sync") {
        if (argc != 7) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-sync-download", tail(argc, argv, 2));
    }
    if (command == "inspect") {
        if (argc < 3) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-store-inspect", tail(argc, argv, 2));
    }
    if (command == "new-miner") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-wallet", {"new-miner", argv[2]});
    }
    if (command == "address") {
        if (argc != 3) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-wallet", {"address", argv[2]});
    }
    if (command == "balance") {
        if (argc != 4) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-wallet", {"balance", argv[2], argv[3]});
    }
    if (command == "mine") {
        if (argc != 9) { printUsage(argv[0]); return 1; }
        return runTool(argv[0], "primechain-frontier-miner", tail(argc, argv, 2));
    }

    std::cerr << "unknown client command: " << command << "\n";
    printUsage(argv[0]);
    return 1;
}
