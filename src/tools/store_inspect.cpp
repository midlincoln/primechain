#include <cstdint>
#include <iostream>
#include <string>

#include "primechain/crypto/hash.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

constexpr const char* kDefaultStorePath = "data/sequential-chain.dat";

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [record_store_path]\n"
              << "example:\n"
              << "  " << argv0 << " ./data/sequential-500.dat\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const std::string store_path = argc > 1 ? argv[1] : kDefaultStorePath;

    std::string error;
    primechain::storage::RecordStore store(store_path);
    const auto records = store.loadAll(error);
    if (!error.empty()) {
        std::cerr << "record_store_error: " << error << "\n";
        return 1;
    }

    std::uint64_t prime_records = 0;
    std::uint64_t composite_records = 0;
    for (const auto& record : records) {
        if (record.kind == primechain::storage::StoredRecordKind::Prime) {
            ++prime_records;
        } else if (record.kind == primechain::storage::StoredRecordKind::Composite) {
            ++composite_records;
        }
    }

    primechain::node::SequentialNode node(store_path);
    error.clear();
    if (!node.load(error)) {
        std::cerr << "node_replay_error: " << error << "\n";
        return 1;
    }

    const auto& status = node.status();
    std::cout << "record store inspection\n";
    std::cout << "store_path: " << store_path << "\n";
    std::cout << "records: " << records.size() << "\n";
    std::cout << "prime_records: " << prime_records << "\n";
    std::cout << "composite_records: " << composite_records << "\n";
    std::cout << "has_genesis: " << (status.has_genesis ? "yes" : "no") << "\n";
    std::cout << "height: " << status.height << "\n";
    std::cout << "frontier_integer: " << status.frontier_integer << "\n";
    std::cout << "latest_record_hash: " << primechain::crypto::toHex(status.latest_record_hash) << "\n";

    return 0;
}
