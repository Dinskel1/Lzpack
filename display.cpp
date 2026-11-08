#include "display.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lzpack {
namespace display {
namespace {

Verbosity g_verbosity = Verbosity::Normal;
bool g_color = false;
bool g_progress = false;

bool isStderrTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

bool isStdoutTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

const char* colorCode(const char* name) {
    if (!g_color) return "";
    return name;
}

bool isWideByte(unsigned char byte) { return byte >= 0xE0; }

}

void setVerbosity(Verbosity level) { g_verbosity = level; }
Verbosity verbosity() { return g_verbosity; }

void setProgressEnabled(bool enabled) { g_progress = enabled; }
bool progressEnabled() { return g_progress && g_verbosity != Verbosity::Quiet; }

void initialize() {
    const bool terminal = isStderrTerminal() || isStdoutTerminal();
    g_color = terminal;
    g_progress = terminal;
}

void setColorEnabled(bool enabled) { g_color = enabled; }

void error(const std::string& message) {
    std::fputs(colorCode("\x1b[31m"), stderr);
    std::fputs("error: ", stderr);
    std::fputs(colorCode("\x1b[0m"), stderr);
    std::fputs(message.c_str(), stderr);
    std::fputc('\n', stderr);
}

void warn(const std::string& message) {
    if (g_verbosity == Verbosity::Quiet) return;
    std::fputs(colorCode("\x1b[33m"), stderr);
    std::fputs("warning: ", stderr);
    std::fputs(colorCode("\x1b[0m"), stderr);
    std::fputs(message.c_str(), stderr);
    std::fputc('\n', stderr);
}

void info(const std::string& message) {
    if (g_verbosity == Verbosity::Quiet) return;
    std::fputs(message.c_str(), stdout);
    std::fputc('\n', stdout);
}

void detail(const std::string& message) {
    if (g_verbosity < Verbosity::Verbose) return;
    std::fputs("  ", stdout);
    std::fputs(message.c_str(), stdout);
    std::fputc('\n', stdout);
}

void debug(const std::string& message) {
    if (g_verbosity < Verbosity::Debug) return;
    std::fputs(colorCode("\x1b[90m"), stdout);
    std::fputs("  [debug] ", stdout);
    std::fputs(message.c_str(), stdout);
    std::fputs(colorCode("\x1b[0m"), stdout);
    std::fputc('\n', stdout);
}

void success(const std::string& message) {
    if (g_verbosity == Verbosity::Quiet) return;
    std::fputs(colorCode("\x1b[32m"), stdout);
    std::fputs(message.c_str(), stdout);
    std::fputs(colorCode("\x1b[0m"), stdout);
    std::fputc('\n', stdout);
}

void section(const std::string& title) {
    if (g_verbosity == Verbosity::Quiet) return;
    std::fputs(colorCode("\x1b[36m"), stdout);
    std::fputs("== ", stdout);
    std::fputs(title.c_str(), stdout);
    std::fputs(" ==", stdout);
    std::fputs(colorCode("\x1b[0m"), stdout);
    std::fputc('\n', stdout);
}

std::string formatSize(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
    }
    return buffer;
}

std::string formatSpeed(std::uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return "-";
    return formatSize(static_cast<std::uint64_t>(static_cast<double>(bytes) / seconds)) + "/s";
}

std::string formatDuration(double seconds) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f s", seconds);
    return buffer;
}

std::string formatRatio(std::uint64_t compressed, std::uint64_t original) {
    if (original == 0) return "-";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f%%", 100.0 * static_cast<double>(compressed) /
                                                       static_cast<double>(original));
    return buffer;
}

std::string formatTime(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return "-";
    const std::time_t value = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &value) != 0) return "-";
#else
    if (localtime_r(&value, &local) == nullptr) return "-";
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

std::size_t displayWidth(const std::string& text) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            ++width;
            ++i;
        } else if (isWideByte(byte)) {
            width += 2;
            i += 3;
        } else {
            ++width;
            i += 2;
        }
    }
    return width;
}

std::string pad(const std::string& text, std::size_t width, bool alignRight) {
    const std::size_t current = displayWidth(text);
    if (current >= width) return text;
    const std::string spaces(width - current, ' ');
    return alignRight ? spaces + text : text + spaces;
}

Table::Table(std::vector<std::string> headers) : headers_(std::move(headers)) {}

void Table::addRow(std::vector<std::string> cells) { rows_.push_back(std::move(cells)); }

void Table::print() const {
    std::vector<std::size_t> widths(headers_.size(), 0);
    for (std::size_t i = 0; i < headers_.size(); ++i) {
        widths[i] = displayWidth(headers_[i]);
    }
    for (const auto& row : rows_) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = (std::max)(widths[i], displayWidth(row[i]));
        }
    }

    std::string line;
    for (std::size_t i = 0; i < headers_.size(); ++i) {
        if (i > 0) line += "  ";
        line += pad(headers_[i], widths[i], i > 0);
    }
    std::fputs(colorCode("\x1b[1m"), stdout);
    std::fputs(line.c_str(), stdout);
    std::fputs(colorCode("\x1b[0m"), stdout);
    std::fputc('\n', stdout);

    std::string separator;
    for (std::size_t i = 0; i < headers_.size(); ++i) {
        if (i > 0) separator += "  ";
        separator.append(widths[i], '-');
    }
    std::fputs(separator.c_str(), stdout);
    std::fputc('\n', stdout);

    for (const auto& row : rows_) {
        std::string text;
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            if (i > 0) text += "  ";
            text += pad(row[i], widths[i], i > 0);
        }
        std::fputs(text.c_str(), stdout);
        std::fputc('\n', stdout);
    }
}

double nowSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

ProgressBar::ProgressBar(std::string label, std::uint64_t total)
    : label_(std::move(label)), total_(total) {
    enabled_ = progressEnabled();
    startedAt_ = nowSeconds();
    if (enabled_) draw();
}

ProgressBar::~ProgressBar() {
    if (enabled_ && !finished_) finish();
}

void ProgressBar::update(std::uint64_t done) {
    done_ = done;
    if (!enabled_) return;
    const int percent = total_ == 0 ? 100 : static_cast<int>(done * 100 / total_);

    if (percent == lastPercent_) return;
    lastPercent_ = percent;
    draw();
}

void ProgressBar::draw() const {
    constexpr int kWidth = 30;
    const int percent = total_ == 0 ? 100 : static_cast<int>(done_ * 100 / total_);
    const int filled = percent * kWidth / 100;
    std::string bar(static_cast<std::size_t>(filled), '=');
    if (filled < kWidth) {
        bar += '>';
        bar.append(static_cast<std::size_t>(kWidth - filled - 1), ' ');
    }

    const double elapsed = nowSeconds() - startedAt_;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "\r%-20.20s [%s] %3d%% %s/%s %s",
                  label_.c_str(), bar.c_str(), percent, formatSize(done_).c_str(),
                  formatSize(total_).c_str(), formatSpeed(done_, elapsed).c_str());
    std::fputs(buffer, stderr);
    std::fflush(stderr);
}

void ProgressBar::finish() {
    if (!enabled_) {
        finished_ = true;
        return;
    }
    done_ = total_;
    draw();
    std::fputc('\n', stderr);
    finished_ = true;
}

}
}
