#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

#include "primechain/storage/record_store.hpp"

namespace {

bool expect(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "failed: " << name << "\n";
        return false;
    }
    return true;
}

primechain::protocol::PrimeRecordV0 makePrime2() {
    primechain::protocol::PrimeRecordV0 record;
    record.version = 0;
    record.height = 0;
    record.integer = 2;
    record.proof.p = 2;
    record.proof.witness = 0;
    record.proof.provider_address = "pcdev1_genesis";
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

primechain::protocol::CompositeRecordV0 makeComposite4(
    const primechain::Hash256& previous,
    const std::string& provider = "pcdev1_composite") {
    primechain::protocol::CompositeRecordV0 record;
    record.version = 0;
    record.height = 2;
    record.previous_record_hash = previous;
    record.integer = 4;
    record.proof.g = 4;
    record.proof.d = 2;
    record.proof.e = 2;
    record.proof.provider_address = provider;
    primechain::protocol::applyDevelopmentFinalization(record);
    return record;
}

std::uint64_t fileSize(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
}

std::uint64_t recoveryBackupCount(const std::string& path) {
    const auto separator = path.find_last_of('/');
    const std::string directory = separator == std::string::npos ? "." : path.substr(0, separator);
    const std::string name = separator == std::string::npos ? path : path.substr(separator + 1);
    const std::string prefix = name + ".recovery-backup.";

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) return 0;
    std::uint64_t count = 0;
    while (const auto* entry = readdir(dir)) {
        const std::string entry_name = entry->d_name;
        if (entry_name.rfind(prefix, 0) == 0) ++count;
    }
    closedir(dir);
    return count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: record-store-tests <store-path>\n";
        return 1;
    }

    const std::string path = argv[1];
    primechain::storage::RecordStore store(path);
    std::string error;

    const auto genesis = primechain::storage::makeStoredRecord(makePrime2());
    if (!expect(store.append(genesis, error), "append genesis")) {
        std::cerr << error << "\n";
        return 1;
    }

    const auto composite = primechain::storage::makeStoredRecord(makeComposite4(genesis.record_hash));
    error.clear();
    if (!expect(store.append(composite, error), "append composite")) {
        std::cerr << error << "\n";
        return 1;
    }

    error.clear();
    const auto records = store.loadAll(error);
    if (!expect(error.empty(), "load without error")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(records.size() == 2, "load two records")) {
        return 1;
    }
    if (!expect(records[0].kind == primechain::storage::StoredRecordKind::Prime, "first record prime")) {
        return 1;
    }
    if (!expect(records[0].integer == 2 && records[0].height == 0, "genesis metadata")) {
        return 1;
    }
    if (!expect(records[1].kind == primechain::storage::StoredRecordKind::Composite, "second record composite")) {
        return 1;
    }
    if (!expect(records[1].integer == 4 && records[1].height == 2, "composite metadata")) {
        return 1;
    }

    error.clear();
    const auto latest = store.latest(error);
    if (!expect(latest.has_value(), "latest record exists")) {
        return 1;
    }
    if (!expect(latest->record_hash == composite.record_hash, "latest hash matches composite")) {
        return 1;
    }

    error.clear();
    const auto found_genesis = store.findByInteger(2, error);
    if (!expect(found_genesis.has_value(), "find genesis by integer")) {
        return 1;
    }
    if (!expect(found_genesis->record_hash == genesis.record_hash, "found genesis hash matches")) {
        return 1;
    }

    error.clear();
    const auto found_composite = store.findByInteger(4, error);
    if (!expect(found_composite.has_value(), "find composite by integer")) {
        return 1;
    }
    if (!expect(found_composite->record_hash == composite.record_hash, "found composite hash matches")) {
        return 1;
    }

    error.clear();
    if (!expect(!store.findByInteger(3, error).has_value(), "missing integer not found")) {
        return 1;
    }

    error.clear();
    const auto range = store.findRange(2, 4, error);
    if (!expect(error.empty(), "range lookup without error")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(range.size() == 2, "range lookup returns existing records")) {
        return 1;
    }
    if (!expect(range[0].integer == 2 && range[1].integer == 4, "range lookup preserves store order")) {
        return 1;
    }

    error.clear();
    const auto empty_range = store.findRange(10, 12, error);
    if (!expect(error.empty() && empty_range.empty(), "empty range lookup")) {
        return 1;
    }

    error.clear();
    store.findRange(5, 4, error);
    if (!expect(!error.empty(), "reject invalid range")) {
        return 1;
    }

    auto corrupted = composite;
    corrupted.record_hash[0] ^= 0xff;
    error.clear();
    if (!expect(!store.append(corrupted, error), "reject hash mismatch on append")) {
        return 1;
    }

    const std::uint64_t complete_size = fileSize(path);
    const std::uint64_t backups_before_recovery = recoveryBackupCount(path);
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const char interrupted_header[] = "partial-record";
        out.write(interrupted_header, sizeof(interrupted_header));
    }
    if (!expect(fileSize(path) > complete_size, "simulate interrupted append")) return 1;
    error.clear();
    const auto recovered_tip = store.latest(error);
    if (!expect(error.empty() && recovered_tip.has_value(), "recover incomplete append")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(recovered_tip->record_hash == composite.record_hash,
            "recovery preserves complete tip")) return 1;
    if (!expect(fileSize(path) == complete_size, "recovery truncates incomplete bytes")) return 1;
    if (!expect(recoveryBackupCount(path) == backups_before_recovery + 1,
            "recovery creates backup before truncation")) return 1;

    {
        std::ofstream out(path + ".idx", std::ios::binary | std::ios::trunc);
        out << "corrupt-index";
    }
    error.clear();
    const auto rebuilt_lookup = store.findByInteger(2, error);
    if (!expect(error.empty() && rebuilt_lookup.has_value(), "rebuild corrupt index")) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!expect(fileSize(path + ".idx") > 24, "persist rebuilt index")) return 1;

    const auto replacement = primechain::storage::makeStoredRecord(
        makeComposite4(genesis.record_hash, "pcdev1_replacement"));
    error.clear();
    if (!expect(store.replaceTip(composite.record_hash, replacement, error),
            "atomically replace tip")) {
        std::cerr << error << "\n";
        return 1;
    }
    error.clear();
    const auto replaced_tip = store.latest(error);
    if (!expect(error.empty() && replaced_tip.has_value() &&
            replaced_tip->record_hash == replacement.record_hash,
            "replacement becomes indexed tip")) return 1;
    if (!expect(fileSize(path + ".rewrite.tmp") == 0, "replacement temp removed")) return 1;

    const std::string source_path = path + ".source";
    primechain::storage::RecordStore source_store(source_path);
    error.clear();
    if (!expect(source_store.append(genesis, error), "create validated install source")) {
        std::cerr << error << "\n";
        return 1;
    }
    {
        std::ofstream out(source_path, std::ios::binary | std::ios::app);
        out << "incomplete";
    }
    error.clear();
    if (!expect(!store.installValidatedStore(source_path, error),
            "reject incomplete install source")) return 1;
    error.clear();
    const auto tip_after_failed_install = store.latest(error);
    if (!expect(error.empty() && tip_after_failed_install.has_value() &&
            tip_after_failed_install->record_hash == replacement.record_hash,
            "failed install preserves live store")) return 1;

    error.clear();
    source_store.loadAll(error);
    if (!expect(error.empty(), "recover install source tail")) return 1;
    error.clear();
    if (!expect(store.installValidatedStore(source_path, error),
            "atomically install validated store")) {
        std::cerr << error << "\n";
        return 1;
    }
    error.clear();
    const auto installed = store.loadAll(error);
    if (!expect(error.empty() && installed.size() == 1 &&
            installed.front().record_hash == genesis.record_hash,
            "installed store replaces prior chain")) return 1;

    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekg(72);
        char byte = 0;
        io.get(byte);
        byte ^= 0x01;
        io.seekp(72);
        io.put(byte);
    }
    error.clear();
    store.loadAll(error);
    if (!expect(error.find("payload hash mismatch") != std::string::npos ||
                    error.find("payload identity mismatch") != std::string::npos,
            "detect interior payload corruption")) return 1;

    std::cout << "record store tests passed\n";
    return 0;
}
