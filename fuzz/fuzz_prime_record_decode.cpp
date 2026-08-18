// libFuzzer target for primechain::protocol::deserializePrimeRecord().
// See fuzz_transaction_decode.cpp for the general rationale; build
// instructions in fuzz/README.md.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "primechain/protocol/records.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    std::string error;
    const auto record = primechain::protocol::deserializePrimeRecord(bytes, error);
    if (record.has_value()) {
        (void)primechain::protocol::serializePrimeRecord(*record);
        (void)primechain::protocol::candidateRecordHash(*record);
        (void)primechain::protocol::finalizedRecordHash(*record);
    }
    return 0;
}
