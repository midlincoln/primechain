#include "primechain/storage/atomic_file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace primechain::storage::detail {
namespace {

bool exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

std::string parentDirectory(const std::string& path) {
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) return ".";
    if (separator == 0) return "/";
    return path.substr(0, separator);
}

bool syncFile(const std::string& path, const std::string& description, std::string& error) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = "could not open " + description + " for sync: " + std::strerror(errno);
        return false;
    }
    const bool ok = fsync(fd) == 0;
    const int saved_errno = errno;
    close(fd);
    if (!ok) {
        error = "could not sync " + description + ": " + std::strerror(saved_errno);
        return false;
    }
    return true;
}

bool syncDirectory(const std::string& path, const std::string& description, std::string& error) {
    const std::string directory = parentDirectory(path);
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        error = "could not open " + description + " directory: " + std::strerror(errno);
        return false;
    }
    const bool ok = fsync(fd) == 0;
    const int saved_errno = errno;
    close(fd);
    if (!ok) {
        error = "could not sync " + description + " directory: " + std::strerror(saved_errno);
        return false;
    }
    return true;
}

} // namespace

bool commitAtomicTemp(
    const std::string& temp_path,
    const std::string& path,
    const std::string& description,
    std::string& error) {
    if (!syncFile(temp_path, "temporary " + description, error)) return false;
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        error = "could not atomically replace " + description + ": " + std::strerror(errno);
        return false;
    }
    return syncDirectory(path, description, error);
}

bool prepareAtomicLoad(
    const std::string& path,
    const FileValidator& validator,
    std::string& error) {
    const std::string temp_path = path + ".tmp";
    if (exists(path)) {
        std::remove(temp_path.c_str());
        return true;
    }
    if (!exists(temp_path)) return true;

    std::string validation_error;
    if (!validator(temp_path, validation_error)) {
        std::remove(temp_path.c_str());
        return true;
    }
    return commitAtomicTemp(temp_path, path, "recovered sidecar", error);
}

} // namespace primechain::storage::detail
