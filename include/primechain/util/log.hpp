#pragma once

// Structured, leveled logging for primechain's CLI tools (client, frontier
// miner, sync). Every line has the same shape so a long-running run-jobs
// session stays scannable instead of turning into an undifferentiated wall
// of ad hoc printf-style text:
//
//   [LEVEL]  Component  Message                  key=value key=value ...
//
// Usage:
//   primechain::log::warn("RPC", "Response timeout")
//       .field("peer", peer.host + ":" + std::to_string(peer.port))
//       .field("attempt", "2/4");
//
// The returned Entry prints itself when it goes out of scope (its
// destructor runs at the end of the full statement), so a bare call like
// the one above is enough -- no separate ".log()"/".flush()" call needed.
//
// Every level goes to stderr, never stdout -- deliberately, regardless of
// level. Several of these tools' stdout lines (SYNCED, JOB_COMPLETE,
// "frontier miner complete", RETRYING_LOCAL_MINER, ...) are a stable
// machine-readable contract that tests and other tools (primechain-ops,
// CMakeLists.txt test cases) grep verbatim out of a raw pipe. This facility
// is strictly additive human-diagnostic output layered on top of that
// contract, not a replacement for it -- so it must never share a stream
// with anything a `| grep -q '...'` might depend on.

#include <iostream>
#include <sstream>
#include <string>

namespace primechain::log {

enum class Level { Info, Warn, Error };

namespace detail {
// Off by default: run-jobs' default view is the curated event stream (state
// changes -- frontier advanced, submission failed, sync caught up). Entries
// marked .verboseOnly() are the finer-grained per-attempt chatter (e.g. each
// individual RPC timeout while probing candidate validators) that's mostly
// useful when actively debugging a stuck node, not on every normal run.
inline bool& verboseFlag() {
    static bool value = false;
    return value;
}
}  // namespace detail

inline void setVerbose(bool value) {
    detail::verboseFlag() = value;
}

inline bool verbose() {
    return detail::verboseFlag();
}

namespace detail {

constexpr std::size_t kLevelWidth = 8;      // "[INFO]  ", "[WARN]  ", "[ERROR] "
constexpr std::size_t kComponentWidth = 9;  // "Storage  ", "Sync     ", ...
constexpr std::size_t kMessageWidth = 24;   // "Response timeout        "

// Pads to `width`, but always appends at least one trailing space even
// when `text` is already at or past `width` -- otherwise a message that
// doesn't fit the usual column (e.g. a longer message kept verbatim for
// backward-compatible substring matching elsewhere) runs directly into
// whatever follows with zero separation.
inline std::string padRight(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    } else {
        text.push_back(' ');
    }
    return text;
}

inline const char* levelTag(Level level) {
    switch (level) {
        case Level::Info:
            return "[INFO]";
        case Level::Warn:
            return "[WARN]";
        case Level::Error:
            return "[ERROR]";
    }
    return "[INFO]";
}

}  // namespace detail

class Entry {
public:
    Entry(Level level, std::string component, std::string message)
        : level_(level), component_(std::move(component)), message_(std::move(message)) {}

    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = default;

    Entry& field(const std::string& key, const std::string& value) {
        if (!fields_.empty()) fields_ += ' ';
        fields_ += key;
        fields_ += '=';
        fields_ += value;
        return *this;
    }

    template <typename T>
    Entry& field(const std::string& key, const T& value) {
        std::ostringstream out;
        out << value;
        return field(key, out.str());
    }

    // Marks this entry as fine-grained/debug-level: it only prints when
    // --verbose is passed. Use for chatter that repeats often in normal
    // operation (per-attempt RPC timeouts, every up-to-date sync poll) --
    // not for anything that represents an actual state change.
    Entry& verboseOnly() {
        verbose_only_ = true;
        return *this;
    }

    ~Entry() {
        if (emitted_) return;
        emitted_ = true;
        if (verbose_only_ && !verbose()) return;
        std::cerr << detail::padRight(detail::levelTag(level_), detail::kLevelWidth)
            << detail::padRight(component_, detail::kComponentWidth) << " "
            << detail::padRight(message_, detail::kMessageWidth);
        if (!fields_.empty()) {
            std::cerr << fields_;
        }
        std::cerr << "\n";
    }

private:
    Level level_;
    std::string component_;
    std::string message_;
    std::string fields_;
    bool emitted_ = false;
    bool verbose_only_ = false;
};

inline Entry info(std::string component, std::string message) {
    return Entry(Level::Info, std::move(component), std::move(message));
}

inline Entry warn(std::string component, std::string message) {
    return Entry(Level::Warn, std::move(component), std::move(message));
}

inline Entry error(std::string component, std::string message) {
    return Entry(Level::Error, std::move(component), std::move(message));
}

// A visual break for the start of a long-running unattended session (e.g.
// run-jobs), so scrolling back through hours of interleaved log output --
// possibly from more than one invocation in the same terminal/log file --
// makes it obvious where a given run actually began:
//
//   ================================================================
//     MINING STARTED   workdir=/path/to/workdir  target=999999999
//   ================================================================
//
// Stderr, like everything else in this facility. Title is short/plain
// (e.g. "MINING STARTED"); fields is pre-joined "key=value key=value" text
// (build it the same way Entry::field does, or just interpolate directly).
inline void banner(const std::string& title, const std::string& fields = "") {
    constexpr std::size_t kWidth = 64;
    const std::string rule(kWidth, '=');
    std::cerr << rule << "\n  " << title;
    if (!fields.empty()) std::cerr << "   " << fields;
    std::cerr << "\n" << rule << "\n";
}

}  // namespace primechain::log
