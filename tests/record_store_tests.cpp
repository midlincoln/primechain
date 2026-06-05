#include <iostream>
#include <string>

#include "primechain/storage/record_store.hpp"

namespace {

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

primechain::protocol::PrimeRecordV0 makePrime2() {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = 0;
    record.integer = 2;
    record.proof.p = 2;
    record.proof.witness = 0;
    record.proof.provider_address = "pcdev1_genesis";
    return record;
}

primechain::protocol::CompositeRecordV0 makeComposite4(const primechain::Hash256& previous) {
    primechain::protocol::CompositeRecordV0 record;
    record.version = 0;
    record.height = 2;
    record.previous_record_hash = previous;
    record.integer = 4;
    record.proof.g = 4;
    record.proof.d = 2;
    record.proof.e = 2;
    record.proof.provider_address = "pcdev1_composite";
    return record;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: record-store-tests <store-path>\n";
        return 1;
    }

    const std::string path = argv[1];
    primechain::storage::RecordStore store(path);
    std::string error;

    const auto genesis = primechain::storage::makeStoredRecord(makePrime2());
    if (!expect(store.append(genesis, error), "append genesis")) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto composite = primechain::storage::makeStoredRecord(makeComposite4(genesis.record_hash));
    error.clear();
    if (!expect(store.append(composite, error), "append composite")) {
        std::cerr << error << "\n";
        return 1;
    }

    error.clear();
    const auto records = store.loadAll(error);
    if (!expect(error.empty(), "load without error")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(records.size() == 2, "load two records")) {
        return 1;
    }
    if (!expect(records[0].kind == primechain::storage::StoredRecordKind::Prime, "first record prime")) {
        return 1;
    }
    if (!expect(records[0].integer == 2 && records[0].height == 0, "genesis metadata")) {
        return 1;
    }
    if (!expect(records[1].kind == primechain::storage::StoredRecordKind::Composite, "second record composite")) {
        return 1;
    }
    if (!expect(records[1].integer == 4 && records[1].height == 2, "composite metadata")) {
        return 1;
    }

    error.clear();
    const auto latest = store.latest(error);
    if (!expect(latest.has_value(), "latest record exists")) {
        return 1;
    }
    if (!expect(latest->record_hash == composite.record_hash, "latest hash matches composite")) {
        return 1;
    }

    error.clear();
    const auto found_genesis = store.findByInteger(2, error);
    if (!expect(found_genesis.has_value(), "find genesis by integer")) {
        return 1;
    }
    if (!expect(found_genesis->record_hash == genesis.record_hash, "found genesis hash matches")) {
        return 1;
    }

    error.clear();
    const auto found_composite = store.findByInteger(4, error);
    if (!expect(found_composite.has_value(), "find composite by integer")) {
        return 1;
    }
    if (!expect(found_composite->record_hash == composite.record_hash, "found composite hash matches")) {
        return 1;
    }

    error.clear();
    if (!expect(!store.findByInteger(3, error).has_value(), "missing integer not found")) {
        return 1;
    }

    auto corrupted = composite;
    corrupted.record_hash[0] ^= 0xff;
    error.clear();
    if (!expect(!store.append(corrupted, error), "reject hash mismatch on append")) {
        return 1;
    }

    std::cout << "record store tests passed\n";
    return 0;
}
