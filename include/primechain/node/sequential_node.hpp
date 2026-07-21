#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "primechain/protocol/records.hpp"
#include "primechain/storage/record_store.hpp"
#include "primechain/storage/replay_snapshot_store.hpp"
#include "primechain/types.hpp"

namespace primechain::node {

struct SequentialNodeStatus {
    bool has_genesis{false};
    std::uint64_t height{0};
    PrimeValue frontier_integer{0};
    Hash256 latest_record_hash{};
};

constexpr std::uint64_t kAssetMicroUnits = 1000000;

class SequentialNode {
public:
    explicit SequentialNode(std::string record_store_path);

    bool load(std::string& error);
    bool initializeGenesis(const std::vector<Address>& validator_set, std::string& error);
    bool initializeGenesis(std::string& error) { return initializeGenesis({}, error); }

    bool validateCompositeCandidate(const protocol::CompositeRecordV0& record, std::string& error);
    bool validatePrimeCandidate(const protocol::PrimeRecordV0& record, std::string& error);
    bool appendComposite(const protocol::CompositeRecordV0& record, std::string& error);
    bool appendPrime(const protocol::PrimeRecordV0& record, std::string& error);

    const SequentialNodeStatus& status() const { return status_; }
    std::uint64_t balanceMicroUnits(const Address& address, PrimeValue prime) const;
    std::uint64_t accountNonce(const Address& address) const;
    std::vector<std::pair<PrimeValue, std::uint64_t>> holdingsForAddress(const Address& address) const;
    std::uint64_t totalSupplyMicroUnits(PrimeValue prime) const;
    bool validatePendingTransactions(
        const std::vector<protocol::TransactionV0>& transactions,
        std::string& error);
    const std::vector<Address>& validatorSet() const { return validator_set_; }
    std::uint64_t validatorEpoch() const { return validator_epoch_; }
    std::uint64_t transferFeeMicroUnits() const { return transfer_fee_micro_units_; }
    Address validatorFeePoolAddress() const;
    bool loadedFromSnapshot() const { return loaded_from_snapshot_; }

private:
    bool validateCommon(
        std::uint64_t height,
        PrimeValue integer,
        const Hash256& previous_record_hash,
        std::string& error) const;
    bool applyTransactions(
        const std::vector<protocol::TransactionV0>& transactions,
        const Address& fee_recipient,
        std::string& error);
    bool applyCompositeLedger(const protocol::CompositeRecordV0& record, std::string& error);
    bool applyPrimeLedger(const protocol::PrimeRecordV0& record, std::string& error);
    bool applyEconomicPolicy(
        const protocol::EconomicPolicyUpdateV1& update,
        PrimeValue record_integer,
        std::string& error);
    bool restoreSnapshot(const storage::ReplaySnapshot& snapshot);
    void saveSnapshot(bool force = false) const;
    void credit(const Address& address, PrimeValue prime, std::uint64_t micro_units);
    bool debit(const Address& address, PrimeValue prime, std::uint64_t micro_units, std::string& error);

    storage::RecordStore store_;
    storage::ReplaySnapshotStore snapshot_store_;
    SequentialNodeStatus status_;
    std::map<std::pair<Address, PrimeValue>, std::uint64_t> balances_;
    std::map<PrimeValue, std::uint64_t> total_supply_;
    std::map<Address, std::uint64_t> account_nonces_;
    std::vector<Address> pending_composite_providers_;
    std::vector<Address> validator_set_;
    std::uint64_t validator_epoch_{0};
    std::uint64_t transfer_fee_micro_units_{1};
    bool loaded_from_snapshot_{false};
};

protocol::PrimeRecordV0 makeGenesisPrimeRecordV0(const std::vector<Address>& validator_set = {});

} // namespace primechain::node
