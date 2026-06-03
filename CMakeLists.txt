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

add_executable(primechain-tests
    tests/consensus_tests.cpp
)

target_link_libraries(primechain-tests
    PRIVATE
        primechain_core
)

enable_testing()
add_test(NAME consensus COMMAND primechain-tests)
