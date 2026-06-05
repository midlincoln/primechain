#include <iostream>
#include <string>

#include "primechain/node/sequential_node.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <record-store.dat> <address>\n";
        return 1;
    }

    primechain::node::SequentialNode node(argv[1]);
    std::string error;
    if (!node.load(error)) {
        std::cerr << "could not load record store: " << error << "\n";
        return 1;
    }

    const std::string address = argv[2];
    const auto holdings = node.holdingsForAddress(address);
    std::cout << "address: " << address << "\n";
    std::cout << "holdings: " << holdings.size() << "\n";
    for (const auto& holding : holdings) {
        std::cout << holding.first << " " << holding.second << "\n";
    }
    return 0;
}
