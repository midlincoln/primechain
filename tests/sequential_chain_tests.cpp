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
        std::cerr << "usage: sequential-chain-tests <log-path>\n";
        return 1;
    }

    std::ifstream in(argv[1]);
    if (!expect(static_cast<bool>(in), "open sequential chain log")) {
        return 1;
    }

    std::string line;
    int count = 0;
    bool saw_11_pratt = false;
    bool saw_12_factorization = false;
    bool saw_500_factorization = false;

    while (std::getline(in, line)) {
        ++count;
        if (line.find("11 PRIME witness=2 factors_p_minus_1=2^1*5^1") != std::string::npos) {
            saw_11_pratt = true;
        }
        if (line.find("12 COMPOSITE") != std::string::npos &&
            line.find("full_factorization=2^2*3^1") != std::string::npos) {
            saw_12_factorization = true;
        }
        if (line.find("500 COMPOSITE") != std::string::npos &&
            line.find("full_factorization=2^2*5^3") != std::string::npos) {
            saw_500_factorization = true;
        }
    }

    if (!expect(count == 499, "records 2 through 500")) {
        return 1;
    }
    if (!expect(saw_11_pratt, "Pratt proof line for 11")) {
        return 1;
    }
    if (!expect(saw_12_factorization, "full factorization line for 12")) {
        return 1;
    }
    if (!expect(saw_500_factorization, "full factorization line for 500")) {
        return 1;
    }

    std::cout << "sequential chain tests passed\n";
    return 0;
}
