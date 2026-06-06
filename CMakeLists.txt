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

add_executable(primechain-sync-server
    src/node/sync_server.cpp
)

target_link_libraries(primechain-sync-server
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

add_executable(primechain-sync-query
    src/tools/sync_query.cpp
)

add_executable(primechain-sync-download
    src/tools/sync_download.cpp
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
    NAME sync_server_status
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sync-500.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-500.log ${CMAKE_CURRENT_BINARY_DIR}/sync-server.log; $<TARGET_FILE:primechain-sequential> 500 ${CMAKE_CURRENT_BINARY_DIR}/sync-500.log ${CMAKE_CURRENT_BINARY_DIR}/sync-500.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18989 ${CMAKE_CURRENT_BINARY_DIR}/sync-500.dat > ${CMAKE_CURRENT_BINARY_DIR}/sync-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/sync-server.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18989 GET_STATUS | grep -q 'STATUS 499 95 404 1 498 500' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/sync-server.pid)"
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
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/submit-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/submit-src.dat ${CMAKE_CURRENT_BINARY_DIR}/submit-src.log ${CMAKE_CURRENT_BINARY_DIR}/submit-server.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/submit-alice.wallet >/dev/null && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/submit-src.log ${CMAKE_CURRENT_BINARY_DIR}/submit-src.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18993 ${CMAKE_CURRENT_BINARY_DIR}/submit-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/submit-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/submit-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/submit-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18993 ${CMAKE_CURRENT_BINARY_DIR}/submit-miner.wallet $alice 3 250000 1 | grep -q '^TX_ACCEPTED ' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/submit-server.pid)"
)
add_test(
    NAME tcp_get_mempool
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/mempool-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.dat ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.log ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/mempool-alice.wallet >/dev/null && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.log ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18994 ${CMAKE_CURRENT_BINARY_DIR}/mempool-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.pid) && sleep 0.2 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/mempool-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18994 ${CMAKE_CURRENT_BINARY_DIR}/mempool-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18994 GET_MEMPOOL | grep -q '^MEMPOOL 1' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/mempool-server.pid)"
)
add_test(
    NAME sequential_include_tcp_mempool
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/include-src.dat ${CMAKE_CURRENT_BINARY_DIR}/include-src.log ${CMAKE_CURRENT_BINARY_DIR}/include-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/include-chain.log ${CMAKE_CURRENT_BINARY_DIR}/include-server.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet >/dev/null && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/include-src.log ${CMAKE_CURRENT_BINARY_DIR}/include-src.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18995 ${CMAKE_CURRENT_BINARY_DIR}/include-src.dat > ${CMAKE_CURRENT_BINARY_DIR}/include-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/include-server.pid) && sleep 0.2 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18995 ${CMAKE_CURRENT_BINARY_DIR}/include-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sequential> 20 ${CMAKE_CURRENT_BINARY_DIR}/include-chain.log ${CMAKE_CURRENT_BINARY_DIR}/include-chain.dat --prime-miner $miner --composite-miner pcdev1_include_composite --mempool 127.0.0.1 18995 4 >/dev/null && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/include-chain.dat ${CMAKE_CURRENT_BINARY_DIR}/include-alice.wallet | grep -q '^3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18995 GET_MEMPOOL | grep -q '^MEMPOOL 0' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/include-server.pid)"
)
add_test(
    NAME tcp_advance_to_includes_mempool
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/advance-server.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18996 ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat > ${CMAKE_CURRENT_BINARY_DIR}/advance-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/advance-server.pid) && sleep 0.2 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18996 ${CMAKE_CURRENT_BINARY_DIR}/advance-miner.wallet $alice 3 250000 1 | grep -q '^TX_ACCEPTED ' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18996 ADVANCE_TO 20 $miner pcdev1_advance_composite 4 | grep -q '^ADVANCED 18 included_txs=1 frontier=20' && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/advance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/advance-alice.wallet | grep -q '^3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18996 GET_MEMPOOL | grep -q '^MEMPOOL 0' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/advance-server.pid)"
)
add_test(
    NAME sync_download_after_tcp_advance
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-downloaded.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18997 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-node.dat > ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.pid) && sleep 0.2 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18997 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18997 ADVANCE_TO 20 $miner pcdev1_sync_advance_composite 4 | grep -q '^ADVANCED 18 included_txs=1 frontier=20' && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18997 2 20 ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-downloaded.dat | grep -q 'frontier_integer: 20' && $<TARGET_FILE:primechain-wallet> balance ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-downloaded.dat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-alice.wallet | grep -q '^3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/sync-advance-server.pid)"
)
add_test(
    NAME tcp_get_balance_after_advance
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/balance-node-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/balance-node.dat ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/balance-node-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18998 ${CMAKE_CURRENT_BINARY_DIR}/balance-node.dat > ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.pid) && sleep 0.2 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/balance-node-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18998 ${CMAKE_CURRENT_BINARY_DIR}/balance-node-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18998 ADVANCE_TO 20 $miner pcdev1_balance_node_composite 4 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18998 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/balance-node-server.pid)"
)
add_test(
    NAME three_node_local_convergence
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/three-node-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.dat ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.dat ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.dat ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.log ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.log ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/three-node-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 18999 ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.dat > ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.pid) && sleep 0.2 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/three-node-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 18999 ${CMAKE_CURRENT_BINARY_DIR}/three-node-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18999 ADVANCE_TO 20 $miner pcdev1_three_node_composite 4 | grep -q '^ADVANCED 18 included_txs=1 frontier=20' && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18999 2 20 ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.dat >/dev/null && $<TARGET_FILE:primechain-sync-download> 127.0.0.1 18999 2 20 ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.dat >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19000 ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.pid) && ($<TARGET_FILE:primechain-sync-server> 19001 ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.dat > ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.pid) && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 18999 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19000 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19001 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/three-node-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/three-node-b.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/three-node-c.pid)"
)
add_test(
    NAME startup_peer_sync
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.dat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.dat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.log ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19002 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.dat > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.pid) && sleep 0.2 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 19002 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19002 ADVANCE_TO 20 $miner pcdev1_peer_sync_composite 4 | grep -q '^ADVANCED 18 included_txs=1 frontier=20' && ($<TARGET_FILE:primechain-sync-server> 19003 ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.dat --peer 127.0.0.1 19002 > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.pid) && sleep 0.3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19003 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19003 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/peer-sync-b.pid)"
)
add_test(
    NAME continuous_peer_sync
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.dat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.dat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.log ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19004 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.dat > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19005 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.dat --peer 127.0.0.1 19004 --sync-interval 1 > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.pid) && sleep 0.3 && miner=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet) && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 19004 ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-miner.wallet $alice 3 250000 1 >/dev/null && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19004 ADVANCE_TO 20 $miner pcdev1_continuous_sync_composite 4 | grep -q '^ADVANCED 18 included_txs=1 frontier=20' && sleep 3 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19005 GET_STATUS | grep -q 'STATUS 19 8 11 1 18 20' && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19005 GET_BALANCE $alice | grep -q '^HOLDING 3 250000' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/continuous-sync-b.pid)"
)
add_test(
    NAME mempool_propagates_to_peer
    COMMAND
        /bin/sh -c
        "rm -f ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet ${CMAKE_CURRENT_BINARY_DIR}/propagate-alice.wallet ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.dat ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.dat ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.log ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.log; $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet >/dev/null && $<TARGET_FILE:primechain-wallet> new ${CMAKE_CURRENT_BINARY_DIR}/propagate-alice.wallet >/dev/null && ($<TARGET_FILE:primechain-sync-server> 19006 ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.dat > ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.pid) && sleep 0.2 && ($<TARGET_FILE:primechain-sync-server> 19007 ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.dat --peer 127.0.0.1 19006 > ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.log 2>&1 & echo $! > ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.pid) && sleep 0.3 && alice=$($<TARGET_FILE:primechain-wallet> address ${CMAKE_CURRENT_BINARY_DIR}/propagate-alice.wallet) && $<TARGET_FILE:primechain-send> submit 127.0.0.1 19007 ${CMAKE_CURRENT_BINARY_DIR}/propagate-miner.wallet $alice 3 250000 1 | grep -q '^TX_ACCEPTED ' && sleep 0.2 && $<TARGET_FILE:primechain-sync-query> 127.0.0.1 19006 GET_MEMPOOL | grep -q '^MEMPOOL 1' && kill $(cat ${CMAKE_CURRENT_BINARY_DIR}/propagate-a.pid) $(cat ${CMAKE_CURRENT_BINARY_DIR}/propagate-b.pid)"
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
