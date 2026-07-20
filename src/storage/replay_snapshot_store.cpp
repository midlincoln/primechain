#include "primechain/storage/replay_snapshot_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

#include "primechain/crypto/hash.hpp"
#include "primechain/storage/atomic_file.hpp"

namespace primechain::storage {
namespace {

constexpr std::uint64_t kMagic = 0x3150414e53434350ull; // "PCCSNAP1"
constexpr std::uint64_t kVersion = 2;
constexpr std::uint64_t kMaxEntries = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxAddressBytes = 64ull * 1024ull;
constexpr std::uint64_t kMaxSnapshotBytes = 512ull * 1024ull * 1024ull;

void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back((value >> (i * 8)) & 0xffu);
}

void putAddress(std::vector<std::uint8_t>& out, const Address& value) {
    put64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

class Reader {
public:
    Reader(const std::vector<std::uint8_t>& bytes, std::size_t limit)
        : bytes_(bytes), limit_(limit) {}

    bool u64(std::uint64_t& value) {
        if (remaining() < 8) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) value |= std::uint64_t(bytes_[offset_++]) << (i * 8);
        return true;
    }
    bool hash(Hash256& value) {
        if (remaining() < value.size()) return false;
        std::memcpy(value.data(), bytes_.data() + offset_, value.size());
        offset_ += value.size();
        return true;
    }
    bool address(Address& value) {
        std::uint64_t size = 0;
        if (!u64(size) || size > kMaxAddressBytes || size > remaining()) return false;
        value.assign(bytes_.begin() + offset_, bytes_.begin() + offset_ + size);
        offset_ += size;
        return true;
    }
    bool count(std::uint64_t& value) { return u64(value) && value <= kMaxEntries; }
    bool done() const { return offset_ == limit_; }

private:
    std::size_t remaining() const { return limit_ - offset_; }
    const std::vector<std::uint8_t>& bytes_;
    std::size_t limit_;
    std::size_t offset_{0};
};

std::vector<std::uint8_t> encode(const ReplaySnapshot& value) {
    std::vector<std::uint8_t> out;
    put64(out, kMagic); put64(out, kVersion); put64(out, value.height);
    put64(out, value.frontier_integer);
    out.insert(out.end(), value.record_hash.begin(), value.record_hash.end());
    put64(out, value.balances.size());
    for (const auto& item : value.balances) {
        putAddress(out, item.first.first); put64(out, item.first.second); put64(out, item.second);
    }
    put64(out, value.total_supply.size());
    for (const auto& item : value.total_supply) { put64(out, item.first); put64(out, item.second); }
    put64(out, value.account_nonces.size());
    for (const auto& item : value.account_nonces) { putAddress(out, item.first); put64(out, item.second); }
    put64(out, value.pending_composite_providers.size());
    for (const auto& address : value.pending_composite_providers) putAddress(out, address);
    put64(out, value.validator_set.size());
    for (const auto& address : value.validator_set) putAddress(out, address);
    put64(out, value.validator_epoch);
    put64(out, value.transfer_fee_micro_units);
    const auto checksum = crypto::sha3_256(out);
    out.insert(out.end(), checksum.begin(), checksum.end());
    return out;
}

bool decode(const std::string& path, ReplaySnapshot& result, std::string& error) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > kMaxSnapshotBytes) {
        error = "invalid replay snapshot size";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "could not open replay snapshot"; return false; }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    if (bytes.size() < Hash256{}.size()) {
        error = "could not read replay snapshot"; return false;
    }
    const std::size_t payload_size = bytes.size() - Hash256{}.size();
    const std::vector<std::uint8_t> payload(bytes.begin(), bytes.begin() + payload_size);
    const auto checksum = crypto::sha3_256(payload);
    if (!std::equal(checksum.begin(), checksum.end(), bytes.begin() + payload_size)) {
        error = "replay snapshot checksum mismatch"; return false;
    }
    Reader reader(bytes, payload_size);
    ReplaySnapshot value;
    std::uint64_t magic = 0, version = 0, count = 0;
    if (!reader.u64(magic) || magic != kMagic || !reader.u64(version) || (version != 1 && version != kVersion) ||
        !reader.u64(value.height) || !reader.u64(value.frontier_integer) || !reader.hash(value.record_hash)) {
        error = "invalid replay snapshot header"; return false;
    }
    if (!reader.count(count)) { error = "invalid replay snapshot balance count"; return false; }
    for (std::uint64_t i = 0; i < count; ++i) {
        Address address; PrimeValue prime = 0; std::uint64_t amount = 0;
        if (!reader.address(address) || !reader.u64(prime) || !reader.u64(amount) ||
            !value.balances.emplace(std::make_pair(address, prime), amount).second) {
            error = "invalid replay snapshot balance"; return false;
        }
    }
    if (!reader.count(count)) { error = "invalid replay snapshot supply count"; return false; }
    for (std::uint64_t i = 0; i < count; ++i) {
        PrimeValue prime = 0; std::uint64_t amount = 0;
        if (!reader.u64(prime) || !reader.u64(amount) || !value.total_supply.emplace(prime, amount).second) {
            error = "invalid replay snapshot supply"; return false;
        }
    }
    if (!reader.count(count)) { error = "invalid replay snapshot nonce count"; return false; }
    for (std::uint64_t i = 0; i < count; ++i) {
        Address address; std::uint64_t nonce = 0;
        if (!reader.address(address) || !reader.u64(nonce) ||
            !value.account_nonces.emplace(std::move(address), nonce).second) {
            error = "invalid replay snapshot nonce"; return false;
        }
    }
    if (!reader.count(count)) { error = "invalid replay snapshot provider count"; return false; }
    for (std::uint64_t i = 0; i < count; ++i) {
        Address address;
        if (!reader.address(address)) { error = "invalid replay snapshot provider"; return false; }
        value.pending_composite_providers.push_back(std::move(address));
    }
    if (!reader.count(count) || count > 16) { error = "invalid replay snapshot validator count"; return false; }
    for (std::uint64_t i = 0; i < count; ++i) {
        Address address;
        if (!reader.address(address)) { error = "invalid replay snapshot validator"; return false; }
        value.validator_set.push_back(std::move(address));
    }
    if (!reader.u64(value.validator_epoch)) {
        error = "invalid replay snapshot trailing data"; return false;
    }
    if (version >= 2) {
        if (!reader.u64(value.transfer_fee_micro_units)) {
            error = "invalid replay snapshot policy"; return false;
        }
    } else {
        value.transfer_fee_micro_units = 1;
    }
    if (value.transfer_fee_micro_units == 0 || !reader.done()) {
        error = "invalid replay snapshot trailing data"; return false;
    }
    result = std::move(value);
    return true;
}

} // namespace

ReplaySnapshotStore::ReplaySnapshotStore(std::string path) : path_(std::move(path)) {}

bool ReplaySnapshotStore::load(ReplaySnapshot& snapshot, bool& found, std::string& error) const {
    found = false;
    if (!detail::prepareAtomicLoad(path_, [](const std::string& path, std::string& validation_error) {
            ReplaySnapshot ignored;
            return decode(path, ignored, validation_error);
        }, error)) return false;
    std::ifstream probe(path_, std::ios::binary);
    if (!probe) return true;
    if (!decode(path_, snapshot, error)) return false;
    found = true;
    return true;
}

bool ReplaySnapshotStore::replace(const ReplaySnapshot& snapshot, std::string& error) const {
    const auto bytes = encode(snapshot);
    const std::string temp = detail::uniqueAtomicTempPath(path_);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) { error = "could not open temporary replay snapshot"; return false; }
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    if (!out) { error = "could not write temporary replay snapshot"; return false; }
    return detail::commitAtomicTemp(temp, path_, "replay snapshot", error);
}

void ReplaySnapshotStore::discard() const {
    std::remove(path_.c_str());
    std::remove((path_ + ".tmp").c_str());
}

} // namespace primechain::storage
