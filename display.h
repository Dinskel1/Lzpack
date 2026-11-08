#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lzpack {
namespace display {

enum class Verbosity {
    Quiet,
    Normal,
    Verbose,
    Debug,
};

void setVerbosity(Verbosity level);
Verbosity verbosity();

void setProgressEnabled(bool enabled);
bool progressEnabled();

void initialize();

void setColorEnabled(bool enabled);

void error(const std::string& message);
void warn(const std::string& message);
void info(const std::string& message);
void detail(const std::string& message);
void debug(const std::string& message);
void success(const std::string& message);

void section(const std::string& title);

std::string formatSize(std::uint64_t bytes);

std::string formatSpeed(std::uint64_t bytes, double seconds);

std::string formatDuration(double seconds);

std::string formatRatio(std::uint64_t compressed, std::uint64_t original);

std::string formatTime(std::int64_t unixSeconds);

std::string pad(const std::string& text, std::size_t width, bool alignRight = false);
std::size_t displayWidth(const std::string& text);

class Table {
public:
    explicit Table(std::vector<std::string> headers);
    void addRow(std::vector<std::string> cells);
    void print() const;

private:
    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;
};

class ProgressBar {
public:
    ProgressBar(std::string label, std::uint64_t total);
    ~ProgressBar();

    void update(std::uint64_t done);
    void finish();

private:
    void draw() const;

    std::string label_;
    std::uint64_t total_;
    std::uint64_t done_ = 0;
    bool finished_ = false;
    bool enabled_ = false;
    int lastPercent_ = -1;
    double startedAt_ = 0.0;
};

double nowSeconds();

}
}
