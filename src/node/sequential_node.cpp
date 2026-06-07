#include "primechain/node/sequential_node.hpp"

#include <map>
#include <utility>

#include "primechain/crypto/signature.hpp"
#include "primechain/math/number_theory.hpp"

namespace primechain::node {

namespace {

std::optional<std::uint64_t> microUnits(const protocol::Amount& amount) {
    if (amount.denominator != 1 || amount.numerator == 0) {
        return std::nullopt;
    }
    return amount.numerator;
}

bool validateTransactionBatch(
    const protocol::TransactionBatchV0& batch,
    const std::vector<protocol::TransactionV0>& transactions,
    std::string& error) {
    if (batch.transaction_count != transactions.size()) {
        error = "transaction batch count mismatch";
        return false;
    }
    if (batch.transaction_merkle_root != protocol::transactionMerkleRoot(transactions)) {
        error = "transaction batch root mismatch";
        return false;
    }
    return true;
}

math::Factorization toMathFactorization(const std::vector<protocol::PrimePowerV0>& factors) {
    math::Factorization out;
    for (const auto& factor : factors) {
        out.factors.push_back({factor.prime, factor.exponent});
    }
    return out;
}

math::PrattProof toMathPrattProof(const protocol::PrattPrimeProofV0& proof) {
    math::PrattProof out;
    out.p = proof.p;
    out.witness = proof.witness;
    out.factors_of_p_minus_1 = toMathFactorization(proof.factors_of_p_minus_1);
    return out;
}

CompositeProof toLegacyCompositeProof(const protocol::CompositeProofV0& proof) {
    CompositeProof out;
    out.m = proof.g;
    out.d = proof.d;
    out.e = proof.e;
    out.provider_address = proof.provider_address;
    out.signature = proof.signature;
    return out;
}

bool validateCompositeProviderSignature(
    const protocol::CompositeProofV0& proof,
    std::string& error) {
    if (protocol::isDevelopmentAddress(proof.provider_address)) {
        return true;
    }
    if (!primechain::crypto::isEd25519Address(proof.provider_address)) {
        error = "unsupported composite provider address";
        return false;
    }
    return primechain::crypto::verifyPackedCompositeRevealProof(
        proof.g,
        proof.d,
        proof.e,
        proof.provider_address,
        proof.signature,
        error);
}

bool validateStoredCompositePayload(
    const storage::StoredRecord& stored,
    const Hash256& expected_previous_hash,
    std::string& error) {
    const auto decoded = protocol::deserializeCompositeRecord(stored.payload, error);
    if (!decoded.has_value()) {
        return false;
    }
    if (decoded->height != stored.height || decoded->integer != stored.integer) {
        error = "composite payload metadata does not match store envelope";
        return false;
    }
    if (decoded->previous_record_hash != expected_previous_hash) {
        error = "composite payload previous hash mismatch";
        return false;
    }
    if (decoded->proof.g != decoded->integer) {
        error = "composite payload proof integer mismatch";
        return false;
    }
    if (!protocol::isProtocolAddress(decoded->proof.provider_address)) {
        error = "invalid composite payload provider address";
        return false;
    }
    if (!math::verifyCompositeProof(toLegacyCompositeProof(decoded->proof))) {
        error = "invalid composite payload proof";
        return false;
    }
    if (!validateCompositeProviderSignature(decoded->proof, error)) {
        error = "invalid composite payload provider signature: " + error;
        return false;
    }
    if (!validateTransactionBatch(decoded->tx_batch, decoded->transactions, error)) {
        return false;
    }
    if (!protocol::verifyCommitPhaseCertificate(*decoded, error)) {
        return false;
    }
    if (!protocol::verifyDevelopmentFinalization(decoded->finalized_by, protocol::candidateRecordHash(*decoded), error)) {
        return false;
    }
    return true;
}

bool validateStoredPrimePayload(
    const storage::StoredRecord& stored,
    const Hash256& expected_previous_hash,
    std::string& error) {
    const auto decoded = protocol::deserializePrimeRecord(stored.payload, error);
    if (!decoded.has_value()) {
        return false;
    }
    if (decoded->height != stored.height || decoded->integer != stored.integer) {
        error = "prime payload metadata does not match store envelope";
        return false;
    }
    if (decoded->previous_record_hash != expected_previous_hash) {
        error = "prime payload previous hash mismatch";
        return false;
    }
    if (decoded->proof.p != decoded->integer) {
        error = "prime payload proof integer mismatch";
        return false;
    }
    if (!protocol::isProtocolAddress(decoded->proof.provider_address)) {
        error = "invalid prime payload provider address";
        return false;
    }
    if (!math::verifyPrattProof(toMathPrattProof(decoded->proof))) {
        error = "invalid prime payload Pratt proof";
        return false;
    }
    if (!validateTransactionBatch(decoded->tx_batch, decoded->transactions, error)) {
        return false;
    }
    if (!protocol::verifyDevelopmentFinalization(decoded->finalized_by, protocol::candidateRecordHash(*decoded), error)) {
        return false;
    }
    return true;
}

} // namespace

SequentialNode::SequentialNode(std::string record_store_path)
    : store_(std::move(record_store_path)) {}

bool SequentialNode::load(std::string& error) {
    const auto records = store_.loadAll(error);
    if (!error.empty()) {
        return false;
    }

    status_ = {};
    balances_.clear();
    total_supply_.clear();
    pending_composite_providers_.clear();
    if (records.empty()) {
        return true;
    }

    std::uint64_t expected_height = 0;
    PrimeValue expected_integer = 2;
    Hash256 expected_previous_hash{};

    for (const auto& record : records) {
        if (record.height != expected_height) {
            error = "stored record height is not sequential";
            return false;
        }
        if (record.integer != expected_integer) {
            error = "stored record integer is not sequential";
            return false;
        }
        if (record.height == 0 && record.kind != storage::StoredRecordKind::Prime) {
            error = "genesis record must be prime";
            return false;
        }
        if (record.kind == storage::StoredRecordKind::Composite) {
            if (!validateStoredCompositePayload(record, expected_previous_hash, error)) {
                return false;
            }
            const auto decoded = protocol::deserializeCompositeRecord(record.payload, error);
            if (!decoded.has_value() ||
                !applyTransactions(decoded->transactions, error) ||
                !applyCompositeLedger(*decoded, error)) {
                return false;
            }
        } else {
            if (!validateStoredPrimePayload(record, expected_previous_hash, error)) {
                return false;
            }
            const auto decoded = protocol::deserializePrimeRecord(record.payload, error);
            if (!decoded.has_value() ||
                !applyTransactions(decoded->transactions, error) ||
                !applyPrimeLedger(*decoded, error)) {
                return false;
            }
        }

        expected_previous_hash = record.record_hash;
        ++expected_height;
        ++expected_integer;
    }

    const auto& latest = records.back();
    status_.has_genesis = true;
    status_.height = latest.height;
    status_.frontier_integer = latest.integer;
    status_.latest_record_hash = latest.record_hash;
    return true;
}

bool SequentialNode::initializeGenesis(std::string& error) {
    if (status_.has_genesis) {
        error = "genesis already initialized";
        return false;
    }

    const auto record = makeGenesisPrimeRecordV0();
    const auto stored = storage::makeStoredRecord(record);
    if (!store_.append(stored, error)) {
        return false;
    }

    status_.has_genesis = true;
    status_.height = 0;
    status_.frontier_integer = 2;
    status_.latest_record_hash = stored.record_hash;
    if (!applyPrimeLedger(record, error)) {
        return false;
    }
    return true;
}

bool SequentialNode::appendComposite(const protocol::CompositeRecordV0& record, std::string& error) {
    if (!validateCommon(record.height, record.integer, record.previous_record_hash, error)) {
        return false;
    }
    if (record.proof.g != record.integer) {
        error = "composite proof integer mismatch";
        return false;
    }
    if (!protocol::isProtocolAddress(record.proof.provider_address)) {
        error = "invalid composite provider address";
        return false;
    }
    if (!math::verifyCompositeProof(toLegacyCompositeProof(record.proof))) {
        error = "invalid composite proof";
        return false;
    }
    if (!validateCompositeProviderSignature(record.proof, error)) {
        error = "invalid composite provider signature: " + error;
        return false;
    }
    if (!validateTransactionBatch(record.tx_batch, record.transactions, error)) {
        return false;
    }
    if (!protocol::verifyCommitPhaseCertificate(record, error)) {
        return false;
    }
    if (!protocol::verifyDevelopmentFinalization(record.finalized_by, protocol::candidateRecordHash(record), error)) {
        return false;
    }

    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto pending_before = pending_composite_providers_;
    if (!applyTransactions(record.transactions, error)) {
        return false;
    }
    if (!applyCompositeLedger(record, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        pending_composite_providers_ = pending_before;
        return false;
    }

    const auto stored = storage::makeStoredRecord(record);
    if (!store_.append(stored, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        pending_composite_providers_ = pending_before;
        return false;
    }

    status_.height = record.height;
    status_.frontier_integer = record.integer;
    status_.latest_record_hash = stored.record_hash;
    return true;
}

bool SequentialNode::appendPrime(const protocol::PrimeRecordV0& record, std::string& error) {
    if (!validateCommon(record.height, record.integer, record.previous_record_hash, error)) {
        return false;
    }
    if (record.proof.p != record.integer) {
        error = "prime proof integer mismatch";
        return false;
    }
    if (!protocol::isProtocolAddress(record.proof.provider_address)) {
        error = "invalid prime provider address";
        return false;
    }
    if (!math::verifyPrattProof(toMathPrattProof(record.proof))) {
        error = "invalid Pratt proof";
        return false;
    }
    if (!validateTransactionBatch(record.tx_batch, record.transactions, error)) {
        return false;
    }
    if (!protocol::verifyDevelopmentFinalization(record.finalized_by, protocol::candidateRecordHash(record), error)) {
        return false;
    }
    if (totalSupplyMicroUnits(record.integer) != 0) {
        error = "prime asset already minted";
        return false;
    }

    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto pending_before = pending_composite_providers_;
    if (!applyTransactions(record.transactions, error)) {
        return false;
    }
    if (!applyPrimeLedger(record, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        pending_composite_providers_ = pending_before;
        return false;
    }

    const auto stored = storage::makeStoredRecord(record);
    if (!store_.append(stored, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        pending_composite_providers_ = pending_before;
        return false;
    }

    status_.height = record.height;
    status_.frontier_integer = record.integer;
    status_.latest_record_hash = stored.record_hash;
    return true;
}

std::uint64_t SequentialNode::balanceMicroUnits(const Address& address, PrimeValue prime) const {
    const auto found = balances_.find({address, prime});
    if (found == balances_.end()) {
        return 0;
    }
    return found->second;
}

std::uint64_t SequentialNode::totalSupplyMicroUnits(PrimeValue prime) const {
    const auto found = total_supply_.find(prime);
    if (found == total_supply_.end()) {
        return 0;
    }
    return found->second;
}

std::vector<std::pair<PrimeValue, std::uint64_t>> SequentialNode::holdingsForAddress(const Address& address) const {
    std::vector<std::pair<PrimeValue, std::uint64_t>> out;
    for (const auto& entry : balances_) {
        if (entry.first.first == address && entry.second > 0) {
            out.push_back({entry.first.second, entry.second});
        }
    }
    return out;
}

bool SequentialNode::applyTransactions(const std::vector<protocol::TransactionV0>& transactions, std::string& error) {
    for (const auto& tx : transactions) {
        if (!protocol::isDevelopmentAddress(tx.sender_address) ||
            !protocol::verifyDevelopmentTransactionSignature(tx)) {
            error = "invalid development transaction signature";
            return false;
        }
        if (tx.inputs.empty() || tx.outputs.empty()) {
            error = "transaction must have inputs and outputs";
            return false;
        }
        if (tx.fee.amount.numerator != 0 || tx.fee.amount.denominator != 1) {
            error = "non-zero fees are not supported yet";
            return false;
        }

        std::map<PrimeValue, std::uint64_t> debits;
        std::map<PrimeValue, std::uint64_t> credits;
        for (const auto& input : tx.inputs) {
            const auto units = microUnits(input.amount);
            if (!units.has_value()) {
                error = "transaction input amount must be positive integer micro-units";
                return false;
            }
            debits[input.prime] += *units;
        }
        for (const auto& output : tx.outputs) {
            if (!protocol::isProtocolAddress(output.receiver_address)) {
                error = "invalid receiver address";
                return false;
            }
            const auto units = microUnits(output.amount);
            if (!units.has_value()) {
                error = "transaction output amount must be positive integer micro-units";
                return false;
            }
            credits[output.prime] += *units;
        }
        if (debits != credits) {
            error = "transaction debits and credits do not balance";
            return false;
        }

        for (const auto& debit_entry : debits) {
            if (!debit(tx.sender_address, debit_entry.first, debit_entry.second, error)) {
                return false;
            }
        }
        for (const auto& output : tx.outputs) {
            credit(output.receiver_address, output.prime, output.amount.numerator);
        }
    }
    return true;
}

bool SequentialNode::applyCompositeLedger(const protocol::CompositeRecordV0& record, std::string& error) {
    (void)error;
    pending_composite_providers_.push_back(record.proof.provider_address);
    return true;
}

bool SequentialNode::applyPrimeLedger(const protocol::PrimeRecordV0& record, std::string& error) {
    if (totalSupplyMicroUnits(record.integer) != 0) {
        error = "prime asset already minted";
        return false;
    }

    if (pending_composite_providers_.empty()) {
        credit(record.proof.provider_address, record.integer, kAssetMicroUnits);
    } else {
        constexpr std::uint64_t prime_reward = kAssetMicroUnits / 2;
        const std::uint64_t composite_pool = kAssetMicroUnits - prime_reward;
        const std::uint64_t per_composite = composite_pool / pending_composite_providers_.size();
        const std::uint64_t remainder = composite_pool % pending_composite_providers_.size();

        credit(record.proof.provider_address, record.integer, prime_reward + remainder);
        for (const auto& provider : pending_composite_providers_) {
            credit(provider, record.integer, per_composite);
        }
    }

    pending_composite_providers_.clear();
    if (totalSupplyMicroUnits(record.integer) != kAssetMicroUnits) {
        error = "prime asset reward allocation does not conserve supply";
        return false;
    }
    return true;
}

void SequentialNode::credit(const Address& address, PrimeValue prime, std::uint64_t micro_units) {
    balances_[{address, prime}] += micro_units;
    total_supply_[prime] += micro_units;
}

bool SequentialNode::debit(const Address& address, PrimeValue prime, std::uint64_t micro_units, std::string& error) {
    const auto key = std::make_pair(address, prime);
    const auto found = balances_.find(key);
    if (found == balances_.end() || found->second < micro_units) {
        error = "insufficient balance";
        return false;
    }
    found->second -= micro_units;
    total_supply_[prime] -= micro_units;
    return true;
}

bool SequentialNode::validateCommon(
    std::uint64_t height,
    PrimeValue integer,
    const Hash256& previous_record_hash,
    std::string& error) const {
    if (!status_.has_genesis) {
        error = "genesis is not initialized";
        return false;
    }
    if (height != status_.height + 1) {
        error = "record height is not next height";
        return false;
    }
    if (integer != status_.frontier_integer + 1) {
        error = "record integer is not next frontier integer";
        return false;
    }
    if (previous_record_hash != status_.latest_record_hash) {
        error = "previous record hash mismatch";
        return false;
    }
    return true;
}

protocol::PrimeRecordV0 makeGenesisPrimeRecordV0() {
    protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = 0;
    record.previous_record_hash = {};
    record.integer = 2;
    record.proof.p = 2;
    record.proof.witness = 0;
    record.proof.provider_address = "pcdev1_genesis";
    protocol::applyDevelopmentFinalization(record);
    return record;
}

} // namespace primechain::node
