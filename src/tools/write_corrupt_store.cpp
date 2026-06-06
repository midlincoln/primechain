#include <iostream>
#include <string>

#include "primechain/node/sequential_node.hpp"
#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"

namespace {

primechain::protocol::PrimeRecordV0 makePrime3(const primechain::Hash256& previous_hash) {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = 1;
    record.previous_record_hash = previous_hash;
    record.integer = 3;
    record.proof.p = 3;
    record.proof.witness = 2;
    record.proof.factors_of_p_minus_1.push_back({2, 1});
    record.proof.provider_address = "pcdev1_corrupt_prime";
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

primechain::protocol::CompositeRecordV0 makeBadComposite4(const primechain::Hash256& previous_hash) {
    primechain::protocol::CompositeRecordV0 record;
    record.version = 0;
    record.height = 2;
    record.previous_record_hash = previous_hash;
    record.integer = 4;
    record.proof.g = 4;
    record.proof.d = 2;
    record.proof.e = 3;
    record.proof.provider_address = "pcdev1_corrupt_composite";
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <record_store_path>\n";
        return 1;
    }

    const std::string path = argv[1];
    primechain::storage::RecordStore store(path);
    std::string error;

    const auto genesis =
        primechain::storage::makeStoredRecord(primechain::node::makeGenesisPrimeRecordV0());
    if (!store.append(genesis, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto prime3 = primechain::storage::makeStoredRecord(makePrime3(genesis.record_hash));
    error.clear();
    if (!store.append(prime3, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto bad_composite =
        primechain::storage::makeStoredRecord(makeBadComposite4(prime3.record_hash));
    error.clear();
    if (!store.append(bad_composite, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << "corrupt store written\n";
    std::cout << "store_path: " << path << "\n";
    std::cout << "bad_integer: 4\n";
    return 0;
}
