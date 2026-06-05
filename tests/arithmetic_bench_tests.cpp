#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: arithmetic-bench-tests <record-log-path>\n";
        return 1;
    }

    std::ifstream in(argv[1]);
    if (!expect(static_cast<bool>(in), "open benchmark output")) {
        return 1;
    }

    std::string line;
    int line_count = 0;
    bool saw_prime = false;
    bool saw_composite = false;
    bool saw_tx_count = false;

    while (std::getline(in, line)) {
        ++line_count;
        if (line.rfind("PRIME ", 0) == 0) {
            saw_prime = true;
        }
        if (line.rfind("COMPOSITE ", 0) == 0) {
            saw_composite = true;
        }
        if (line.find("tx_count=7") != std::string::npos) {
            saw_tx_count = true;
        }
    }

    if (!expect(line_count == 20, "expected record count")) {
        return 1;
    }
    if (!expect(saw_prime, "contains prime records")) {
        return 1;
    }
    if (!expect(saw_composite, "contains composite records")) {
        return 1;
    }
    if (!expect(saw_tx_count, "contains synthetic transaction count")) {
        return 1;
    }

    std::cout << "arithmetic benchmark tests passed\n";
    return 0;
}
