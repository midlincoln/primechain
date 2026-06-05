#include <cstdint>
#include <iostream>
#include <string>

#include "primechain/crypto/hash.hpp"
#include "primechain/node/sequential_node.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

constexpr const char* kDefaultStorePath = "data/sequential-chain.dat";

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [record_store_path] [integer]\n"
              << "       " << argv0 << " [record_store_path] --range [start] [end]\n"
              << "example:\n"
              << "  " << argv0 << " ./data/sequential-500.dat\n"
              << "  " << argv0 << " ./data/sequential-500.dat 500\n"
              << "  " << argv0 << " ./data/sequential-500.dat --range 490 500\n";
}

const char* kindName(primechain::storage::StoredRecordKind kind) {
    switch (kind) {
        case primechain::storage::StoredRecordKind::Composite:
            return "COMPOSITE";
        case primechain::storage::StoredRecordKind::Prime:
            return "PRIME";
    }
    return "UNKNOWN";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const std::string store_path = argc > 1 ? argv[1] : kDefaultStorePath;
    const bool range_lookup = argc > 2 && std::string(argv[2]) == "--range";
    const bool lookup_record = argc > 2 && !range_lookup;

    std::string error;
    primechain::storage::RecordStore store(store_path);
    if (range_lookup) {
        if (argc != 5) {
            printUsage(argv[0]);
            return 1;
        }

        const primechain::PrimeValue start = std::stoull(argv[3]);
        const primechain::PrimeValue end = std::stoull(argv[4]);
        const auto records = store.findRange(start, end, error);
        if (!error.empty()) {
            std::cerr << "record_store_error: " << error << "\n";
            return 1;
        }

        std::cout << "record range\n";
        std::cout << "store_path: " << store_path << "\n";
        std::cout << "start: " << start << "\n";
        std::cout << "end: " << end << "\n";
        std::cout << "records: " << records.size() << "\n";
        std::cout << "integer height kind hash16 payload_bytes\n";
        for (const auto& record : records) {
            std::cout << record.integer << " "
                      << record.height << " "
                      << kindName(record.kind) << " "
                      << primechain::crypto::toHex(record.record_hash).substr(0, 16) << " "
                      << record.payload.size() << "\n";
        }
        return 0;
    }

    if (lookup_record) {
        const primechain::PrimeValue lookup_integer = std::stoull(argv[2]);
        const auto record = store.findByInteger(lookup_integer, error);
        if (!error.empty()) {
            std::cerr << "record_store_error: " << error << "\n";
            return 1;
        }
        if (!record.has_value()) {
            std::cerr << "record_not_found: " << lookup_integer << "\n";
            return 1;
        }

        std::cout << "record lookup\n";
        std::cout << "store_path: " << store_path << "\n";
        std::cout << "integer: " << record->integer << "\n";
        std::cout << "height: " << record->height << "\n";
        std::cout << "kind: " << kindName(record->kind) << "\n";
        std::cout << "record_hash: " << primechain::crypto::toHex(record->record_hash) << "\n";
        std::cout << "payload_bytes: " << record->payload.size() << "\n";
        return 0;
    }

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
