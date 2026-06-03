#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

constexpr long double kSecondsPerJulianYear = 365.25L * 24.0L * 60.0L * 60.0L;

long double nthPrimeEstimate(long double n) {
    if (n < 2.0L) {
        return 2.0L;
    }

    const long double log_n = std::log(n);
    const long double log_log_n = std::log(log_n);

    if (n >= 6.0L) {
        return n * (log_n + log_log_n - 1.0L);
    }
    return n * log_n;
}

void printUsage(const char* executable) {
    std::cerr << "usage: " << executable << " <years> <blocks_per_second>\n";
    std::cerr << "example: " << executable << " 10 1\n";
    std::cerr << "example: " << executable << " 10 0.0166666667  # one block per minute\n";
    std::cerr << "example: " << executable << " 10 1000000      # one block per microsecond\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        printUsage(argv[0]);
        return 1;
    }

    const long double years = std::strtold(argv[1], nullptr);
    const long double blocks_per_second = std::strtold(argv[2], nullptr);

    if (!std::isfinite(years) || !std::isfinite(blocks_per_second) || years <= 0.0L || blocks_per_second <= 0.0L) {
        printUsage(argv[0]);
        return 1;
    }

    const long double seconds = years * kSecondsPerJulianYear;
    const long double block_count = seconds * blocks_per_second;

    // Genesis frontier is p_1 = 2, so height h has frontier approximately p_(h+1).
    const long double prime_index = block_count + 1.0L;
    const long double frontier_estimate = nthPrimeEstimate(prime_index);
    const long double decimal_digits = std::floor(std::log10(frontier_estimate)) + 1.0L;
    const long double bit_length = std::floor(std::log2(frontier_estimate)) + 1.0L;
    const long double expected_gap = std::log(frontier_estimate);

    std::cout << std::setprecision(6) << std::scientific;
    std::cout << "years: " << years << "\n";
    std::cout << "blocks_per_second: " << blocks_per_second << "\n";
    std::cout << "estimated_blocks: " << block_count << "\n";
    std::cout << "estimated_prime_index: " << prime_index << "\n";
    std::cout << "estimated_frontier_prime: " << frontier_estimate << "\n";

    std::cout << std::fixed << std::setprecision(0);
    std::cout << "estimated_decimal_digits: " << decimal_digits << "\n";
    std::cout << "estimated_bit_length: " << bit_length << "\n";

    std::cout << std::setprecision(2);
    std::cout << "expected_prime_gap_near_frontier: " << expected_gap << "\n";

    return 0;
}
