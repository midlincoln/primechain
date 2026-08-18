cmake_minimum_required(VERSION 3.10)

project(primechain
    VERSION 0.1.0
    LANGUAGES C CXX
)

add_definitions(-D_FILE_OFFSET_BITS=64)

# Off by default: libFuzzer targets require -fsanitize=fuzzer, which only
# clang implements (this project's default/tested build uses whatever
# CMAKE_CXX_COMPILER resolves to, commonly gcc). Turning this on with a
# non-clang compiler will fail to configure/build -- see fuzz/README.md
# for how to set up a separate clang-based build directory for this.
option(PRIMECHAIN_BUILD_FUZZERS "Build libFuzzer-based fuzz targets (requires clang)" OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mldsa-native/mldsa/mldsa_native.c")
    message(FATAL_ERROR "missing mldsa-native submodule; run git submodule update --init")
endif()

add_library(primechain_mldsa65 STATIC
    third_party/mldsa-native/mldsa/mldsa_native.c
)
target_compile_definitions(primechain_mldsa65 PRIVATE MLD_CONFIG_PARAMETER_SET=65)
target_include_directories(primechain_mldsa65 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mldsa-native/mldsa
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE PRIMECHAIN_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PRIMECHAIN_GIT_COMMIT)
    set(PRIMECHAIN_GIT_COMMIT "unknown")
endif()
string(TIMESTAMP PRIMECHAIN_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/primechain/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/primechain/version.hpp
    @ONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/primechain-ops
    ${CMAKE_CURRENT_BINARY_DIR}/primechain-ops
    COPYONLY
)

add_library(primechain_core
    src/core/consensus.cpp
    src/crypto/hash.cpp
    src/crypto/signature.cpp
    src/math/number_theory.cpp
    src/node/sequential_node.cpp
    src/node/validator_registry.cpp
    src/protocol/records.cpp
    src/storage/atomic_file.cpp
    src/storage/commitment_store.cpp
    src/storage/finalization_store.cpp
    src/storage/phase_store.cpp
    src/storage/record_store.cpp
    src/storage/replay_snapshot_store.cpp
    src/storage/round_change_store.cpp
    src/storage/validator_epoch_store.cpp
    src/wallet/miner_identity.cpp
)

target_compile_definitions(primechain_core PRIVATE MLD_CONFIG_PARAMETER_SET=65)
target_link_libraries(primechain_core PUBLIC OpenSSL::Crypto primechain_mldsa65)

target_include_directories(primechain_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

if(PRIMECHAIN_BUILD_FUZZERS)
    # -fsanitize=fuzzer-no-link instruments this library for coverage
    # feedback without pulling in libFuzzer's own main() (that only goes
    # into the fuzz executables themselves, via their -fsanitize=fuzzer
    # link flag). Without this, a fuzz target would link fine but get zero
    # coverage signal from inside the code it's actually fuzzing --
    # effectively unguided random testing -- and ASan wouldn't catch
    # memory-safety bugs happening inside this library at all. Only
    # applied when PRIMECHAIN_BUILD_FUZZERS is on, so the default
    # (typically gcc) build is completely unaffected.
    target_compile_options(primechain_core PRIVATE -fsanitize=fuzzer-no-link,address -g -O1)
endif()

target_compile_options(primechain_core
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(PRIMECHAIN_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "PRIMECHAIN_BUILD_FUZZERS requires clang (got ${CMAKE_CXX_COMPILER_ID}). "
            "Configure a separate build directory with -DCMAKE_CXX_COMPILER=clang++.")
    endif()

    # Every fuzz target below fuzzes a pure bytes-in/struct-out decode
    # function -- no sockets, no filesystem, no global state -- so they can
    # run in-process at full libFuzzer speed and their crashes are directly
    # reproducible from the saved input, no server/workdir setup needed.
    function(primechain_add_fuzzer name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE primechain_core)
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address -g -O1)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address)
    endfunction()

    primechain_add_fuzzer(fuzz-transaction-decode fuzz/fuzz_transaction_decode.cpp)
    primechain_add_fuzzer(fuzz-composite-record-decode fuzz/fuzz_composite_record_decode.cpp)
    primechain_add_fuzzer(fuzz-prime-record-decode fuzz/fuzz_prime_record_decode.cpp)
endif()

add_executable(primechain-node
    src/node/main.cpp
)

target_link_libraries(primechain-node
    PRIVATE
        primechain_core
)

add_executable(primechain-sync-server
    src/node/sync_server.cpp
)

target_link_libraries(primechain-sync-server
    PRIVATE
        primechain_core
        Threads::Threads
)

add_executable(primechain-miner
    src/miner/main.cpp
)

target_link_libraries(primechain-miner
    PRIVATE
        primechain_core
)

add_executable(primechain-composite-miner
    src/miner/composite_main.cpp
)

target_link_libraries(primechain-composite-miner
    PRIVATE
        primechain_core
)

add_executable(primechain-frontier-miner
    src/miner/frontier_main.cpp
)

target_link_libraries(primechain-frontier-miner
    PRIVATE
        primechain_core
        Threads::Threads
)

add_executable(primechain-estimator
    src/tools/estimator.cpp
)

add_executable(primechain-arithmetic-bench
    src/tools/arithmetic_bench.cpp
)

target_link_libraries(primechain-arithmetic-bench
    PRIVATE
        primechain_core
)

add_executable(primechain-sequential
    src/tools/sequential_chain.cpp
)

target_link_libraries(primechain-sequential
    PRIVATE
        primechain_core
)

add_executable(primechain-store-inspect
    src/tools/store_inspect.cpp
)

target_link_libraries(primechain-store-inspect
    PRIVATE
        primechain_core
)

add_executable(primechain-write-corrupt-store
    src/tools/write_corrupt_store.cpp
)

target_link_libraries(primechain-write-corrupt-store
    PRIVATE
        primechain_core
)

add_executable(primechain-corrupt-sync-server
    src/tools/corrupt_sync_server.cpp
)

target_link_libraries(primechain-corrupt-sync-server
    PRIVATE
        primechain_core
)

add_executable(primechain-sync-query
    src/tools/sync_query.cpp
)

add_executable(primechain-sync-download
    src/tools/sync_download.cpp
)

add_executable(primechain-composite-commitment
    src/tools/composite_commitment.cpp
)

add_executable(primechain-client
    src/tools/client.cpp
)

target_link_libraries(primechain-client
    PRIVATE
        primechain_core
)

target_link_libraries(primechain-composite-commitment
    PRIVATE
        primechain_core
)

target_link_libraries(primechain-sync-download
    PRIVATE
        primechain_core
)

add_executable(primechain-balance
    src/tools/balance_query.cpp
)

target_link_libraries(primechain-balance
    PRIVATE
        primechain_core
)

add_executable(primechain-send
    src/tools/send.cpp
)

target_link_libraries(primechain-send
    PRIVATE
        primechain_core
)

add_executable(primechain-wallet
    src/wallet/wallet_main.cpp
)

target_link_libraries(primechain-wallet
    PRIVATE
        primechain_core
)

add_executable(primechain-tests
    tests/consensus_tests.cpp
)

target_link_libraries(primechain-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-arithmetic-bench-tests
    tests/arithmetic_bench_tests.cpp
)

add_executable(primechain-protocol-record-tests
    tests/protocol_record_tests.cpp
)

target_link_libraries(primechain-protocol-record-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-factorization-tests
    tests/factorization_tests.cpp
)

target_link_libraries(primechain-factorization-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-pratt-tests
    tests/pratt_tests.cpp
)

target_link_libraries(primechain-pratt-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-sequential-tests
    tests/sequential_chain_tests.cpp
)

add_executable(primechain-record-store-tests
    tests/record_store_tests.cpp
)

target_link_libraries(primechain-record-store-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-sequential-node-tests
    tests/sequential_node_tests.cpp
)

target_link_libraries(primechain-sequential-node-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-validator-epoch-tests
    tests/validator_epoch_tests.cpp
)

add_executable(primechain-validator-registry-tests
    tests/validator_registry_tests.cpp
)

target_link_libraries(primechain-validator-epoch-tests
    PRIVATE
        primechain_core
)

target_link_libraries(primechain-validator-registry-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-finalization-store-tests
    tests/finalization_store_tests.cpp
)

target_link_libraries(primechain-finalization-store-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-sidecar-recovery-tests
    tests/sidecar_recovery_tests.cpp
)

target_link_libraries(primechain-sidecar-recovery-tests
    PRIVATE
        primechain_core
)

add_executable(primechain-round-change-tests
    tests/round_change_tests.cpp
)

add_executable(primechain-signature-tests
    tests/signature_tests.cpp
)

target_link_libraries(primechain-signature-tests
    PRIVATE
        primechain_core
)

target_link_libraries(primechain-round-change-tests
    PRIVATE
        primechain_core
)

enable_testing()
add_test(
    NAME primechain_ops_service_generation
    COMMAND
        /bin/bash
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/primechain_ops_smoke.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/primechain-ops
        ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME consensus COMMAND primechain-tests)
add_test(
    NAME arithmetic_bench_output
    COMMAND
        /bin/sh -c
        "$<TARGET_FILE:primechain-arithmetic-bench> 20 7 3 ${CMAKE_CURRENT_BINARY_DIR}/arithmetic-bench-test.log >/dev/null && $<TARGET_FILE:primechain-arithmetic-bench-tests> ${CMAKE_CURRENT_BINARY_DIR}/arithmetic-bench-test.log"
)
add_test(NAME protocol_records COMMAND primechain-protocol-record-tests)
add_test(NAME validator_registry COMMAND primechain-validator-registry-tests ${CMAKE_CURRENT_BINARY_DIR}/validator-registry.dat)
add_test(NAME recursive_factorization COMMAND primechain-factorization-tests)
add_test(NAME pratt_proofs COMMAND primechain-pratt-tests)
add_test(
    NAME sequential_chain_500
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sequential-500.dat && $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/sequential-500.log ${CMAKE_CURRENT_BINARY_DIR}/sequential-500.dat >/dev/null && $<TARGET_FILE:primechain-sequential-tests> ${CMAKE_CURRENT_BINARY_DIR}/sequential-500.log"
)
add_test(
    NAME store_inspect_500
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/inspect-500.dat && $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/inspect-500.log ${CMAKE_CURRENT_BINARY_DIR}/inspect-500.dat >/dev/null && $<TARGET_FILE:primechain-store-inspect> ${CMAKE_CURRENT_BINARY_DIR}/inspect-500.dat | grep -q 'frontier_integer: 500'"
)
add_test(
    NAME store_lookup_500
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/lookup-500.dat && $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/lookup-500.log ${CMAKE_CURRENT_BINARY_DIR}/lookup-500.dat >/dev/null && $<TARGET_FILE:primechain-store-inspect> ${CMAKE_CURRENT_BINARY_DIR}/lookup-500.dat 500 | grep -q 'kind: COMPOSITE'"
)
add_test(
    NAME store_range_490_500
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/range-500.dat && $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/range-500.log ${CMAKE_CURRENT_BINARY_DIR}/range-500.dat >/dev/null && $<TARGET_FILE:primechain-store-inspect> ${CMAKE_CURRENT_BINARY_DIR}/range-500.dat --range 490 500 | grep -q '^500 498 COMPOSITE '"
)
add_test(
    NAME sync_server_status
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sync-500.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-500.log ${CMAKE_CURRENT_BINARY_DIR}/sync-server.log; $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/sync-500.log ${CMAKE_CURRENT_BINARY_DIR}/sync-500.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18989 ${CMAKE_CURRENT_BINARY_DIR}/sync-500.dat > ${CMAKE_CURRENT_BINARY_DIR}/sync-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/sync-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18989 GET_STATUS | grep -q 'STATUS 499 95 404 1 498 500' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/sync-server.pid)"
)
add_test(
    NAME sync_server_mining_view
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/mining-view.dat ${CMAKE_CURRENT_BINARY_DIR}/mining-view.log ${CMAKE_CURRENT_BINARY_DIR}/mining-view-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/mining-view.log ${CMAKE_CURRENT_BINARY_DIR}/mining-view.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18990 ${CMAKE_CURRENT_BINARY_DIR}/mining-view.dat > ${CMAKE_CURRENT_BINARY_DIR}/mining-view-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/mining-view-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18990 GET_MINING_VIEW | grep -q '^MINING_VIEW 20 21 1 .* OPEN 0 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/mining-view-server.pid)"
)
add_test(
    NAME sync_get_factorization_12
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/factor-20.dat ${CMAKE_CURRENT_BINARY_DIR}/factor-20.log ${CMAKE_CURRENT_BINARY_DIR}/factor-20-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/factor-20.log ${CMAKE_CURRENT_BINARY_DIR}/factor-20.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19038 ${CMAKE_CURRENT_BINARY_DIR}/factor-20.dat --enable-factorization-helper > ${CMAKE_CURRENT_BINARY_DIR}/factor-20-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/factor-20.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19038 GET_FACTORIZATION 12 | grep -q '^FACTORIZATION 12 FACTORS 2 PRIME 2 EXP 2 PRIME 3 EXP 1$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/factor-20.pid)"
)
add_test(
    NAME sync_get_factorization_prime_19
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.dat ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.log ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.log ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19039 ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.dat --enable-factorization-helper > ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19039 GET_FACTORIZATION 19 | grep -q '^FACTORIZATION 19 FACTORS 1 PRIME 19 EXP 1$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/factor-prime-20.pid)"
)
add_test(
    NAME sync_get_factorization_unavailable
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/factor-short.dat ${CMAKE_CURRENT_BINARY_DIR}/factor-short.log ${CMAKE_CURRENT_BINARY_DIR}/factor-short-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/factor-short.log ${CMAKE_CURRENT_BINARY_DIR}/factor-short.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19040 ${CMAKE_CURRENT_BINARY_DIR}/factor-short.dat --enable-factorization-helper > ${CMAKE_CURRENT_BINARY_DIR}/factor-short-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/factor-short.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19040 GET_FACTORIZATION 12 | grep -q '^ERROR factorization helper only serves integers at or below the local frontier$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/factor-short.pid)"
)
add_test(
    NAME sync_get_factorization_disabled_by_default
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.dat ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.log ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.log ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19041 ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.dat > ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19041 GET_FACTORIZATION 12 | grep -q '^ERROR GET_FACTORIZATION disabled' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/factor-disabled.pid)"
)
add_test(
    NAME sync_server_bind_any
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/bind-any.dat ${CMAKE_CURRENT_BINARY_DIR}/bind-any.log ${CMAKE_CURRENT_BINARY_DIR}/bind-any-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/bind-any.log ${CMAKE_CURRENT_BINARY_DIR}/bind-any.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18992 ${CMAKE_CURRENT_BINARY_DIR}/bind-any.dat --bind 0.0.0.0 > ${CMAKE_CURRENT_BINARY_DIR}/bind-any-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/bind-any-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18992 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/bind-any-server.pid)"
)
add_test(
    NAME sync_download_500
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sync-src-500.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-src-500.log ${CMAKE_CURRENT_BINARY_DIR}/sync-downloaded-500.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-download-server.log; $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/sync-src-500.log ${CMAKE_CURRENT_BINARY_DIR}/sync-src-500.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18991 ${CMAKE_CURRENT_BINARY_DIR}/sync-src-500.dat > ${CMAKE_CURRENT_BINARY_DIR}/sync-download-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/sync-download-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18991 2 500 ${CMAKE_CURRENT_BINARY_DIR}/sync-downloaded-500.dat | grep -q 'frontier_integer: 500' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/sync-download-server.pid)"
)
add_test(
    NAME sync_download_resume_500
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/resume-src-500.dat ${CMAKE_CURRENT_BINARY_DIR}/resume-src-500.log ${CMAKE_CURRENT_BINARY_DIR}/resume-downloaded-500.dat ${CMAKE_CURRENT_BINARY_DIR}/resume-server.log; $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/resume-src-500.log ${CMAKE_CURRENT_BINARY_DIR}/resume-src-500.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18992 ${CMAKE_CURRENT_BINARY_DIR}/resume-src-500.dat > ${CMAKE_CURRENT_BINARY_DIR}/resume-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/resume-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18992 2 250 ${CMAKE_CURRENT_BINARY_DIR}/resume-downloaded-500.dat >/dev/null && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18992 251 500 ${CMAKE_CURRENT_BINARY_DIR}/resume-downloaded-500.dat | grep -q 'frontier_integer: 500' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/resume-server.pid)"
)
add_test(
    NAME sync_rejects_corrupt_peer_record
    COMMAND
        /bin/sh -c
        "corrupt_port=$((20000 + ($$ % 10000))); clean_port=$((30000 + ($$ % 10000))); rm -f ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.dat ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.log ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.pid ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.dat ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.log ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.exit; $<TARGET_FILE:primechain-write-corrupt-store> ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.dat >/dev/null && ($<TARGET_FILE:primechain-corrupt-sync-server> $corrupt_port ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.dat > ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.pid) && ready=0; attempts=0; while [ $attempts -lt 20 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 $corrupt_port GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done; [ $ready -eq 1 ] && $<TARGET_FILE:primechain-sync-server> $clean_port ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.dat --peer 127.0.0.1 $corrupt_port > ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.log 2>&1; clean_exit=$?; echo $clean_exit > ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.exit; grep -q '^1$' ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.exit && grep -q 'invalid composite payload proof' ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.log && $<TARGET_FILE:primechain-store-inspect> ${CMAKE_CURRENT_BINARY_DIR}/corrupt-clean.dat | grep -q 'has_genesis: no'; result=$?; peer_pid=$(cat ${CMAKE_CURRENT_BINARY_DIR}/corrupt-peer.pid); kill $peer_pid 2>/dev/null || true; attempts=0; while kill -0 $peer_pid 2>/dev/null && [ $attempts -lt 20 ]; do attempts=$((attempts + 1)); sleep 0.05; done; exit $result"
)
add_test(
    NAME sync_rejects_large_record_range
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/range-limit.dat ${CMAKE_CURRENT_BINARY_DIR}/range-limit.log ${CMAKE_CURRENT_BINARY_DIR}/range-limit-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/range-limit.log ${CMAKE_CURRENT_BINARY_DIR}/range-limit.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18988 ${CMAKE_CURRENT_BINARY_DIR}/range-limit.dat > ${CMAKE_CURRENT_BINARY_DIR}/range-limit-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/range-limit-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18988 GET_RECORD_RANGE 2 10002 | grep -q '^ERROR GET_RECORD_RANGE too large' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/range-limit-server.pid)"
)
add_test(
    NAME sync_rejects_too_many_commands
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/command-limit.dat ${CMAKE_CURRENT_BINARY_DIR}/command-limit.log; ($<TARGET_FILE:primechain-sync-server> 19019 ${CMAKE_CURRENT_BINARY_DIR}/command-limit.dat > ${CMAKE_CURRENT_BINARY_DIR}/command-limit.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/command-limit.pid) && sleep 0.2 && /bin/bash -c 'exec 3<>/dev/tcp/127.0.0.1/19019; for i in $(seq 1 129); do printf \"GET_STATUS\\n\" >&3; done; cat <&3' | grep -q '^ERROR rate limit exceeded: too many commands' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/command-limit.pid)"
)
add_test(
    NAME sync_rejects_too_many_write_commands
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/write-limit.dat ${CMAKE_CURRENT_BINARY_DIR}/write-limit.log; ($<TARGET_FILE:primechain-sync-server> 19020 ${CMAKE_CURRENT_BINARY_DIR}/write-limit.dat > ${CMAKE_CURRENT_BINARY_DIR}/write-limit.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/write-limit.pid) && sleep 0.2 && /bin/bash -c 'exec 3<>/dev/tcp/127.0.0.1/19020; for i in $(seq 1 17); do printf \"ADD_PEER 127.0.0.1 19021\\n\" >&3; done; cat <&3' | grep -q '^ERROR rate limit exceeded: too many write commands' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/write-limit.pid)"
)
add_test(
    NAME peer_add_and_list
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/peer-add.dat ${CMAKE_CURRENT_BINARY_DIR}/peer-add.dat.peers ${CMAKE_CURRENT_BINARY_DIR}/peer-add.log; ($<TARGET_FILE:primechain-sync-server> 19014 ${CMAKE_CURRENT_BINARY_DIR}/peer-add.dat > ${CMAKE_CURRENT_BINARY_DIR}/peer-add.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/peer-add.pid) && ready=0 && attempts=0 && while [ $attempts -lt 30 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19014 GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done && [ $ready -eq 1 ] && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19014 ADD_PEER 127.0.0.1 19015 | grep -q '^PEER_ADDED 127.0.0.1 19015' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19014 GET_PEERS | grep -q '^PEER 127.0.0.1 19015' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/peer-add.pid)"
)
add_test(
    NAME peer_discovers_peer_of_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/discover-c.dat ${CMAKE_CURRENT_BINARY_DIR}/discover-c.dat.peers ${CMAKE_CURRENT_BINARY_DIR}/discover-c.log ${CMAKE_CURRENT_BINARY_DIR}/discover-b.dat ${CMAKE_CURRENT_BINARY_DIR}/discover-b.dat.peers ${CMAKE_CURRENT_BINARY_DIR}/discover-b.log ${CMAKE_CURRENT_BINARY_DIR}/discover-a.dat ${CMAKE_CURRENT_BINARY_DIR}/discover-a.dat.peers ${CMAKE_CURRENT_BINARY_DIR}/discover-a.log ${CMAKE_CURRENT_BINARY_DIR}/discover-chain.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/discover-chain.log ${CMAKE_CURRENT_BINARY_DIR}/discover-c.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 29116 ${CMAKE_CURRENT_BINARY_DIR}/discover-c.dat > ${CMAKE_CURRENT_BINARY_DIR}/discover-c.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/discover-c.pid) && ready=0 && attempts=0 && while [ $attempts -lt 30 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 29116 GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done && [ $ready -eq 1 ] && ($<TARGET_FILE:primechain-sync-server> 29115 ${CMAKE_CURRENT_BINARY_DIR}/discover-b.dat --peer 127.0.0.1 29116 > ${CMAKE_CURRENT_BINARY_DIR}/discover-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/discover-b.pid) && ready=0 && attempts=0 && while [ $attempts -lt 30 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 29115 GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done && [ $ready -eq 1 ] && ($<TARGET_FILE:primechain-sync-server> 29114 ${CMAKE_CURRENT_BINARY_DIR}/discover-a.dat --peer 127.0.0.1 29115 > ${CMAKE_CURRENT_BINARY_DIR}/discover-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/discover-a.pid) && ready=0 && attempts=0 && while [ $attempts -lt 30 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 29114 GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done && [ $ready -eq 1 ] && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 29114 GET_PEERS | grep -q '^PEER 127.0.0.1 29116' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 29114 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/discover-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/discover-b.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/discover-c.pid)"
)
add_test(
    NAME startup_sync_skips_dead_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-source.dat ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-source.dat.peers ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-source.log ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-good.log ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.dat ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.dat.peers ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-source.log ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-source.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19018 ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-source.dat > ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-good.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-good.pid) && ready=0 && attempts=0 && while [ $attempts -lt 30 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19018 GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done && [ $ready -eq 1 ] && ($<TARGET_FILE:primechain-sync-server> 19017 ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.dat --peer 127.0.0.1 19999 --peer 127.0.0.1 19018 > ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.pid) && ready=0 && attempts=0 && while [ $attempts -lt 30 ]; do if $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19017 GET_STATUS >/dev/null 2>&1; then ready=1; break; fi; attempts=$((attempts + 1)); sleep 0.1; done && [ $ready -eq 1 ] && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19017 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && grep -q 'peer status warning from 127.0.0.1:19999' ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.log && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-node.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/skip-dead-good.pid)"
)
add_test(
    NAME wallet_new_address
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/alice.wallet && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/alice.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/alice.wallet | grep -q '^pcdev1_'"
)
add_test(
    NAME balance_query_prime_miner
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/balance-50.dat ${CMAKE_CURRENT_BINARY_DIR}/balance-50.log && $<TARGET_FILE:primechain-sequential> 50 ${CMAKE_CURRENT_BINARY_DIR}/balance-50.log ${CMAKE_CURRENT_BINARY_DIR}/balance-50.dat >/dev/null && $<TARGET_FILE:primechain-balance> ${CMAKE_CURRENT_BINARY_DIR}/balance-50.dat pcdev1_prime_miner | grep -q '^3 1000000'"
)
add_test(
    NAME client_new_miner_and_address
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-miner.wallet && $<TARGET_FILE:primechain-client> new-miner ${CMAKE_CURRENT_BINARY_DIR}/client-miner.wallet | grep -q '^pcpq1_' && $<TARGET_FILE:primechain-client> address ${CMAKE_CURRENT_BINARY_DIR}/client-miner.wallet | grep -q '^pcpq1_'"
)
add_test(
    NAME client_inspect_store
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.log && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.log ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat >/dev/null && $<TARGET_FILE:primechain-client> inspect ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat | grep -q 'frontier_integer: 20' && $<TARGET_FILE:primechain-client> inspect ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat --range 18 20 | grep -q '^20 18 COMPOSITE ' && $<TARGET_FILE:primechain-client> decode-record ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat 19 | grep -q '^factors_of_p_minus_1: 2 2^1 3^2$' && $<TARGET_FILE:primechain-client> decode-record ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat 20 | grep -q '^divisor: 2$' && $<TARGET_FILE:primechain-client> decode-record ${CMAKE_CURRENT_BINARY_DIR}/client-inspect.dat 20 | grep -q '^cofactor: 10$'"
)
add_test(
    NAME client_board_reports
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat ${CMAKE_CURRENT_BINARY_DIR}/client-board.log && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/client-board.log ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat >/dev/null && $<TARGET_FILE:primechain-client> board-report ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat --from 2 --to 20 | grep -q '^BOARD_REPORT .* from=2 to=20$' && $<TARGET_FILE:primechain-client> board-report ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat --from 2 --to 20 | grep -q '^RECORDS total=19 prime=8 composite=11 transactions=0$' && $<TARGET_FILE:primechain-client> board-report ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat --from 2 --to 20 | grep -q '^REWARDS discovery_micro_units=8000000 fee_micro_units=0 unique_miners=3$' && $<TARGET_FILE:primechain-client> validator-reputation ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat pcdev1_prime_miner | grep -q '^MINING_HISTORY prime_records=7 composite_records=0 work_score=110 discovery_micro_units=4000004 fee_micro_units=0$' && $<TARGET_FILE:primechain-client> board-report ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat --from 2 --to 20 | grep -q '^VALIDATOR_EVIDENCE_SUMMARY active=0 historical=0 bootstrap_dev=2$' && $<TARGET_FILE:primechain-client> board-report ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat --from 2 --to 20 | grep -q '^VALIDATOR_EVIDENCE pcdev1_validator_a class=bootstrap-dev ' && $<TARGET_FILE:primechain-client> validator-reputation ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat pcdev1_validator_a | grep -q '^VALIDATOR_PARTICIPATION finalization_votes=19 commit_phase_votes=0 round_change_votes=0 total_participation_events=19$' && $<TARGET_FILE:primechain-client> validator-eligibility ${CMAKE_CURRENT_BINARY_DIR}/client-board.dat pcdev1_prime_miner --reserve 5000000 --observed 80 --total 100 | grep -q '^VALIDATOR_ELIGIBILITY pcdev1_prime_miner eligible=1$'"
)

add_test(
    NAME client_launch_report
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.log && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.log ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat >/dev/null && $<TARGET_FILE:primechain-client> launch-report ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat | grep -q '^LAUNCH_REPORT ' && $<TARGET_FILE:primechain-client> launch-report ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat | grep -q '^CHAIN has_genesis=1 height=18 frontier=20 .* records=19 prime_records=8 composite_records=11 transactions=0$' && $<TARGET_FILE:primechain-client> launch-report ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat | grep -q '^VALIDATOR_STATE epoch=0 active_validators=0 registry_events=0 endpoint_events=0 active_endpoints=0 transfer_fee_micro_units=1 validator_min_reserve_micro_units=5000000$' && $<TARGET_FILE:primechain-client> launch-report ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat | grep -q '^VALIDATOR_EVIDENCE_SUMMARY active=0 historical=0 bootstrap_dev=2$' && $<TARGET_FILE:primechain-client> launch-report ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat | grep -q '^BOARD records=19 prime=8 composite=11 transactions=0 discovery_micro_units=8000000 fee_micro_units=0 unique_miners=3 pending_composites_after_range=1$' && $<TARGET_FILE:primechain-client> launch-report ${CMAKE_CURRENT_BINARY_DIR}/client-launch-report.dat | grep -q '^VALIDATOR_EVIDENCE pcdev1_validator_a class=bootstrap-dev '"
)

add_test(
    NAME client_status_and_sync
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-sync-src.dat ${CMAKE_CURRENT_BINARY_DIR}/client-sync-src.log ${CMAKE_CURRENT_BINARY_DIR}/client-sync-downloaded.dat ${CMAKE_CURRENT_BINARY_DIR}/client-sync-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/client-sync-src.log ${CMAKE_CURRENT_BINARY_DIR}/client-sync-src.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19082 ${CMAKE_CURRENT_BINARY_DIR}/client-sync-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/client-sync-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/client-sync-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-client> status 127.0.0.1 19082 | grep -q '^STATUS 19 8 11 1 18 20 ' && $<TARGET_FILE:primechain-client> sync 127.0.0.1 19082 2 20 ${CMAKE_CURRENT_BINARY_DIR}/client-sync-downloaded.dat | grep -q 'frontier_integer: 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/client-sync-server.pid)"
)
add_test(
    NAME client_mine_to_frontier
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-mine*; $<TARGET_FILE:primechain-client> new-miner ${CMAKE_CURRENT_BINARY_DIR}/client-mine-prime.wallet >/dev/null && $<TARGET_FILE:primechain-client> new-miner ${CMAKE_CURRENT_BINARY_DIR}/client-mine-composite.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19083 ${CMAKE_CURRENT_BINARY_DIR}/client-mine.dat > ${CMAKE_CURRENT_BINARY_DIR}/client-mine-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/client-mine-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-client> mine 127.0.0.1 19083 6 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/client-mine-prime.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/client-mine-composite.wallet | grep -q 'frontier miner complete frontier=6' && $<TARGET_FILE:primechain-client> status 127.0.0.1 19083 | grep -q '^STATUS 5 3 2 1 4 6 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/client-mine-server.pid)"
)
add_test(
    NAME client_workdir_sync_status
    COMMAND
        /bin/sh -c
        "rm -rf ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-src.dat ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-src.log ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-src.log ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-src.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19084 ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-client> init-workdir ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync 127.0.0.1 19084 | grep -q '^WORKDIR_INITIALIZED ' && $<TARGET_FILE:primechain-client> sync-peer ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync | grep -q '^SYNCED 2 20$' && $<TARGET_FILE:primechain-client> job-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync | grep -q '^LOCAL_FRONTIER 20$' && $<TARGET_FILE:primechain-client> update-indexes ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync | grep -q '^INDEX_UPDATED .* frontier=20 proofs=11 ' && $<TARGET_FILE:primechain-client> index-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync | grep -q '^INDEX_STATUS .* frontier=20 height=18 proofs=11 ' && $<TARGET_FILE:primechain-client> factor-workdir ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync 18 | grep -q '^FACTOR 2 2^1 3^2$' && $<TARGET_FILE:primechain-client> pratt-workdir ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync 19 | grep -q '^PRATT 19 witness=2 factors=2^1,3^2$' && $<TARGET_FILE:primechain-client> inspect ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync/data/chain.dat | grep -q 'frontier_integer: 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-sync-server.pid)"
)
add_test(
    NAME client_workdir_mine_job
    COMMAND
        /bin/sh -c
        "rm -rf ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine.dat ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine-server.log; ($<TARGET_FILE:primechain-sync-server> 19085 ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine.dat > ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-client> init-workdir ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine 127.0.0.1 19085 >/dev/null && $<TARGET_FILE:primechain-client> add-mine-job ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine --target 6 | grep -q '^MINE_JOB_ADDED .* target=6$' && $<TARGET_FILE:primechain-client> job-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^JOB_STATUS pending$' && $<TARGET_FILE:primechain-client> run-jobs ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^JOB_COMPLETE target=6 frontier=6$' && $<TARGET_FILE:primechain-client> job-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^MINE_TARGET 6$' && $<TARGET_FILE:primechain-client> job-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^JOB_STATUS complete$' && $<TARGET_FILE:primechain-client> job-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^JOB_LAST_SYNCED_FRONTIER 6$' && $<TARGET_FILE:primechain-client> job-status ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^LOCAL_FRONTIER 6$' && $<TARGET_FILE:primechain-client> balances ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^WALLET prime .* total_micro_units=1500000$' && $<TARGET_FILE:primechain-client> balances ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^WALLET composite .* total_micro_units=500000$' && $<TARGET_FILE:primechain-client> rewards ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^PRIME_WALLET .* records=2 reward_micro_units=1500000$' && $<TARGET_FILE:primechain-client> rewards ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^COMPOSITE_WALLET .* records=2 reward_micro_units=500000$' && $<TARGET_FILE:primechain-client> rewards ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^PENDING_COMPOSITE_RECORDS 1$' && $<TARGET_FILE:primechain-client> reward-history ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^REWARD prime integer=3 amount=1000000 role=prime-miner record_height=1$' && $<TARGET_FILE:primechain-client> reward-history ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine | grep -q '^REWARD composite integer=5 amount=500000 role=composite-provider source=4 record_height=3$' && $<TARGET_FILE:primechain-client> reward-history ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine --last 2 | grep -q '^REWARD_HISTORY .* events=2$' && $<TARGET_FILE:primechain-client> reward-history ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine --last 2 | grep -q '^REWARD prime integer=5 amount=500000 role=prime-miner record_height=3$' && $<TARGET_FILE:primechain-client> status 127.0.0.1 19085 | grep -q '^STATUS 5 3 2 1 4 6 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-mine-server.pid)"
)
add_test(
    NAME client_two_workdirs_race_to_same_target
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_two_workdir_race.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/client-two-workdir-race
)

add_test(
    NAME quorum_client_workdir_mines_to_10
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/quorum_client_workdir_mine.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/quorum-client-workdir-mine
)

add_test(
    NAME address_index_workdir_smoke
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/address_index_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-wallet>
        $<TARGET_FILE:primechain-sequential>
        $<TARGET_FILE:primechain-send>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/address-index-smoke
)

add_test(
    NAME reward_index_workdir_smoke
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/reward_index_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-wallet>
        $<TARGET_FILE:primechain-sequential>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/reward-index-smoke
)

add_test(
    NAME participation_index_workdir_smoke
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/participation_index_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-wallet>
        $<TARGET_FILE:primechain-sequential>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/participation-index-smoke
)

add_test(
    NAME board_report_index_smoke
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/board_report_index_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-wallet>
        $<TARGET_FILE:primechain-sequential>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/board-report-index-smoke
)

add_test(
    NAME launch_report_index_smoke
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/launch_report_index_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-wallet>
        $<TARGET_FILE:primechain-sequential>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/launch-report-index-smoke
)

add_test(
    NAME composite_lottery_allows_winning_composites
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/composite_lottery_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/composite-lottery-win
        win
)

add_test(
    NAME composite_lottery_zero_bps_blocks_composites
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/composite_lottery_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/composite-lottery-lose
        lose
)

add_test(
    NAME composite_lottery_quorum_mines_to_6
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/composite_lottery_quorum_smoke.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/composite-lottery-quorum
)

add_test(
    NAME composite_lottery_two_client_bypass
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/composite_lottery_two_client_bypass.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/composite-lottery-two-client-bypass
)

add_test(
    NAME quorum_direct_reveal_accepts_without_commitment
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/quorum_pending_reveal.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        $<TARGET_FILE:primechain-composite-commitment>
        $<TARGET_FILE:primechain-sequential>
        ${CMAKE_CURRENT_BINARY_DIR}/quorum-pending-reveal
)

add_test(
    NAME quorum_two_client_workdirs_race_to_same_target
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/quorum_two_client_workdir_race.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        ${CMAKE_CURRENT_BINARY_DIR}/quorum-two-client-workdir-race
)

add_test(
    NAME client_workdir_mines_from_synced_history
    COMMAND
        /bin/sh -c
        "rm -rf ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-src.dat ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-src.log ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-server.log; $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-src.log ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-src.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19086 ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-client> init-workdir ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history 127.0.0.1 19086 >/dev/null && $<TARGET_FILE:primechain-client> sync-peer ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history | grep -q '^SYNCED 2 20$' && $<TARGET_FILE:primechain-client> add-mine-job ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history --target 30 >/dev/null && $<TARGET_FILE:primechain-client> run-jobs ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history | grep -q '^JOB_COMPLETE target=30 frontier=30$' && $<TARGET_FILE:primechain-client> status 127.0.0.1 19086 | grep -q '^STATUS 29 10 19 1 28 30 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/client-workdir-history-server.pid)"
)
add_test(
    NAME client_math_workbench
    COMMAND
        /bin/sh -c
        "$<TARGET_FILE:primechain-client> is-prime 97 | grep -q '^PRIME 97$' && $<TARGET_FILE:primechain-client> is-prime 99 | grep -q '^COMPOSITE 99$' && $<TARGET_FILE:primechain-client> divisor 91 | grep -q '^DIVISOR 91 7 13$'"
)
add_test(
    NAME client_factor_and_pratt_workbench
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-math.dat ${CMAKE_CURRENT_BINARY_DIR}/client-math.log && $<TARGET_FILE:primechain-sequential> 100 ${CMAKE_CURRENT_BINARY_DIR}/client-math.log ${CMAKE_CURRENT_BINARY_DIR}/client-math.dat >/dev/null && $<TARGET_FILE:primechain-client> factor ${CMAKE_CURRENT_BINARY_DIR}/client-math.dat 84 | grep -q '^FACTOR 3 2^2 3^1 7^1$' && $<TARGET_FILE:primechain-client> pratt ${CMAKE_CURRENT_BINARY_DIR}/client-math.dat 97 | grep -q '^PRATT 97 witness=5 factors=2^5,3^1$'"
)
add_test(
    NAME client_factor_requires_history
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/client-short.dat ${CMAKE_CURRENT_BINARY_DIR}/client-short.log && $<TARGET_FILE:primechain-sequential> 10 ${CMAKE_CURRENT_BINARY_DIR}/client-short.log ${CMAKE_CURRENT_BINARY_DIR}/client-short.dat >/dev/null && ! $<TARGET_FILE:primechain-client> factor ${CMAKE_CURRENT_BINARY_DIR}/client-short.dat 84 2>&1 | grep -q '^FACTOR '"
)
add_test(
    NAME sequential_custom_miner_address
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/custom-miner-50.dat ${CMAKE_CURRENT_BINARY_DIR}/custom-miner-50.log && $<TARGET_FILE:primechain-sequential> 50 ${CMAKE_CURRENT_BINARY_DIR}/custom-miner-50.log ${CMAKE_CURRENT_BINARY_DIR}/custom-miner-50.dat --prime-miner pcdev1_custom_prime_miner --composite-miner pcdev1_custom_composite_miner >/dev/null && $<TARGET_FILE:primechain-balance> ${CMAKE_CURRENT_BINARY_DIR}/custom-miner-50.dat pcdev1_custom_prime_miner | grep -q '^3 1000000'"
)
add_test(
    NAME sequential_transfer_to_wallet
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/tx-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/tx-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/tx-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/tx-chain.log && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/tx-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/tx-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/tx-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/tx-alice.wallet) && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/tx-chain.log ${CMAKE_CURRENT_BINARY_DIR}/tx-chain.dat --prime-miner $miner --composite-miner pcdev1_custom_composite_miner --transfer ${CMAKE_CURRENT_BINARY_DIR}/tx-miner.wallet $alice 3 250000 4 >/dev/null && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/tx-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/tx-alice.wallet | grep -q '^3 250000'"
)
add_test(
    NAME send_tool_transfer_to_wallet
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/send-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/send-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/send-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/send-chain.log && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/send-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/send-alice.wallet >/dev/null && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/send-alice.wallet) && $<TARGET_FILE:primechain-send> 20 ${CMAKE_CURRENT_BINARY_DIR}/send-chain.log ${CMAKE_CURRENT_BINARY_DIR}/send-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/send-miner.wallet $alice 3 250000 4 >/dev/null && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/send-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/send-alice.wallet | grep -q '^3 250000'"
)
add_test(
    NAME tcp_submit_tx
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/submit-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/submit-src.dat ${CMAKE_CURRENT_BINARY_DIR}/submit-src.log ${CMAKE_CURRENT_BINARY_DIR}/submit-server.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/submit-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/submit-src.log ${CMAKE_CURRENT_BINARY_DIR}/submit-src.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18993 ${CMAKE_CURRENT_BINARY_DIR}/submit-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/submit-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/submit-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/submit-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18993 ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet $alice 3 250000 1 1 | grep -q '^TX_ACCEPTED ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18993 GET_NONCE $miner | grep -q ' 0 2$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/submit-server.pid)"
)
add_test(
    NAME tcp_get_mempool
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/mempool-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.dat ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.log ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/mempool-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.log ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18994 ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/mempool-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18994 ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18994 GET_MEMPOOL | grep -q '^MEMPOOL 1' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.pid)"
)
add_test(
    NAME composite_commitment_hash_shape
    COMMAND
        /bin/sh -c
        "$<TARGET_FILE:primechain-composite-commitment> 4 2 2 44 pcdev1_commit_miner | grep -Eq '^[0-9a-f]{64}$'"
)
add_test(
    NAME tcp_composite_commit_reveal_accepts
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.log ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.log ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19042 ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.pid) && sleep 0.2 && commitment=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 44 pcdev1_commit_miner) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19042 SUBMIT_COMMIT 4 $commitment pcdev1_commit_miner | grep -q '^COMMIT_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19042 SUBMIT_COMPOSITE_REVEAL 4 2 2 44 pcdev1_commit_miner | grep -q '^COMPOSITE_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19042 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-reveal.pid)"
)
add_test(
    NAME tcp_composite_reveal_requires_commit
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.dat ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.log ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.log ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19043 ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.dat > ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19043 SUBMIT_COMPOSITE_REVEAL 4 2 2 44 pcdev1_commit_miner | grep -q '^ERROR no prior commitment for reveal$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/reveal-no-commit.pid)"
)
add_test(
    NAME tcp_composite_reveal_rejects_mismatch
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.dat ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.log ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.log ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19044 ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.dat > ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.pid) && sleep 0.2 && commitment=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 44 pcdev1_commit_miner) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19044 SUBMIT_COMMIT 4 $commitment pcdev1_commit_miner >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19044 SUBMIT_COMPOSITE_REVEAL 4 2 2 45 pcdev1_commit_miner | grep -q '^ERROR reveal does not match prior commitment$' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19044 GET_STATUS | grep -q 'STATUS 2 2 0 1 1 3' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/reveal-mismatch.pid)"
)
add_test(
    NAME tcp_composite_commit_propagates_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-base.log ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.log ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-base.log ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.dat >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.dat && ($<TARGET_FILE:primechain-sync-server> 19045 ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19046 ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.dat --peer 127.0.0.1 19045 > ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.pid) && sleep 0.3 && commitment=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 44 pcdev1_commit_miner) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19046 SUBMIT_COMMIT 4 $commitment pcdev1_commit_miner | grep -q '^COMMIT_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19045 SUBMIT_COMPOSITE_REVEAL 4 2 2 44 pcdev1_commit_miner | grep -q '^COMPOSITE_ACCEPTED 4 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-prop-b.pid)"
)
add_test(
    NAME tcp_commit_winner_independent_of_arrival_order
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/commit-order-base.log ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.log ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/commit-order-base.log ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.dat >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.dat && ($<TARGET_FILE:primechain-sync-server> 19047 ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.pid) && ($<TARGET_FILE:primechain-sync-server> 19048 ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.pid) && sleep 0.2 && hash_a=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 44 pcdev1_commit_a) && hash_b=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 45 pcdev1_commit_b) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19047 SUBMIT_COMMIT 4 $hash_a pcdev1_commit_a >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19047 SUBMIT_COMMIT 4 $hash_b pcdev1_commit_b >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19048 SUBMIT_COMMIT 4 $hash_b pcdev1_commit_b >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19048 SUBMIT_COMMIT 4 $hash_a pcdev1_commit_a >/dev/null && winner_a=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19047 GET_COMMIT_WINNER 4) && winner_b=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19048 GET_COMMIT_WINNER 4) && [ \"$winner_a\" = \"$winner_b\" ] && if lower=$(printf '%s\\n%s\\n' \"$hash_a\" \"$hash_b\" | sort | head -n 1) && [ \"$lower\" = \"$hash_a\" ]; then winner_address=pcdev1_commit_a; winner_nonce=44; loser_address=pcdev1_commit_b; loser_nonce=45; else winner_address=pcdev1_commit_b; winner_nonce=45; loser_address=pcdev1_commit_a; loser_nonce=44; fi && printf '%s\\n' \"$winner_a\" | grep -q \" $winner_address$\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19047 SUBMIT_COMPOSITE_REVEAL 4 2 2 $loser_nonce $loser_address | grep -q \"^ERROR commitment not selected for reveal; winner=$winner_address$\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19047 SUBMIT_COMPOSITE_REVEAL 4 2 2 $winner_nonce $winner_address | grep -q '^COMPOSITE_ACCEPTED 4 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-order-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-order-b.pid)"
)
add_test(
    NAME tcp_commitment_survives_restart
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.log ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-a.log ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-b.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.log ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19049 ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-a.pid) && sleep 0.2 && commitment=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 44 pcdev1_restart_miner) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19049 SUBMIT_COMMIT 4 $commitment pcdev1_restart_miner | grep -q '^COMMIT_ACCEPTED 4 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-a.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19050 ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-b.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19050 GET_COMMIT_WINNER 4 | grep -q \"^COMMIT_WINNER 4 $commitment pcdev1_restart_miner$\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19050 SUBMIT_COMPOSITE_REVEAL 4 2 2 44 pcdev1_restart_miner | grep -q '^COMPOSITE_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19050 GET_COMMITMENTS 4 | grep -q '^COMMITMENTS 4 0$' && test ! -s ${CMAKE_CURRENT_BINARY_DIR}/commit-restart.dat.commitments && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-restart-b.pid)"
)
add_test(
    NAME tcp_commitments_sync_from_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/commit-sync.log ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.log ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/commit-sync.log ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.dat >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.dat ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.dat && ($<TARGET_FILE:primechain-sync-server> 19051 ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.dat > ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.pid) && sleep 0.2 && commitment=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 55 pcdev1_sync_miner) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19051 SUBMIT_COMMIT 4 $commitment pcdev1_sync_miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19052 ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.dat --peer 127.0.0.1 19051 > ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.pid) && sleep 0.4 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19052 GET_COMMIT_WINNER 4 | grep -q \"^COMMIT_WINNER 4 $commitment pcdev1_sync_miner$\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19052 SUBMIT_COMPOSITE_REVEAL 4 2 2 55 pcdev1_sync_miner | grep -q '^COMPOSITE_ACCEPTED 4 ' && test ! -s ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.dat.commitments && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-source.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/commit-sync-target.pid)"
)
add_test(
    NAME wallet_new_mldsa65_identity
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/signed-miner.wallet; address=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/signed-miner.wallet) && printf '%s\n' \"$address\" | grep -Eq '^pcpq1_[0-9a-f]{40}$' && [ \"$address\" = \"$($<TARGET_FILE:primechain-wallet> miner-address ${CMAKE_CURRENT_BINARY_DIR}/signed-miner.wallet)\" ]"
)
add_test(
    NAME wallet_new_mldsa65_identity_is_encrypted
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/encrypted-miner.wallet; address=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/encrypted-miner.wallet) && grep -q '^version=pc-miner-mldsa65-v3$' ${CMAKE_CURRENT_BINARY_DIR}/encrypted-miner.wallet && ! grep -q '^private_key=' ${CMAKE_CURRENT_BINARY_DIR}/encrypted-miner.wallet && [ \"$address\" = \"$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/encrypted-miner.wallet)\" ] && ! PRIMECHAIN_WALLET_PASSPHRASE=wrong $<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/encrypted-miner.wallet 4 2 2 44 >/dev/null 2>&1"
)
add_test(
    NAME tcp_signed_commit_reveal_accepts
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.dat ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.log ${CMAKE_CURRENT_BINARY_DIR}/signed-flow-server.log ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.wallet; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.log ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.dat >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19053 ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.dat > ${CMAKE_CURRENT_BINARY_DIR}/signed-flow-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.pid) && sleep 0.2 && commit=$($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.wallet 4 2 2 44) && reveal=$($<TARGET_FILE:primechain-composite-commitment> sign-reveal ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.wallet 4 2 2 44) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19053 $commit | grep -q '^COMMIT_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19053 $reveal | grep -q '^COMPOSITE_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19053 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/signed-flow.pid)"
)
add_test(
    NAME tcp_signed_commit_rejects_forgery
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.dat ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.log ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery-server.log ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.wallet; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.log ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.dat >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19054 ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.dat > ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.pid) && sleep 0.2 && set -- $($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.wallet 4 2 2 44) && bad_signature=$(printf '%s' \"$6\" | sed 's/^../00/') && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19054 $1 $2 $3 $4 $5 $bad_signature | grep -q '^ERROR invalid signed commitment signature$' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19054 GET_COMMITMENTS 4 | grep -q '^COMMITMENTS 4 0$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/signed-forgery.pid)"
)
add_test(
    NAME tcp_signed_commit_survives_restart_and_peer_sync
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.dat ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.dat ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/signed-persist.log ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.log ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.log ${CMAKE_CURRENT_BINARY_DIR}/signed-persist.wallet; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/signed-persist.log ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.dat >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.dat ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.dat && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/signed-persist.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19055 ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.dat > ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.pid) && sleep 0.2 && commit=$($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/signed-persist.wallet 4 2 2 55) && reveal=$($<TARGET_FILE:primechain-composite-commitment> sign-reveal ${CMAKE_CURRENT_BINARY_DIR}/signed-persist.wallet 4 2 2 55) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19055 $commit >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19056 ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.dat --peer 127.0.0.1 19055 > ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.pid) && sleep 0.4 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19056 GET_COMMITMENTS 4 | awk '$1 == \"COMMITMENT\" && $2 == 4 && length($3) == 64 && substr($4, 1, 6) == \"pcpq1_\" && length($4) == 46 && length($5) == 3904 && length($6) == 6618 { found=1 } END { exit !found }' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19057 ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b2.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19057 $reveal | grep -q '^COMPOSITE_ACCEPTED 4 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/signed-persist-b2.pid)"
)
add_test(
    NAME frontier_miner_uses_signed_composite_identity
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.dat ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.log ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier-server.log ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.wallet; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.log ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.dat >/dev/null && address=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.wallet) && ($<TARGET_FILE:primechain-sync-server> 19058 ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.dat > ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.pid) && sleep 0.2 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19058 6 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.wallet | grep -q 'frontier miner complete frontier=6' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19058 GET_BALANCE $address | grep -q '^HOLDING 5 1000000$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/signed-frontier.pid)"
)
add_test(
    NAME tcp_submit_composite_extends_frontier
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/submit-composite.dat ${CMAKE_CURRENT_BINARY_DIR}/submit-composite.log ${CMAKE_CURRENT_BINARY_DIR}/submit-composite-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/submit-composite.log ${CMAKE_CURRENT_BINARY_DIR}/submit-composite.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19021 ${CMAKE_CURRENT_BINARY_DIR}/submit-composite.dat > ${CMAKE_CURRENT_BINARY_DIR}/submit-composite-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/submit-composite-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19021 SUBMIT_COMPOSITE 4 2 2 pcdev1_composite_submitter | grep -q '^COMPOSITE_ACCEPTED 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19021 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/submit-composite-server.pid)"
)
add_test(
    NAME tcp_submit_composite_rejects_wrong_frontier
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/wrong-composite.dat ${CMAKE_CURRENT_BINARY_DIR}/wrong-composite.log; ($<TARGET_FILE:primechain-sync-server> 19022 ${CMAKE_CURRENT_BINARY_DIR}/wrong-composite.dat > ${CMAKE_CURRENT_BINARY_DIR}/wrong-composite.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/wrong-composite.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19022 SUBMIT_COMPOSITE 4 2 2 pcdev1_composite_submitter | grep -q '^ERROR SUBMIT_COMPOSITE must extend frontier 2 with integer 3' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/wrong-composite.pid)"
)
add_test(
    NAME tcp_submit_composite_rejects_bad_arithmetic
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/bad-composite.dat ${CMAKE_CURRENT_BINARY_DIR}/bad-composite.log ${CMAKE_CURRENT_BINARY_DIR}/bad-composite-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/bad-composite.log ${CMAKE_CURRENT_BINARY_DIR}/bad-composite.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19025 ${CMAKE_CURRENT_BINARY_DIR}/bad-composite.dat > ${CMAKE_CURRENT_BINARY_DIR}/bad-composite-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/bad-composite-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19025 SUBMIT_COMPOSITE 5 2 2 pcdev1_bad_composite | grep -q '^ERROR invalid composite proof' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19025 GET_STATUS | grep -q 'STATUS 2 2 0 1 1 3' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/bad-composite-server.pid)"
)
add_test(
    NAME tcp_submit_composite_same_tip_uses_lower_hash
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/same-composite.dat ${CMAKE_CURRENT_BINARY_DIR}/same-composite.log ${CMAKE_CURRENT_BINARY_DIR}/same-composite-server.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/same-composite.log ${CMAKE_CURRENT_BINARY_DIR}/same-composite.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19026 ${CMAKE_CURRENT_BINARY_DIR}/same-composite.dat > ${CMAKE_CURRENT_BINARY_DIR}/same-composite-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/same-composite-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19026 SUBMIT_COMPOSITE 4 2 2 pcdev1_composite_a | grep -q '^COMPOSITE_ACCEPTED 4 ' && first=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19026 GET_RECORD 4 | awk '{print $5}') && response=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19026 SUBMIT_COMPOSITE 4 2 2 pcdev1_composite_b) && second=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19026 GET_RECORD 4 | awk '{print $5}') && lower=$(printf '%s\\n%s\\n' \"$first\" \"$second\" | sort | head -n 1) && printf '%s\\n' \"$response\" | grep -Eq '^RECORD_(REPLACED|CONFLICT_WORSE) ' && [ \"$second\" = \"$lower\" ] && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/same-composite-server.pid)"
)
add_test(
    NAME tcp_submit_composite_propagates_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-base.log ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.dat ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.dat ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.log ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.log; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-base.log ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.dat >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.dat ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.dat && ($<TARGET_FILE:primechain-sync-server> 19023 ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19024 ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.dat --peer 127.0.0.1 19023 > ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19024 SUBMIT_COMPOSITE 4 2 2 pcdev1_composite_submitter | grep -q '^COMPOSITE_ACCEPTED 4 ' && sleep 0.4 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19023 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/composite-prop-b.pid)"
)
add_test(
    NAME tcp_submit_prime_from_genesis
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis*; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19029 ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.dat > ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.pid) && sleep 0.2 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19029 3 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.wallet | grep -q 'frontier miner complete frontier=3' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19029 GET_STATUS | grep -q 'STATUS 2 2 0 1 1 3' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-genesis.pid)"
)
add_test(
    NAME tcp_submit_prime_after_composite
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5*; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19030 ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5.dat > ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5.pid) && sleep 0.2 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19030 5 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5.wallet | grep -q 'frontier miner complete frontier=5' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19030 GET_STATUS | grep -q 'STATUS 4 3 1 1 3 5' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/submit-prime-5.pid)"
)
add_test(
    NAME tcp_submit_prime_rejects_bad_pratt
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.dat ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.log; ($<TARGET_FILE:primechain-sync-server> 19031 ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.dat > ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19031 SUBMIT_PRIME 3 2 1 2 2 pcdev1_prime_submitter | grep -q '^ERROR unsigned prime submissions are disabled' && $<TARGET_FILE:primechain-store-inspect> ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.dat | grep -q 'has_genesis: no' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/bad-prime.pid)"
)
add_test(
    NAME tcp_submit_prime_rejects_wrong_frontier
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/wrong-prime.dat ${CMAKE_CURRENT_BINARY_DIR}/wrong-prime.log; ($<TARGET_FILE:primechain-sync-server> 19032 ${CMAKE_CURRENT_BINARY_DIR}/wrong-prime.dat > ${CMAKE_CURRENT_BINARY_DIR}/wrong-prime.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/wrong-prime.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19032 SUBMIT_PRIME 5 2 1 2 2 pcdev1_prime_submitter | grep -q '^ERROR unsigned prime submissions are disabled' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/wrong-prime.pid)"
)
add_test(
    NAME tcp_submit_prime_propagates_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/prime-prop*; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/prime-prop.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19033 ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19034 ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-a.dat --peer 127.0.0.1 19033 > ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-a.pid) && sleep 0.3 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19034 5 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/prime-prop.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/prime-prop.wallet | grep -q 'frontier miner complete frontier=5' && sleep 0.4 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19033 GET_STATUS | grep -q 'STATUS 4 3 1 1 3 5' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/prime-prop-b.pid)"
)
add_test(
    NAME frontier_miner_advances_to_20
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner*; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19035 ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.dat > ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.pid) && sleep 0.2 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19035 20 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.wallet | grep -q 'frontier miner complete frontier=20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19035 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/frontier-miner.pid)"
)
add_test(
    NAME frontier_miner_records_propagate_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop*; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19036 ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19037 ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-a.dat --peer 127.0.0.1 19036 > ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-a.pid) && sleep 0.3 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19037 20 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop.wallet | grep -q 'frontier miner complete frontier=20' && sleep 0.8 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19036 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/frontier-prop-b.pid)"
)
add_test(
    NAME sequential_include_tcp_mempool
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/include-src.dat ${CMAKE_CURRENT_BINARY_DIR}/include-src.log ${CMAKE_CURRENT_BINARY_DIR}/include-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/include-chain.log ${CMAKE_CURRENT_BINARY_DIR}/include-server.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/include-src.log ${CMAKE_CURRENT_BINARY_DIR}/include-src.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18995 ${CMAKE_CURRENT_BINARY_DIR}/include-src.dat --enable-ack-mempool > ${CMAKE_CURRENT_BINARY_DIR}/include-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/include-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18995 ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/include-chain.log ${CMAKE_CURRENT_BINARY_DIR}/include-chain.dat --prime-miner $miner --composite-miner pcdev1_include_composite --mempool 127.0.0.1 18995 4 >/dev/null && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/include-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet | grep -q '^3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18995 GET_MEMPOOL | grep -q '^MEMPOOL 0' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/include-server.pid)"
)
add_test(
    NAME tcp_advance_to_includes_mempool
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/advance-node.log ${CMAKE_CURRENT_BINARY_DIR}/advance-server.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/advance-node.log ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18996 ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/advance-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/advance-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18996 ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet $alice 3 250000 1 | grep -q '^TX_ACCEPTED ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18996 ADVANCE_TO 20 $miner pcdev1_advance_composite 4 | grep -q '^ADVANCED 17 included_txs=1 frontier=20' && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet | grep -q '^3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18996 GET_MEMPOOL | grep -q '^MEMPOOL 0' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/advance-server.pid)"
)
add_test(
    NAME tcp_advance_disabled_by_default
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/advance-disabled-node.dat ${CMAKE_CURRENT_BINARY_DIR}/advance-disabled-server.log; ($<TARGET_FILE:primechain-sync-server> 18990 ${CMAKE_CURRENT_BINARY_DIR}/advance-disabled-node.dat > ${CMAKE_CURRENT_BINARY_DIR}/advance-disabled-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/advance-disabled-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18990 ADVANCE_TO 20 pcdev1_disabled_prime pcdev1_disabled_composite 4 | grep -q '^ERROR ADVANCE_TO disabled' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/advance-disabled-server.pid)"
)
add_test(
    NAME sync_download_after_tcp_advance
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.log ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-downloaded.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.log ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18997 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18997 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18997 ADVANCE_TO 20 $miner pcdev1_sync_advance_composite 4 | grep -q '^ADVANCED 17 included_txs=1 frontier=20' && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18997 2 20 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-downloaded.dat | grep -q 'frontier_integer: 20' && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-downloaded.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet | grep -q '^3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.pid)"
)
add_test(
    NAME tcp_get_balance_after_advance
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/balance-node-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/balance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/balance-node.log ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/balance-node-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/balance-node.log ${CMAKE_CURRENT_BINARY_DIR}/balance-node.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18998 ${CMAKE_CURRENT_BINARY_DIR}/balance-node.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/balance-node-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18998 ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18998 ADVANCE_TO 20 $miner pcdev1_balance_node_composite 4 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18998 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.pid)"
)
add_test(
    NAME three_node_local_convergence
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/three-node-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.dat ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.dat ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.dat ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.log ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.log ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/three-node-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.log ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18999 ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.dat --enable-advance >> ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/three-node-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18999 ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18999 ADVANCE_TO 20 $miner pcdev1_three_node_composite 4 | grep -q '^ADVANCED 17 included_txs=1 frontier=20' && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18999 2 20 ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.dat >/dev/null && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18999 2 20 ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19000 ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.pid) && ($<TARGET_FILE:primechain-sync-server> 19001 ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.dat > ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18999 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19000 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19001 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.pid)"
)
add_test(
    NAME startup_peer_sync
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.dat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.dat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.log ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.log ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19002 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.dat --enable-advance >> ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 19002 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19002 ADVANCE_TO 20 $miner pcdev1_peer_sync_composite 4 | grep -q '^ADVANCED 17 included_txs=1 frontier=20' && ($<TARGET_FILE:primechain-sync-server> 19003 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.dat --peer 127.0.0.1 19002 > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19003 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19003 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.pid)"
)
add_test(
    NAME continuous_peer_sync
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.dat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.dat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.log ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.log ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.dat --prime-miner $miner >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19004 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.dat --enable-advance >> ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19005 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.dat --peer 127.0.0.1 19004 --sync-interval 1 > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.pid) && sleep 0.3 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 19004 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19004 ADVANCE_TO 20 $miner pcdev1_continuous_sync_composite 4 | grep -q '^ADVANCED 17 included_txs=1 frontier=20' && sleep 3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19005 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19005 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.pid)"
)
add_test(
    NAME mempool_propagates_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/propagate-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/propagate-base.log ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.dat ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.dat ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.log ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.log; $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/propagate-alice.wallet >/dev/null && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/propagate-base.log ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.dat --prime-miner $miner >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.dat ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.dat && ($<TARGET_FILE:primechain-sync-server> 19006 ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19007 ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.dat --peer 127.0.0.1 19006 > ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.pid) && sleep 0.3 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/propagate-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 19007 ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet $alice 3 250000 1 | grep -q '^TX_ACCEPTED ' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19006 GET_MEMPOOL | grep -q '^MEMPOOL 1' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.pid)"
)
add_test(
    NAME records_propagate_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/record-prop-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/record-prop-a.dat ${CMAKE_CURRENT_BINARY_DIR}/record-prop-b.dat ${CMAKE_CURRENT_BINARY_DIR}/record-prop-a.log ${CMAKE_CURRENT_BINARY_DIR}/record-prop-b.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/record-prop-miner.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19008 ${CMAKE_CURRENT_BINARY_DIR}/record-prop-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/record-prop-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/record-prop-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19009 ${CMAKE_CURRENT_BINARY_DIR}/record-prop-a.dat --peer 127.0.0.1 19008 --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/record-prop-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/record-prop-a.pid) && sleep 0.3 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/record-prop-miner.wallet) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19009 ADVANCE_TO 20 $miner pcdev1_record_prop_composite 4 | grep -q '^ADVANCED 18 included_txs=0 frontier=20' && sleep 0.5 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19008 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/record-prop-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/record-prop-b.pid)"
)
add_test(
    NAME same_tip_record_conflict_replaces_worse_tip
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/conflict-base.log ${CMAKE_CURRENT_BINARY_DIR}/conflict-base.dat ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.dat ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.dat ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.log ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.log; $<TARGET_FILE:primechain-sequential> 19 ${CMAKE_CURRENT_BINARY_DIR}/conflict-base.log ${CMAKE_CURRENT_BINARY_DIR}/conflict-base.dat --prime-miner pcdev1_conflict_prime --composite-miner pcdev1_conflict_composite >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/conflict-base.dat ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.dat && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/conflict-base.dat ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.dat && ($<TARGET_FILE:primechain-sync-server> 19010 ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.pid) && ($<TARGET_FILE:primechain-sync-server> 19011 ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19010 ADVANCE_TO 20 pcdev1_conflict_prime pcdev1_conflict_composite_a 4 | grep -q '^ADVANCED 1 included_txs=0 frontier=20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19011 ADVANCE_TO 20 pcdev1_conflict_prime pcdev1_conflict_composite_c 4 | grep -q '^ADVANCED 1 included_txs=0 frontier=20' && rec_a=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19010 GET_RECORD 20) && rec_c=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19011 GET_RECORD 20) && hash_a=$(printf '%s\\n' \"$rec_a\" | awk '{print $5}') && hash_c=$(printf '%s\\n' \"$rec_c\" | awk '{print $5}') && lower=$(printf '%s\\n%s\\n' \"$hash_a\" \"$hash_c\" | sort | head -n 1) && if [ \"$lower\" = \"$hash_a\" ]; then better=\"$rec_a\"; worse_port=19011; better_hash=\"$hash_a\"; else better=\"$rec_c\"; worse_port=19010; better_hash=\"$hash_c\"; fi && submit=$(printf '%s\\n' \"$better\" | sed 's/^RECORD /SUBMIT_RECORD /') && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 $worse_port $submit | grep -q '^RECORD_REPLACED ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 $worse_port GET_RECORD 20 | grep -q \"$better_hash\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 $worse_port GET_STATUS | grep -q \"$better_hash\" && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/conflict-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/conflict-c.pid)"
)
add_test(
    NAME two_active_writers_converge_on_lower_tip
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/writers-base.log ${CMAKE_CURRENT_BINARY_DIR}/writers-base.dat ${CMAKE_CURRENT_BINARY_DIR}/writers-a.dat ${CMAKE_CURRENT_BINARY_DIR}/writers-b.dat ${CMAKE_CURRENT_BINARY_DIR}/writers-a.log ${CMAKE_CURRENT_BINARY_DIR}/writers-b.log; $<TARGET_FILE:primechain-sequential> 19 ${CMAKE_CURRENT_BINARY_DIR}/writers-base.log ${CMAKE_CURRENT_BINARY_DIR}/writers-base.dat --prime-miner pcdev1_writers_prime --composite-miner pcdev1_writers_composite >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/writers-base.dat ${CMAKE_CURRENT_BINARY_DIR}/writers-a.dat && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/writers-base.dat ${CMAKE_CURRENT_BINARY_DIR}/writers-b.dat && ($<TARGET_FILE:primechain-sync-server> 19012 ${CMAKE_CURRENT_BINARY_DIR}/writers-a.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/writers-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/writers-a.pid) && ($<TARGET_FILE:primechain-sync-server> 19013 ${CMAKE_CURRENT_BINARY_DIR}/writers-b.dat --enable-advance > ${CMAKE_CURRENT_BINARY_DIR}/writers-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/writers-b.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19012 ADVANCE_TO 20 pcdev1_writers_prime pcdev1_writers_composite_a 4 | grep -q '^ADVANCED 1 included_txs=0 frontier=20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19013 ADVANCE_TO 20 pcdev1_writers_prime pcdev1_writers_composite_b 4 | grep -q '^ADVANCED 1 included_txs=0 frontier=20' && rec_a=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19012 GET_RECORD 20) && rec_b=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19013 GET_RECORD 20) && hash_a=$(printf '%s\\n' \"$rec_a\" | awk '{print $5}') && hash_b=$(printf '%s\\n' \"$rec_b\" | awk '{print $5}') && lower=$(printf '%s\\n%s\\n' \"$hash_a\" \"$hash_b\" | sort | head -n 1) && if [ \"$lower\" = \"$hash_a\" ]; then submit=$(printf '%s\\n' \"$rec_a\" | sed 's/^RECORD /SUBMIT_RECORD /'); $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19013 $submit | grep -Eq '^RECORD_(REPLACED|DUPLICATE) '; else submit=$(printf '%s\\n' \"$rec_b\" | sed 's/^RECORD /SUBMIT_RECORD /'); $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19012 $submit | grep -Eq '^RECORD_(REPLACED|DUPLICATE) '; fi && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19012 GET_RECORD 20 | grep -q \"$lower\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19013 GET_RECORD 20 | grep -q \"$lower\" && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/writers-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/writers-b.pid)"
)
add_test(
    NAME record_store
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/record-store-test.dat* && $<TARGET_FILE:primechain-record-store-tests> ${CMAKE_CURRENT_BINARY_DIR}/record-store-test.dat"
)
add_test(
    NAME sequential_node
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sequential-node-test.dat && $<TARGET_FILE:primechain-sequential-node-tests> ${CMAKE_CURRENT_BINARY_DIR}/sequential-node-test.dat"
)
add_test(
    NAME finalization_store
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/finalization-store-test.dat* && $<TARGET_FILE:primechain-finalization-store-tests> ${CMAKE_CURRENT_BINARY_DIR}/finalization-store-test.dat"
)

add_test(
    NAME sidecar_recovery
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sidecar-recovery-test* && $<TARGET_FILE:primechain-sidecar-recovery-tests> ${CMAKE_CURRENT_BINARY_DIR}/sidecar-recovery-test"
)

add_test(
    NAME round_change
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/round-change-test.dat* && $<TARGET_FILE:primechain-round-change-tests> ${CMAKE_CURRENT_BINARY_DIR}/round-change-test.dat"
)

add_test(
    NAME signatures
    COMMAND primechain-signature-tests
)

add_test(
    NAME validator_epoch_rotation
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/validator-epoch-test.dat* && $<TARGET_FILE:primechain-validator-epoch-tests> ${CMAKE_CURRENT_BINARY_DIR}/validator-epoch-test.dat"
)


add_test(
    NAME commit_phase_timeout_reopens_stalled_phase
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/commit_phase_timeout_tests.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        $<TARGET_FILE:primechain-composite-commitment>
        ${CMAKE_CURRENT_BINARY_DIR}/commit-phase-timeout
)

add_test(
    NAME commit_phase_round_split_validator_recovers
    COMMAND
        /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/commit_phase_round_split_tests.sh
        $<TARGET_FILE:primechain-client>
        $<TARGET_FILE:primechain-sync-server>
        $<TARGET_FILE:primechain-composite-commitment>
        ${CMAKE_CURRENT_BINARY_DIR}/commit-phase-round-split
)

add_test(
    NAME quorum_signed_reveal_closes_phase_with_peer_vote
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-c.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow.log ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow.dat --validator-set $a $b $c --validator-identities ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-a.wallet ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-b.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19059 ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19058 ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-b.dat --peer 127.0.0.1 19059 --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-server-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-b.pid) && sleep 0.3 && ($<TARGET_FILE:primechain-sync-server> 19057 ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-c.dat --peer 127.0.0.1 19059 --peer 127.0.0.1 19058 --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-c.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-server-c.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-c.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 ADD_PEER 127.0.0.1 19058 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 ADD_PEER 127.0.0.1 19057 >/dev/null && unsigned_hash=$($<TARGET_FILE:primechain-composite-commitment> 4 2 2 99 pcdev1_unsigned) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 SUBMIT_COMMIT 4 $unsigned_hash pcdev1_unsigned | grep -q '^ERROR unsigned commitments disabled in quorum mode$' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 SUBMIT_COMPOSITE 4 2 2 pcdev1_direct | grep -q '^ERROR direct SUBMIT_COMPOSITE disabled in quorum mode' && commit=$($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-a.wallet 4 2 2 44) && reveal=$($<TARGET_FILE:primechain-composite-commitment> sign-reveal ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-a.wallet 4 2 2 44) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 $commit | grep -q '^COMMIT_ACCEPTED ' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 $reveal | grep -q '^COMPOSITE_ACCEPTED 4 ' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19059 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19058 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-b.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-flow-c.pid)"
)

add_test(
    NAME quorum_rejects_unconfigured_validator
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-c.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider.log ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider.dat --validator-set $a $b $c --validator-identities ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-a.wallet ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-b.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-x.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19060 ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider.pid) && sleep 0.2 && commit=$($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-a.wallet 4 2 2 44) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19060 $commit >/dev/null && snapshot=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19060 GET_COMMIT_PHASE 4 | awk '{print $5}') && vote=$($<TARGET_FILE:primechain-composite-commitment> sign-phase ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider-x.wallet 4 $snapshot) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19060 $vote | grep -q '^ERROR validator is not in configured set$' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19060 GET_COMMIT_PHASE 4 | grep -q '^COMMIT_PHASE 4 OPEN 0 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-outsider.pid)"
)

add_test(
    NAME quorum_phase_survives_restart
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-c.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart.log ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart.dat --validator-set $a $b $c --validator-identities ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.wallet ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-b.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19061 ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-server-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.pid) && sleep 0.2 && commit=$($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.wallet 4 2 2 44) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19061 $commit >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19061 CLOSE_COMMIT_PHASE 4 | grep -q 'votes=1$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19062 ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-server-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-b.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19062 GET_COMMIT_PHASE 4 | grep -q '^COMMIT_PHASE 4 CLOSING 1 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-restart-b.pid)"
)

add_test(
    NAME quorum_phase_votes_propagate_across_three_nodes
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.wallet) && $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer.log ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.dat --validator-set $a $b $c --validator-identities ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.wallet ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.wallet >/dev/null && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.dat ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.dat && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.dat ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.dat && ($<TARGET_FILE:primechain-sync-server> 19063 ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.pid) && ($<TARGET_FILE:primechain-sync-server> 19064 ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.pid) && ($<TARGET_FILE:primechain-sync-server> 19065 ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.pid) && sleep 0.8 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 ADD_PEER 127.0.0.1 19064 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 ADD_PEER 127.0.0.1 19065 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19064 ADD_PEER 127.0.0.1 19065 >/dev/null && commit=$($<TARGET_FILE:primechain-composite-commitment> sign-commit ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.wallet 4 2 2 44) && reveal=$($<TARGET_FILE:primechain-composite-commitment> sign-reveal ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.wallet 4 2 2 44) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 $commit | grep -q '^COMMIT_ACCEPTED ' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19065 GET_COMMITMENTS 4 | grep -q '^COMMITMENTS 4 1$' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 CLOSE_COMMIT_PHASE 4 | grep -q 'votes=1$' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19064 CLOSE_COMMIT_PHASE 4 | grep -q 'votes=2$' && snapshot=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 GET_COMMIT_PHASE 4 | awk '{print $5}') && bvote=$($<TARGET_FILE:primechain-composite-commitment> sign-phase ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.wallet 4 $snapshot) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 $bvote | grep -q 'votes=2$' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19065 GET_COMMIT_PHASE 4 | grep -q '^COMMIT_PHASE 4 CLOSED 2 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19063 $reveal | grep -q '^COMPOSITE_ACCEPTED 4 ' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19065 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-b.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.pid) && sleep 0.2 && ${CMAKE_COMMAND} -E remove ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.dat.commitments ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.dat.phases && ($<TARGET_FILE:primechain-sync-server> 19066 ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-c.dat > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-replay.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-replay.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19066 GET_STATUS | grep -q 'STATUS 3 2 1 1 2 4' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-peer-replay.pid)"
)

add_test(
    NAME quorum_genesis_anchor_rejects_validator_replacement
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-c.wallet) && x=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-x.wallet) && ($<TARGET_FILE:primechain-sync-server> 19067 ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-a.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19067 GET_STATUS | grep -q '^STATUS 1 1 0 1 0 2 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-a.pid) && sleep 0.2 && ! $<TARGET_FILE:primechain-sync-server> 19068 ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor.dat --validator-set $a $b $x --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-x.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-x.log 2>&1 && grep -q '^validator genesis anchor failed: configured validator set differs from genesis anchor$' ${CMAKE_CURRENT_BINARY_DIR}/quorum-anchor-x.log"
)

add_test(
    NAME quorum_rejects_legacy_unanchored_genesis
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy*; $<TARGET_FILE:primechain-sequential> 3 ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy.log ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy.dat >/dev/null && a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy-c.wallet) && ! $<TARGET_FILE:primechain-sync-server> 19069 ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy-server.log 2>&1 && grep -q '^validator genesis anchor failed: quorum mode requires validator set anchored in genesis$' ${CMAKE_CURRENT_BINARY_DIR}/quorum-legacy-server.log"
)

add_test(
    NAME quorum_peer_bootstraps_anchored_genesis
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-c.wallet) && ($<TARGET_FILE:primechain-sync-server> 19070 ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-a.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-a.pid) && sleep 0.2 && hash_a=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19070 GET_STATUS | awk '{print $8}') && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19070 GET_VALIDATORS | grep -q '^VALIDATORS 3 pcpq1_' && ($<TARGET_FILE:primechain-sync-server> 19071 ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-b.dat --peer 127.0.0.1 19070 --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-b.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19071 GET_STATUS | grep -q \"^STATUS 1 1 0 1 0 2 $hash_a$\" && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-bootstrap-b.pid)"
)

add_test(
    NAME quorum_rejects_wrong_peer_genesis_before_install
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-c.wallet) && x=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-x.wallet) && ($<TARGET_FILE:primechain-sync-server> 19072 ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-a.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-a.pid) && sleep 0.2 && ! $<TARGET_FILE:primechain-sync-server> 19073 ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-b.dat --peer 127.0.0.1 19072 --validator-set $a $b $x --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-x.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-reject.log 2>&1 && grep -q 'peer genesis validator set differs from configured validator set' ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-reject.log && ($<TARGET_FILE:primechain-sync-server> 19074 ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-b.dat --validator-set $a $b $x --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-x.wallet > ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-b.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19074 GET_VALIDATORS | grep -q '^VALIDATORS 3 pcpq1_' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/quorum-wrong-peer-b.pid)"
)
add_test(
    NAME validator_epoch_tcp_workflow
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-c.wallet) && x=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-x.wallet) && next=$(printf '%s\\n%s\\n%s\\n' $a $b $c | sort | tr '\\n' ' ') && set -- $next && n1=$1 && n2=$2 && n3=$3 && ($<TARGET_FILE:primechain-sync-server> 19075 ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.pid) && sleep 0.8 && ($<TARGET_FILE:primechain-sync-server> 19078 ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.dat --peer 127.0.0.1 19075 --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.pid) && sleep 0.8 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 ADD_PEER 127.0.0.1 19078 >/dev/null && previous=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 GET_STATUS | awk '{print $8}') && vote_a=$($<TARGET_FILE:primechain-composite-commitment> sign-epoch ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-a.wallet $previous 3 1 $n1 $n2 $n3) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 $vote_a | grep -q '^EPOCH_VOTE_ACCEPTED 1 votes=1$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.pid) && sleep 0.8 && ($<TARGET_FILE:primechain-sync-server> 19075 ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.pid) && sleep 0.8 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 ADD_PEER 127.0.0.1 19078 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 GET_EPOCH_VOTES | grep -q '^EPOCH_VOTES 1 ' && vote_b=$($<TARGET_FILE:primechain-composite-commitment> sign-epoch ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.wallet $previous 3 1 $n1 $n2 $n3) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 $vote_b | grep -q '^EPOCH_VOTE_ACCEPTED 1 votes=2$' && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19075 3 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-a.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-a.wallet | grep -q 'frontier miner complete frontier=3' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 GET_VALIDATORS | grep -q \"^VALIDATORS 3 $n1 $n2 $n3$\" && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 GET_VALIDATOR_EPOCH | grep -q '^VALIDATOR_EPOCH 1 4 ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 GET_EPOCH_VOTES | grep -q '^EPOCH_VOTES 0$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.pid) && sleep 0.8 && ($<TARGET_FILE:primechain-sync-server> 19075 ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-c.wallet > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.pid) && sleep 0.8 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19075 GET_VALIDATOR_EPOCH | grep -q '^VALIDATOR_EPOCH 1 4 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/epoch-tcp-b.pid)"
)
add_test(
    NAME validator_epoch_votes_and_record_propagate
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/epoch-net*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-c.wallet) && x=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-x.wallet) && next=$(printf '%s\\n%s\\n%s\\n' $a $b $c | sort | tr '\\n' ' ') && set -- $next && n1=$1 && n2=$2 && n3=$3 && ($<TARGET_FILE:primechain-sync-server> 19076 ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19077 ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.dat --peer 127.0.0.1 19076 --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19076 ADD_PEER 127.0.0.1 19077 >/dev/null && previous=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19076 GET_STATUS | awk '{print $8}') && vote_a=$($<TARGET_FILE:primechain-composite-commitment> sign-epoch ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.wallet $previous 3 1 $n1 $n2 $n3) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19076 $vote_a | grep -q '^EPOCH_VOTE_ACCEPTED 1 votes=1$' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19077 GET_EPOCH_VOTES | grep -q '^EPOCH_VOTES 1 ' && vote_b=$($<TARGET_FILE:primechain-composite-commitment> sign-epoch ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.wallet $previous 3 1 $n1 $n2 $n3) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19077 $vote_b | grep -q '^EPOCH_VOTE_ACCEPTED 1 votes=2$' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19076 GET_EPOCH_VOTES | grep -q '^EPOCH_VOTES 2 ' && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19076 3 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.wallet | grep -q 'frontier miner complete frontier=3' && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19077 GET_VALIDATORS | grep -q \"^VALIDATORS 3 $n1 $n2 $n3$\" && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/epoch-net-b.pid)"
)

add_test(
    NAME validator_endpoint_signed_update_on_chain
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net-a.wallet) && ($<TARGET_FILE:primechain-sync-server> 19082 ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net.dat --validator-set $a --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net.pid) && sleep 0.3 && previous=$($<TARGET_FILE:primechain-sync-query> 127.0.0.1 19082 GET_STATUS | awk '{print $8}') && endpoint=$($<TARGET_FILE:primechain-composite-commitment> sign-endpoint ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net-a.wallet $previous 3 127.0.0.1 19082 1) && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19082 $endpoint | grep -q '^VALIDATOR_ENDPOINT_ACCEPTED ' && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19082 3 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net-a.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net-a.wallet | grep -q 'frontier miner complete frontier=3' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19082 GET_VALIDATOR_ENDPOINTS | grep -q '^VALIDATOR_ENDPOINT .* 127.0.0.1 19082 3 1$' && $<TARGET_FILE:primechain-client> validator-endpoints ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net.dat | grep -q '^VALIDATOR_ENDPOINT .* host=127.0.0.1 port=19082 effective_integer=3 sequence=1$' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/endpoint-net.pid)"
)

add_test(
    NAME chain_derived_candidate_startup
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/chain_derived_candidate_startup.sh $<TARGET_FILE:primechain-client> $<TARGET_FILE:primechain-sync-server> ${CMAKE_CURRENT_BINARY_DIR}/chain-derived-candidate
)

add_test(
    NAME fee_pool_distribution_to_active_validators
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/fee_pool_distribution_smoke.sh $<TARGET_FILE:primechain-client> $<TARGET_FILE:primechain-sync-server> $<TARGET_FILE:primechain-send> ${CMAKE_CURRENT_BINARY_DIR}/fee-pool-distribution
)

add_test(
    NAME validator_reserve_lock_on_chain
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/validator_reserve_lock_smoke.sh $<TARGET_FILE:primechain-client> $<TARGET_FILE:primechain-sync-server> $<TARGET_FILE:primechain-send> ${CMAKE_CURRENT_BINARY_DIR}/validator-reserve-lock
)

add_test(
    NAME validator_application_admits_candidate
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/validator_application_admission_smoke.sh $<TARGET_FILE:primechain-client> $<TARGET_FILE:primechain-sync-server> $<TARGET_FILE:primechain-send> $<TARGET_FILE:primechain-composite-commitment> ${CMAKE_CURRENT_BINARY_DIR}/validator-application-admission
)

add_test(
    NAME finalization_round_change_recovers_stalled_candidate
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/round-net*; a=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.wallet) && b=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.wallet) && c=$($<TARGET_FILE:primechain-wallet> new-miner ${CMAKE_CURRENT_BINARY_DIR}/round-net-c.wallet) && ($<TARGET_FILE:primechain-sync-server> 19079 ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.dat --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.pid) && sleep 0.2 && ! $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19079 3 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.wallet > ${CMAKE_CURRENT_BINARY_DIR}/round-net-b-miner.log 2>&1 && ($<TARGET_FILE:primechain-sync-server> 19080 ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.dat --peer 127.0.0.1 19079 --finalization-timeout-ms 50 --validator-set $a $b $c --validator-identity ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.wallet > ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.pid) && sleep 0.3 && $<TARGET_FILE:primechain-frontier-miner> 127.0.0.1 19080 3 --prime-identity ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.wallet --composite-identity ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.wallet | grep -q 'frontier miner complete frontier=3' && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19079 GET_STATUS | grep -q '^STATUS 2 2 0 1 1 3 ' && test ! -s ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.dat.finalization && test ! -s ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.dat.rounds && test ! -s ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.dat.finalization && test ! -s ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.dat.rounds && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/round-net-b.pid) && sleep 0.2 && rm -f ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.dat.finalization ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.dat.rounds && ($<TARGET_FILE:primechain-sync-server> 19081 ${CMAKE_CURRENT_BINARY_DIR}/round-net-a.dat > ${CMAKE_CURRENT_BINARY_DIR}/round-net-replay.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/round-net-replay.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19081 GET_STATUS | grep -q '^STATUS 2 2 0 1 1 3 ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/round-net-replay.pid)"
)
add_test(
    NAME sync_server_discards_corrupt_sidecar_cache
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/sync_server_sidecar_cache_recovery.sh $<TARGET_FILE:primechain-client> $<TARGET_FILE:primechain-sync-server> ${CMAKE_CURRENT_BINARY_DIR}/sync-server-sidecar-cache-recovery
)

add_test(
    NAME workdir_stale_commitment_recovers
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/workdir_stale_commitment_recovers.sh $<TARGET_FILE:primechain-client> $<TARGET_FILE:primechain-sync-server> $<TARGET_FILE:primechain-composite-commitment> ${CMAKE_CURRENT_BINARY_DIR}/workdir-stale-commitment
)

get_property(PRIMECHAIN_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
if(PRIMECHAIN_REGISTERED_TESTS)
    set_tests_properties(${PRIMECHAIN_REGISTERED_TESTS}
        PROPERTIES ENVIRONMENT "PRIMECHAIN_WALLET_PASSPHRASE=primechain-test-passphrase")
endif()
