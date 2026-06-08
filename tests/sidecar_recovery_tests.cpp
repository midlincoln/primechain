#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "primechain/storage/commitment_store.hpp"
#include "primechain/storage/finalization_store.hpp"
#include "primechain/storage/phase_store.hpp"
#include "primechain/storage/round_change_store.hpp"
#include "primechain/storage/validator_epoch_store.hpp"

namespace {

bool expect(bool condition, const std::string& name) {
    if (!condition) std::cerr << "failed: " << name << "\n";
    return condition;
}

bool exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

void writeGarbage(const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "interrupted-sidecar";
}

template <typename Store, typename Value, typename Verify>
bool testRecovery(
    const std::string& path,
    const Value& value,
    Verify verify,
    const std::string& name) {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
    Store store(path);
    std::string error;
    if (!expect(store.replaceAll({value}, error), name + " write primary")) {
        std::cerr << error << "\n";
        return false;
    }

    writeGarbage(path + ".tmp");
    error.clear();
    const auto primary = store.loadAll(error);
    if (!expect(error.empty() && primary.size() == 1 && verify(primary.front()),
            name + " primary wins over stale temp")) return false;
    if (!expect(!exists(path + ".tmp"), name + " stale temp removed")) return false;

    if (!expect(std::rename(path.c_str(), (path + ".tmp").c_str()) == 0,
            name + " simulate crash before rename")) return false;
    error.clear();
    const auto recovered = store.loadAll(error);
    if (!expect(error.empty() && recovered.size() == 1 && verify(recovered.front()),
            name + " recover valid orphan temp")) return false;
    if (!expect(exists(path) && !exists(path + ".tmp"),
            name + " promote orphan temp")) return false;

    if (!expect(std::rename(path.c_str(), (path + ".tmp").c_str()) == 0,
            name + " preserve valid temp beside corrupt primary")) return false;
    writeGarbage(path);
    error.clear();
    store.loadAll(error);
    if (!expect(!error.empty(), name + " corrupt primary remains fatal")) return false;
    if (!expect(exists(path) && !exists(path + ".tmp"),
            name + " primary cannot be overridden by temp")) return false;

    std::remove(path.c_str());
    writeGarbage(path + ".tmp");
    error.clear();
    const auto discarded = store.loadAll(error);
    if (!expect(error.empty() && discarded.empty(), name + " discard malformed orphan temp")) {
        std::cerr << error << "\n";
        return false;
    }
    if (!expect(!exists(path) && !exists(path + ".tmp"),
            name + " malformed temp removed")) return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sidecar-recovery-tests <path-prefix>\n";
        return 1;
    }
    const std::string prefix = argv[1];

    primechain::storage::StoredCommitment commitment;
    commitment.integer = 4;
    commitment.provider_address = "pcpq1_commitment";
    commitment.commitment_hash[0] = 1;
    commitment.public_key = {2, 3};
    commitment.signature = {4, 5};
    if (!testRecovery<primechain::storage::CommitmentStore>(
            prefix + ".commitments", commitment,
            [](const auto& loaded) {
                return loaded.integer == 4 && loaded.provider_address == "pcpq1_commitment" &&
                    loaded.public_key == std::vector<std::uint8_t>({2, 3});
            }, "commitment store")) return 1;

    primechain::storage::CommitPhaseVote phase;
    phase.integer = 6;
    phase.snapshot_hash[0] = 6;
    phase.validator_address = "pcpq1_phase";
    phase.public_key = {7};
    phase.signature = {8};
    if (!testRecovery<primechain::storage::PhaseStore>(
            prefix + ".phases", phase,
            [](const auto& loaded) {
                return loaded.integer == 6 && loaded.validator_address == "pcpq1_phase" &&
                    loaded.signature == std::vector<std::uint8_t>({8});
            }, "phase store")) return 1;

    primechain::storage::ValidatorEpochVoteRecord epoch;
    epoch.previous_record_hash[0] = 9;
    epoch.record_integer = 10;
    epoch.epoch = 2;
    epoch.activation_integer = 11;
    epoch.next_validator_set = {"pcpq1_a", "pcpq1_b", "pcpq1_c"};
    epoch.vote.validator_address = "pcpq1_epoch";
    epoch.vote.public_key = {12};
    epoch.vote.signature = {13};
    if (!testRecovery<primechain::storage::ValidatorEpochStore>(
            prefix + ".epochs", epoch,
            [](const auto& loaded) {
                return loaded.epoch == 2 && loaded.record_integer == 10 &&
                    loaded.next_validator_set.size() == 3 &&
                    loaded.vote.validator_address == "pcpq1_epoch";
            }, "epoch store")) return 1;

    primechain::protocol::RoundChangeVoteV1 round;
    round.validator_address = "pcpq1_round";
    round.public_key = {14};
    round.previous_record_hash[0] = 15;
    round.integer = 16;
    round.new_round = 2;
    round.signature = {17};
    if (!testRecovery<primechain::storage::RoundChangeStore>(
            prefix + ".rounds", round,
            [](const auto& loaded) {
                return loaded.integer == 16 && loaded.new_round == 2 &&
                    loaded.validator_address == "pcpq1_round";
            }, "round-change store")) return 1;

    primechain::storage::SignedCandidateRecord finalization;
    finalization.integer = 18;
    finalization.vote.validator_address = "pcpq1_finalization";
    finalization.vote.public_key = {19};
    finalization.vote.record_hash[0] = 20;
    finalization.vote.round = 1;
    finalization.vote.signature = {21};
    if (!testRecovery<primechain::storage::FinalizationStore>(
            prefix + ".finalization", finalization,
            [](const auto& loaded) {
                return loaded.integer == 18 && loaded.vote.round == 1 &&
                    loaded.vote.validator_address == "pcpq1_finalization";
            }, "finalization store")) return 1;

    std::cout << "sidecar recovery tests passed\n";
    return 0;
}
