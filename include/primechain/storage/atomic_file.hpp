#pragma once

#include <functional>
#include <string>

namespace primechain::storage::detail {

using FileValidator = std::function<bool(const std::string&, std::string&)>;

bool prepareAtomicLoad(
    const std::string& path,
    const FileValidator& validator,
    std::string& error);

std::string uniqueAtomicTempPath(const std::string& path, const std::string& suffix = ".tmp");

bool commitAtomicTemp(
    const std::string& temp_path,
    const std::string& path,
    const std::string& description,
    std::string& error);

} // namespace primechain::storage::detail
