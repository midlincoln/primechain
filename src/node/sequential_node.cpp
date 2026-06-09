#include "primechain/node/sequential_node.hpp"

#include <algorithm>
#include <limits>
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

bool checkedAdd(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
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

bool hasValidatorEpochTransition(const protocol::ValidatorEpochTransitionV1& transition) {
    return transition.epoch != 0 ||
           transition.activation_integer != 0 ||
           !transition.next_validator_set.empty() ||
           !transition.votes.empty();
}

bool validateValidatorEpochRecordVersion(
    std::uint64_t version,
    const protocol::ValidatorEpochTransitionV1& transition,
    std::string& error) {
    const bool has_transition = hasValidatorEpochTransition(transition);
    if ((version == 2) != has_transition) {
        error = version == 2
            ? "version 2 record requires a validator epoch transition"
            : "validator epoch transition requires record version 2";
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

std::vector<std::pair<PrimeValue, std::uint64_t>> primeFactorPairs(
    const protocol::PrattPrimeProofV0& proof) {
    std::vector<std::pair<PrimeValue, std::uint64_t>> factors;
    factors.reserve(proof.factors_of_p_minus_1.size());
    for (const auto& factor : proof.factors_of_p_minus_1) {
        factors.push_back({factor.prime, factor.exponent});
    }
    return factors;
}

bool validatePrimeProviderSignature(
    const protocol::PrattPrimeProofV0& proof,
    const Hash256& previous_record_hash,
    bool allow_development,
    std::string& error) {
    if (allow_development && proof.signature.empty() &&
        protocol::isProtocolAddress(proof.provider_address)) {
        return true;
    }
    if (!crypto::isProtocolSignatureAddress(proof.provider_address)) {
        error = "unsupported prime provider address";
        return false;
    }
    return crypto::verifyPackedPrimeProofAuthentication(
        previous_record_hash, proof.p, proof.witness, primeFactorPairs(proof),
        proof.provider_address, proof.signature, error);
}

bool validateTransactionSignature(
    const protocol::TransactionV0& tx,
    bool allow_development,
    std::string& error) {
    if (crypto::isProtocolSignatureAddress(tx.sender_address)) {
        if (tx.version != 2) {
            error = "authenticated transaction requires version 2";
            return false;
        }
        return protocol::verifyAuthenticatedTransactionSignature(tx, error);
    }
    if (allow_development && tx.version == 0 &&
        protocol::verifyDevelopmentTransactionSignature(tx)) {
        return true;
    }
    error = "transaction requires an authenticated ML-DSA-65 sender";
    return false;
}

bool validateCompositeProviderSignature(
    const protocol::CompositeProofV0& proof,
    std::string& error) {
    if (protocol::isDevelopmentAddress(proof.provider_address)) {
        return true;
    }
    if (!primechain::crypto::isProtocolSignatureAddress(proof.provider_address)) {
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
    const std::vector<Address>& validator_set,
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
    if (decoded->version == 1 && decoded->commit_phase.validator_set.empty()) {
        error = "embedded commit-phase certificate has no validator set";
        return false;
    }
    if (!protocol::verifyRecordFinalization(
            decoded->finalized_by, protocol::candidateRecordHash(*decoded),
            decoded->previous_record_hash, decoded->integer, validator_set, error)) {
        return false;
    }
    return true;
}

bool validateStoredPrimePayload(
    const storage::StoredRecord& stored,
    const Hash256& expected_previous_hash,
    const std::vector<Address>& validator_set,
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
    const bool genesis = decoded->height == 0 && decoded->integer == 2;
    if (!genesis && !validatePrimeProviderSignature(
            decoded->proof, decoded->previous_record_hash, validator_set.empty(), error)) {
        error = "invalid prime payload provider signature: " + error;
        return false;
    }
    if (!math::verifyPrattProof(toMathPrattProof(decoded->proof))) {
        error = "invalid prime payload Pratt proof";
        return false;
    }
    if (!validateTransactionBatch(decoded->tx_batch, decoded->transactions, error)) {
        return false;
    }
    if (!protocol::verifyGenesisConfig(*decoded, error)) {
        return false;
    }
    if (!protocol::verifyRecordFinalization(
            decoded->finalized_by, protocol::candidateRecordHash(*decoded),
            decoded->previous_record_hash, decoded->integer, validator_set, error)) {
        return false;
    }
    return true;
}

} // namespace

SequentialNode::SequentialNode(std::string record_store_path)
    : store_(record_store_path), snapshot_store_(record_store_path + ".snapshot") {}

bool SequentialNode::load(std::string& error) {
    status_ = {};
    balances_.clear();
    total_supply_.clear();
    account_nonces_.clear();
    pending_composite_providers_.clear();
    validator_set_.clear();
    validator_epoch_ = 0;
    loaded_from_snapshot_ = false;

    const auto latest = store_.latest(error);
    if (!error.empty()) return false;
    if (!latest.has_value()) {
        snapshot_store_.discard();
        return true;
    }

    std::uint64_t expected_height = 0;
    PrimeValue expected_integer = 2;
    Hash256 expected_previous_hash{};
    std::vector<storage::StoredRecord> records;

    storage::ReplaySnapshot snapshot;
    bool snapshot_found = false;
    std::string snapshot_error;
    if (!snapshot_store_.load(snapshot, snapshot_found, snapshot_error)) {
        snapshot_store_.discard();
        snapshot_found = false;
    }
    if (snapshot_found) {
        std::string anchor_error;
        const auto anchor = store_.findByInteger(snapshot.frontier_integer, anchor_error);
        if (anchor_error.empty() && anchor.has_value() &&
            anchor->height == snapshot.height &&
            anchor->record_hash == snapshot.record_hash &&
            snapshot.frontier_integer == snapshot.height + 2 &&
            restoreSnapshot(snapshot)) {
            loaded_from_snapshot_ = true;
            expected_height = snapshot.height + 1;
            expected_integer = snapshot.frontier_integer + 1;
            expected_previous_hash = snapshot.record_hash;
            if (latest->integer > snapshot.frontier_integer) {
                records = store_.findRange(
                    snapshot.frontier_integer + 1, latest->integer, error);
                if (!error.empty()) return false;
            }
        } else {
            snapshot_store_.discard();
        }
    }
    if (!loaded_from_snapshot_) {
        records = store_.loadAll(error);
        if (!error.empty()) return false;
    }

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
            if (!validateStoredCompositePayload(record, expected_previous_hash, validator_set_, error)) {
                return false;
            }
            const auto decoded = protocol::deserializeCompositeRecord(record.payload, error);
            if (decoded.has_value() &&
                (validator_set_.empty() ? decoded->version != 0 :
                 ((decoded->version != 1 && decoded->version != 2) ||
                  decoded->commit_phase.validator_set != validator_set_))) {
                error = "stored composite certificate validator set is not authorized by genesis";
                return false;
            }
            if (!decoded.has_value() ||
                !validateValidatorEpochRecordVersion(decoded->version, decoded->validator_epoch, error) ||
                !protocol::verifyValidatorEpochTransition(
                    decoded->validator_epoch, validator_set_, validator_epoch_,
                    decoded->previous_record_hash, decoded->integer, error)) {
                return false;
            }
            if (!decoded.has_value() ||
                !applyTransactions(decoded->transactions, decoded->proof.provider_address, error) ||
                !applyCompositeLedger(*decoded, error)) {
                return false;
            }
            if (hasValidatorEpochTransition(decoded->validator_epoch)) {
                validator_set_ = decoded->validator_epoch.next_validator_set;
                validator_epoch_ = decoded->validator_epoch.epoch;
            }
        } else {
            if (!validateStoredPrimePayload(record, expected_previous_hash, validator_set_, error)) {
                return false;
            }
            const auto decoded = protocol::deserializePrimeRecord(record.payload, error);
            if (decoded.has_value() && record.height == 0) {
                validator_set_ = decoded->genesis_config.validator_set;
            }
            if (!decoded.has_value() ||
                !validateValidatorEpochRecordVersion(decoded->version, decoded->validator_epoch, error) ||
                !protocol::verifyValidatorEpochTransition(
                    decoded->validator_epoch, validator_set_, validator_epoch_,
                    decoded->previous_record_hash, decoded->integer, error)) {
                return false;
            }
            if (!decoded.has_value() ||
                !applyTransactions(decoded->transactions, decoded->proof.provider_address, error) ||
                !applyPrimeLedger(*decoded, error)) {
                return false;
            }
            if (hasValidatorEpochTransition(decoded->validator_epoch)) {
                validator_set_ = decoded->validator_epoch.next_validator_set;
                validator_epoch_ = decoded->validator_epoch.epoch;
            }
        }

        expected_previous_hash = record.record_hash;
        ++expected_height;
        ++expected_integer;
    }

    status_.has_genesis = true;
    status_.height = latest->height;
    status_.frontier_integer = latest->integer;
    status_.latest_record_hash = latest->record_hash;
    saveSnapshot(true);
    return true;
}

bool SequentialNode::initializeGenesis(const std::vector<Address>& validator_set, std::string& error) {
    if (status_.has_genesis) {
        error = "genesis already initialized";
        return false;
    }

    const auto record = makeGenesisPrimeRecordV0(validator_set);
    if (!protocol::verifyGenesisConfig(record, error)) {
        return false;
    }
    const auto stored = storage::makeStoredRecord(record);
    if (!store_.append(stored, error)) {
        return false;
    }

    status_.has_genesis = true;
    status_.height = 0;
    status_.frontier_integer = 2;
    status_.latest_record_hash = stored.record_hash;
    validator_set_ = record.genesis_config.validator_set;
    validator_epoch_ = 0;
    if (!applyPrimeLedger(record, error)) {
        return false;
    }
    saveSnapshot(true);
    return true;
}

bool SequentialNode::validateCompositeCandidate(
    const protocol::CompositeRecordV0& record,
    std::string& error) {
    if (!validateCommon(record.height, record.integer, record.previous_record_hash, error)) return false;
    if (record.proof.g != record.integer) { error = "composite proof integer mismatch"; return false; }
    if (!protocol::isProtocolAddress(record.proof.provider_address)) { error = "invalid composite provider address"; return false; }
    if (!math::verifyCompositeProof(toLegacyCompositeProof(record.proof))) { error = "invalid composite proof"; return false; }
    if (!validateCompositeProviderSignature(record.proof, error)) { error = "invalid composite provider signature: " + error; return false; }
    if (!validateTransactionBatch(record.tx_batch, record.transactions, error)) return false;
    if (!protocol::verifyCommitPhaseCertificate(record, error)) return false;
    if (validator_set_.empty() ? record.version != 0 :
        ((record.version != 1 && record.version != 2) || record.commit_phase.validator_set != validator_set_)) {
        error = "composite certificate validator set is not authorized by genesis";
        return false;
    }
    if (!validateValidatorEpochRecordVersion(record.version, record.validator_epoch, error) ||
        !protocol::verifyValidatorEpochTransition(
            record.validator_epoch, validator_set_, validator_epoch_,
            record.previous_record_hash, record.integer, error)) return false;

    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto nonces_before = account_nonces_;
    const auto pending_before = pending_composite_providers_;
    const bool valid = applyTransactions(record.transactions, record.proof.provider_address, error) &&
        applyCompositeLedger(record, error);
    balances_ = balances_before;
    total_supply_ = total_supply_before;
    account_nonces_ = nonces_before;
    pending_composite_providers_ = pending_before;
    return valid;
}

bool SequentialNode::validatePrimeCandidate(
    const protocol::PrimeRecordV0& record,
    std::string& error) {
    if (!validateCommon(record.height, record.integer, record.previous_record_hash, error)) return false;
    if (record.proof.p != record.integer) { error = "prime proof integer mismatch"; return false; }
    if (!protocol::isProtocolAddress(record.proof.provider_address)) { error = "invalid prime provider address"; return false; }
    if (!validatePrimeProviderSignature(
            record.proof, record.previous_record_hash, validator_set_.empty(), error)) {
        error = "invalid prime provider signature: " + error;
        return false;
    }
    if (!math::verifyPrattProof(toMathPrattProof(record.proof))) { error = "invalid Pratt proof"; return false; }
    if (!validateTransactionBatch(record.tx_batch, record.transactions, error)) return false;
    if (!protocol::verifyGenesisConfig(record, error)) return false;
    if (!validateValidatorEpochRecordVersion(record.version, record.validator_epoch, error) ||
        !protocol::verifyValidatorEpochTransition(
            record.validator_epoch, validator_set_, validator_epoch_,
            record.previous_record_hash, record.integer, error)) return false;
    if (totalSupplyMicroUnits(record.integer) != 0) { error = "prime asset already minted"; return false; }

    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto nonces_before = account_nonces_;
    const auto pending_before = pending_composite_providers_;
    const bool valid = applyTransactions(record.transactions, record.proof.provider_address, error) &&
        applyPrimeLedger(record, error);
    balances_ = balances_before;
    total_supply_ = total_supply_before;
    account_nonces_ = nonces_before;
    pending_composite_providers_ = pending_before;
    return valid;
}

bool SequentialNode::appendComposite(const protocol::CompositeRecordV0& record, std::string& error) {
    if (!validateCompositeCandidate(record, error)) return false;
    if (!protocol::verifyRecordFinalization(
            record.finalized_by, protocol::candidateRecordHash(record),
            record.previous_record_hash, record.integer, validator_set_, error)) return false;

    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto nonces_before = account_nonces_;
    const auto pending_before = pending_composite_providers_;
    if (!applyTransactions(record.transactions, record.proof.provider_address, error) ||
        !applyCompositeLedger(record, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        account_nonces_ = nonces_before;
        pending_composite_providers_ = pending_before;
        return false;
    }
    const auto stored = storage::makeStoredRecord(record);
    if (!store_.append(stored, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        account_nonces_ = nonces_before;
        pending_composite_providers_ = pending_before;
        return false;
    }
    status_.height = record.height;
    status_.frontier_integer = record.integer;
    status_.latest_record_hash = stored.record_hash;
    if (hasValidatorEpochTransition(record.validator_epoch)) {
        validator_set_ = record.validator_epoch.next_validator_set;
        validator_epoch_ = record.validator_epoch.epoch;
    }
    saveSnapshot();
    return true;
}

bool SequentialNode::appendPrime(const protocol::PrimeRecordV0& record, std::string& error) {
    if (!validatePrimeCandidate(record, error)) return false;
    if (!protocol::verifyRecordFinalization(
            record.finalized_by, protocol::candidateRecordHash(record),
            record.previous_record_hash, record.integer, validator_set_, error)) return false;

    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto nonces_before = account_nonces_;
    const auto pending_before = pending_composite_providers_;
    if (!applyTransactions(record.transactions, record.proof.provider_address, error) ||
        !applyPrimeLedger(record, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        account_nonces_ = nonces_before;
        pending_composite_providers_ = pending_before;
        return false;
    }
    const auto stored = storage::makeStoredRecord(record);
    if (!store_.append(stored, error)) {
        balances_ = balances_before;
        total_supply_ = total_supply_before;
        account_nonces_ = nonces_before;
        pending_composite_providers_ = pending_before;
        return false;
    }
    status_.height = record.height;
    status_.frontier_integer = record.integer;
    status_.latest_record_hash = stored.record_hash;
    if (hasValidatorEpochTransition(record.validator_epoch)) {
        validator_set_ = record.validator_epoch.next_validator_set;
        validator_epoch_ = record.validator_epoch.epoch;
    }
    saveSnapshot();
    return true;
}

bool SequentialNode::restoreSnapshot(const storage::ReplaySnapshot& snapshot) {
    std::map<PrimeValue, std::uint64_t> reconstructed_supply;
    for (const auto& entry : snapshot.balances) {
        if (!protocol::isProtocolAddress(entry.first.first) || entry.first.second < 2) return false;
        if (entry.second > std::numeric_limits<std::uint64_t>::max() -
                reconstructed_supply[entry.first.second]) return false;
        reconstructed_supply[entry.first.second] += entry.second;
    }
    if (reconstructed_supply != snapshot.total_supply ||
        (snapshot.validator_set.empty()
            ? snapshot.validator_epoch != 0
            : snapshot.validator_set.size() != 3) ||
        !std::all_of(snapshot.pending_composite_providers.begin(),
            snapshot.pending_composite_providers.end(), protocol::isProtocolAddress) ||
        !std::all_of(snapshot.validator_set.begin(), snapshot.validator_set.end(),
            crypto::isProtocolSignatureAddress) ||
        !std::all_of(snapshot.account_nonces.begin(), snapshot.account_nonces.end(),
            [](const auto& entry) { return protocol::isProtocolAddress(entry.first); }) ||
        !std::is_sorted(snapshot.validator_set.begin(), snapshot.validator_set.end()) ||
        std::adjacent_find(snapshot.validator_set.begin(), snapshot.validator_set.end()) !=
            snapshot.validator_set.end()) return false;

    status_.has_genesis = true;
    status_.height = snapshot.height;
    status_.frontier_integer = snapshot.frontier_integer;
    status_.latest_record_hash = snapshot.record_hash;
    balances_ = snapshot.balances;
    total_supply_ = snapshot.total_supply;
    account_nonces_ = snapshot.account_nonces;
    pending_composite_providers_ = snapshot.pending_composite_providers;
    validator_set_ = snapshot.validator_set;
    validator_epoch_ = snapshot.validator_epoch;
    return true;
}

void SequentialNode::saveSnapshot(bool force) const {
    if (!status_.has_genesis) return;
    constexpr std::uint64_t kSnapshotInterval = 256;
    if (!force && status_.height % kSnapshotInterval != 0) return;
    storage::ReplaySnapshot snapshot;
    snapshot.height = status_.height;
    snapshot.frontier_integer = status_.frontier_integer;
    snapshot.record_hash = status_.latest_record_hash;
    snapshot.balances = balances_;
    snapshot.total_supply = total_supply_;
    snapshot.account_nonces = account_nonces_;
    snapshot.pending_composite_providers = pending_composite_providers_;
    snapshot.validator_set = validator_set_;
    snapshot.validator_epoch = validator_epoch_;
    std::string ignored;
    snapshot_store_.replace(snapshot, ignored);
}

std::uint64_t SequentialNode::balanceMicroUnits(const Address& address, PrimeValue prime) const {
    const auto found = balances_.find({address, prime});
    if (found == balances_.end()) {
        return 0;
    }
    return found->second;
}

std::uint64_t SequentialNode::accountNonce(const Address& address) const {
    const auto found = account_nonces_.find(address);
    return found == account_nonces_.end() ? 0 : found->second;
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

bool SequentialNode::validatePendingTransactions(
    const std::vector<protocol::TransactionV0>& transactions,
    std::string& error) {
    const auto balances_before = balances_;
    const auto total_supply_before = total_supply_;
    const auto nonces_before = account_nonces_;
    const bool valid = applyTransactions(transactions, {}, error);
    balances_ = balances_before;
    total_supply_ = total_supply_before;
    account_nonces_ = nonces_before;
    return valid;
}

bool SequentialNode::applyTransactions(
    const std::vector<protocol::TransactionV0>& transactions,
    const Address& fee_recipient,
    std::string& error) {
    std::map<PrimeValue, std::uint64_t> collected_fees;
    for (const auto& tx : transactions) {
        if (!validateTransactionSignature(tx, validator_set_.empty(), error)) {
            error = "invalid transaction signature: " + error;
            return false;
        }
        if (tx.inputs.empty() || tx.outputs.empty()) {
            error = "transaction must have inputs and outputs";
            return false;
        }
        const auto expected_nonce = accountNonce(tx.sender_address) + 1;
        if (expected_nonce == 0 || tx.nonce != expected_nonce) {
            error = "transaction nonce must be the next sender nonce";
            return false;
        }
        if (tx.fee.amount.denominator != 1 ||
            (tx.fee.amount.numerator != 0 && tx.fee.prime < 2)) {
            error = "transaction fee must be integer micro-units of a valid prime asset";
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
            if (!checkedAdd(debits[input.prime], *units)) {
                error = "transaction input amount overflow";
                return false;
            }
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
            if (!checkedAdd(credits[output.prime], *units)) {
                error = "transaction output amount overflow";
                return false;
            }
        }
        if (tx.fee.amount.numerator != 0 &&
            !checkedAdd(credits[tx.fee.prime], tx.fee.amount.numerator)) {
            error = "transaction fee amount overflow";
            return false;
        }
        if (debits != credits) {
            error = "transaction inputs must equal outputs plus fee for each prime asset";
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
        if (tx.fee.amount.numerator != 0 &&
            !checkedAdd(collected_fees[tx.fee.prime], tx.fee.amount.numerator)) {
            error = "transaction fee batch overflow";
            return false;
        }
        account_nonces_[tx.sender_address] = tx.nonce;
    }
    if (!fee_recipient.empty()) {
        for (const auto& fee : collected_fees) {
            credit(fee_recipient, fee.first, fee.second);
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

protocol::PrimeRecordV0 makeGenesisPrimeRecordV0(const std::vector<Address>& validator_set) {
    protocol::PrimeRecordV0 record;
    record.version = validator_set.empty() ? 0 : 1;
    record.height = 0;
    record.previous_record_hash = {};
    record.integer = 2;
    record.proof.p = 2;
    record.proof.witness = 0;
    record.proof.provider_address = "pcdev1_genesis";
    record.genesis_config.validator_set = validator_set;
    std::sort(record.genesis_config.validator_set.begin(), record.genesis_config.validator_set.end());
    protocol::applyDevelopmentFinalization(record);
    return record;
}

} // namespace primechain::node
