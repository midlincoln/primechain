// libFuzzer target for primechain::protocol::deserializeTransaction().
//
// This is a pure bytes-in/struct-out decode function: no sockets, no
// filesystem, no global state. That makes it directly reachable by any
// SUBMIT_RECORD payload a peer sends (a transaction is embedded inside a
// record), so it's realistic attack surface, and it's cheap and safe to
// fuzz in-process at full speed with no server/workdir scaffolding.
//
// Build (requires clang -- see fuzz/README.md):
//   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DPRIMECHAIN_BUILD_FUZZERS=ON
//   cmake --build build-fuzz --target fuzz-transaction-decode
//   ./build-fuzz/fuzz-transaction-decode

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "primechain/protocol/records.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    std::string error;
    const auto tx = primechain::protocol::deserializeTransaction(bytes, error);
    if (tx.has_value()) {
        // Round-trip it back through the serializer -- a decoded value
        // should always be re-encodable without crashing, whether or not
        // the re-encoded bytes match the original input exactly (they
        // needn't, e.g. if the input had trailing garbage the decoder
        // tolerates).
        (void)primechain::protocol::serializeTransaction(*tx, /*include_signature=*/true);
        (void)primechain::protocol::transactionHash(*tx);
    }
    return 0;
}
