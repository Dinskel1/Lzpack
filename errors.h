#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lzpack {

enum class ErrorKind {
    InvalidArgument,
    NotFound,
    Io,
    Corrupt,
    Unsupported,
    Internal,
};

enum class ExitCode : int {
    Ok = 0,
    InvalidArgument = 1001,
    NotFound = 2000,
    Io = 2001,
    Corrupt = 2002,
    Unsupported = 2005,
    Internal = 1099,
};

class CompressError : public std::runtime_error {
public:
    CompressError(ErrorKind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    ErrorKind kind() const noexcept { return kind_; }

    ExitCode exitCode() const noexcept {
        switch (kind_) {
            case ErrorKind::InvalidArgument: return ExitCode::InvalidArgument;
            case ErrorKind::NotFound: return ExitCode::NotFound;
            case ErrorKind::Io: return ExitCode::Io;
            case ErrorKind::Corrupt: return ExitCode::Corrupt;
            case ErrorKind::Unsupported: return ExitCode::Unsupported;
            case ErrorKind::Internal: return ExitCode::Internal;
        }
        return ExitCode::Internal;
    }

private:
    ErrorKind kind_;
};

namespace detail {

inline std::string toString(const std::string& v) { return v; }
inline std::string toString(const char* v) { return v ? std::string(v) : std::string("(null)"); }
inline std::string toString(char* v) { return v ? std::string(v) : std::string("(null)"); }
inline std::string toString(bool v) { return v ? "true" : "false"; }
template <typename T>
std::string toString(T v) { return std::to_string(v); }

}

template <typename... Args>
std::string fmt(const std::string& pattern, Args&&... args) {
    const std::vector<std::string> parts{detail::toString(std::forward<Args>(args))...};
    std::string out;
    out.reserve(pattern.size() + 32);
    std::size_t next = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            out += (next < parts.size()) ? parts[next++] : std::string("{}");
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

[[noreturn]] inline void fail(ErrorKind kind, std::string message) {
    throw CompressError(kind, std::move(message));
}

}
