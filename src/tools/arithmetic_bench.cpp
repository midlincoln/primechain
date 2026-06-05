#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/stat.h>

#include "primechain/math/number_theory.hpp"
#include "primechain/types.hpp"

namespace {

constexpr const char* kDefaultOutputPath = "data/arithmetic-records.log";

bool ensureParentDataDir(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }
    const std::string dir = path.substr(0, slash);
    if (dir.empty()) {
        return true;
    }
    if (mkdir(dir.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

std::string recordLine(
    primechain::PrimeValue n,
    const primechain::Address& provider,
    std::uint64_t tx_per_record) {
    std::ostringstream out;
    if (primechain::math::isPrime(n)) {
        out << "PRIME " << n
            << " provider=" << provider
            << " tx_count=" << tx_per_record
            << " tx_root=synthetic-" << n << "-" << tx_per_record;
        return out.str();
    }

    const auto proof = primechain::math::makeCompositeProof(n, provider);
    if (!proof.has_value()) {
        return {};
    }

    out << "COMPOSITE " << proof->m
        << " d=" << proof->d
        << " e=" << proof->e
        << " provider=" << proof->provider_address
        << " tx_count=" << tx_per_record
        << " tx_root=synthetic-" << n << "-" << tx_per_record;
    return out.str();
}

std::uint64_t fileSize(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(st.st_size);
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [records] [tx_per_record] [start_integer] [output_path]\n"
              << "\n"
              << "example:\n"
              << "  " << argv0 << " 100000 100 3 ./bench-data/arithmetic.log\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const std::uint64_t records = argc > 1 ? std::stoull(argv[1]) : 100000;
    const std::uint64_t tx_per_record = argc > 2 ? std::stoull(argv[2]) : 100;
    const primechain::PrimeValue start = argc > 3 ? std::stoull(argv[3]) : 3;
    const std::string output_path = argc > 4 ? argv[4] : kDefaultOutputPath;

    if (records == 0 || start < 2) {
        printUsage(argv[0]);
        return 1;
    }
    if (!ensureParentDataDir(output_path)) {
        std::cerr << "could not create parent directory for " << output_path << "\n";
        return 1;
    }

    std::ofstream out(output_path, std::ios::trunc);
    if (!out) {
        std::cerr << "could not open " << output_path << " for writing\n";
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t prime_records = 0;
    std::uint64_t composite_records = 0;
    primechain::PrimeValue n = start;

    for (std::uint64_t i = 0; i < records; ++i, ++n) {
        const bool prime = primechain::math::isPrime(n);
        const std::string line = recordLine(n, "bench-miner", tx_per_record);
        if (line.empty()) {
            std::cerr << "could not classify integer " << n << "\n";
            return 1;
        }
        out << line << "\n";
        if (!out) {
            std::cerr << "failed while writing " << output_path << "\n";
            return 1;
        }

        if (prime) {
            ++prime_records;
        } else {
            ++composite_records;
        }
    }
    out.close();

    const auto finished = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = finished - started;
    const double seconds = elapsed.count() > 0.0 ? elapsed.count() : 0.000001;
    const std::uint64_t synthetic_txs = records * tx_per_record;
    const std::uint64_t bytes = fileSize(output_path);

    std::cout << "arithmetic benchmark complete\n";
    std::cout << "output_path: " << output_path << "\n";
    std::cout << "start_integer: " << start << "\n";
    std::cout << "end_integer: " << (n - 1) << "\n";
    std::cout << "records_written: " << records << "\n";
    std::cout << "prime_records: " << prime_records << "\n";
    std::cout << "composite_records: " << composite_records << "\n";
    std::cout << "synthetic_transactions: " << synthetic_txs << "\n";
    std::cout << "elapsed_seconds: " << std::fixed << std::setprecision(6) << seconds << "\n";
    std::cout << "records_per_second: " << std::fixed << std::setprecision(2)
              << static_cast<double>(records) / seconds << "\n";
    std::cout << "transactions_per_second: " << std::fixed << std::setprecision(2)
              << static_cast<double>(synthetic_txs) / seconds << "\n";
    std::cout << "log_bytes: " << bytes << "\n";
    std::cout << "bytes_per_record: " << std::fixed << std::setprecision(2)
              << static_cast<double>(bytes) / static_cast<double>(records) << "\n";

    return 0;
}
