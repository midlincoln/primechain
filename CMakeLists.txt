cmake_minimum_required(VERSION 3.10)

project(primechain
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(primechain_core
    src/core/consensus.cpp
    src/crypto/hash.cpp
    src/math/number_theory.cpp
    src/node/sequential_node.cpp
    src/protocol/records.cpp
    src/storage/record_store.cpp
)

target_include_directories(primechain_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(primechain_core
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_executable(primechain-node
    src/node/main.cpp
)

target_link_libraries(primechain-node
    PRIVATE
        primechain_core
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

enable_testing()
add_test(NAME consensus COMMAND primechain-tests)
add_test(
    NAME arithmetic_bench_output
    COMMAND
        /bin/sh -c
        "$<TARGET_FILE:primechain-arithmetic-bench> 20 7 3 ${CMAKE_CURRENT_BINARY_DIR}/arithmetic-bench-test.log >/dev/null && $<TARGET_FILE:primechain-arithmetic-bench-tests> ${CMAKE_CURRENT_BINARY_DIR}/arithmetic-bench-test.log"
)
add_test(NAME protocol_records COMMAND primechain-protocol-record-tests)
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
    NAME record_store
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/record-store-test.dat && $<TARGET_FILE:primechain-record-store-tests> ${CMAKE_CURRENT_BINARY_DIR}/record-store-test.dat"
)
add_test(
    NAME sequential_node
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sequential-node-test.dat && $<TARGET_FILE:primechain-sequential-node-tests> ${CMAKE_CURRENT_BINARY_DIR}/sequential-node-test.dat"
)
