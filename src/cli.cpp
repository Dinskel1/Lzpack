#include "cli.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "codec.h"
#include "display.h"
#include "fio.h"
#include "format.h"
#include "matchfinder.h"

namespace lzpack {
namespace cli {
namespace {

constexpr const char* kProgramName = "lzpack";
constexpr const char* kProgramVersion = "1.0.0";

struct ParsedArguments {
    std::map<std::string, std::vector<std::string>> options;
    std::vector<std::string> positional;

    bool has(const std::string& name) const { return options.find(name) != options.end(); }

    std::string get(const std::string& name, const std::string& fallback = std::string()) const {
        const auto it = options.find(name);
        if (it == options.end() || it->second.empty() || it->second.back().empty()) return fallback;
        return it->second.back();
    }

    const std::vector<std::string>& all(const std::string& name) const {
        static const std::vector<std::string> kEmpty;
        const auto it = options.find(name);
        return it == options.end() ? kEmpty : it->second;
    }

    std::int64_t integer(const std::string& name, std::int64_t fallback) const;
};

std::uint64_t parseSize(const std::string& text, const std::string& optionName) {
    if (text.empty()) {
        fail(ErrorKind::InvalidArgument, fmt("--{} requires a size value", optionName));
    }
    std::size_t index = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) ++index;
    if (index == 0) {
        fail(ErrorKind::InvalidArgument, fmt("invalid size for --{}: {}", optionName, text));
    }
    const std::uint64_t value = std::strtoull(text.substr(0, index).c_str(), nullptr, 10);
    std::string suffix = text.substr(index);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        multiplier = 1024ull;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        multiplier = 1024ull * 1024;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        multiplier = 1024ull * 1024 * 1024;
    } else {
        fail(ErrorKind::InvalidArgument,
             fmt("unknown size suffix for --{}: {} (use k, m, or g)", optionName, text));
    }
    return value * multiplier;
}

std::int64_t ParsedArguments::integer(const std::string& name, std::int64_t fallback) const {
    const std::string text = get(name);
    if (text.empty()) return fallback;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != '\0')) {
        fail(ErrorKind::InvalidArgument, fmt("--{} must be an integer: {}", name, text));
    }
    return static_cast<std::int64_t>(value);
}

bool isGlobalOption(const std::string& name) {
    static const char* const kNames[] = {"help",     "h",         "version",     "verbose",
                                         "quiet",    "color",     "no-color",    "progress",
                                         "no-progress"};
    for (const char* option : kNames) {
        if (name == option) return true;
    }
    return false;
}

bool isBooleanOption(const std::string& name) {
    static const char* const names[] = {
        "help", "h", "version", "quiet", "q", "color", "no-color", "progress",
        "no-progress", "force", "verify", "no-empty-dirs", "no-filter", "fast",
        "flatten", "no-overwrite", "no-verify", "no-time", "dry-run", "long", "L"};
    for (const char* candidate : names) if (name == candidate) return true;
    return false;
}

ParsedArguments parseArguments(const std::vector<std::string>& arguments,
                               const std::vector<std::string>& known) {
    ParsedArguments result;
    bool optionsEnded = false;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& token = arguments[i];
        if (optionsEnded || token.size() < 2 || token[0] != '-') {
            result.positional.push_back(token);
            continue;
        }
        if (token == "--") {
            optionsEnded = true;
            continue;
        }

        std::string name;
        std::string value;
        bool hasValue = false;
        if (token.size() > 2 && token[1] == '-') {
            const std::size_t equals = token.find('=');
            if (equals == std::string::npos) {
                name = token.substr(2);
            } else {
                name = token.substr(2, equals - 2);
                value = token.substr(equals + 1);
                hasValue = true;
            }
        } else {

            name = token.substr(1, 1);
            if (token.size() > 2) {
                value = (token[2] == '=') ? token.substr(3) : token.substr(2);
                hasValue = true;
            }
        }

        if (!known.empty()) {
            const bool allowed =
                std::find(known.begin(), known.end(), name) != known.end() || isGlobalOption(name);
            if (!allowed) {
                std::string suggestion;
                for (const auto& candidate : known) {
                    if (!candidate.empty() && candidate.compare(0, name.size(), name) == 0) {
                        suggestion = fmt(" (did you mean --{}?)", candidate);
                        break;
                    }
                }
                fail(ErrorKind::InvalidArgument,
                     fmt("unknown option {}{}{}", token, "", suggestion));
            }
        }

        if (!hasValue && !isBooleanOption(name) && i + 1 < arguments.size() &&
            !arguments[i + 1].empty() && arguments[i + 1][0] != '-') {
            value = arguments[++i];
            hasValue = true;
        } else if (!hasValue && (name == "verbose" || name == "v") &&
                   i + 1 < arguments.size() &&
                   (arguments[i + 1] == "debug" || arguments[i + 1] == "2")) {
            value = arguments[++i];
            hasValue = true;
        }
        (void)hasValue;
        result.options[name].push_back(value);
    }
    return result;
}

CodecParams presetParams(CompressionLevel level) {
    CodecParams params;
    params.level = level;
    switch (level) {
        case CompressionLevel::Store:
            params.windowSize = 1u << 20;
            params.searchDepth = 1;
            params.niceLength = 8;
            params.lazyMatching = false;
            break;
        case CompressionLevel::Fastest:
            params.windowSize = 1u << 20;
            params.searchDepth = 8;
            params.niceLength = 32;
            params.lazyMatching = false;
            break;
        case CompressionLevel::Fast:
            params.windowSize = 2u << 20;
            params.searchDepth = 16;
            params.niceLength = 48;
            break;
        case CompressionLevel::Normal:
            params.windowSize = 4u << 20;
            params.searchDepth = 32;
            params.niceLength = 64;
            break;
        case CompressionLevel::Good:
            params.windowSize = 8u << 20;
            params.searchDepth = 48;
            params.niceLength = 96;
            break;
        case CompressionLevel::Max:
            params.windowSize = 16u << 20;
            params.searchDepth = 96;
            params.niceLength = 160;
            break;
        case CompressionLevel::Ultra:
            params.windowSize = 64u << 20;
            params.searchDepth = 256;
            params.niceLength = 273;
            break;
    }
    return params;
}

CompressionLevel parseLevel(const std::string& text) {
    static const std::map<std::string, CompressionLevel> kNames = {
        {"store", CompressionLevel::Store},     {"0", CompressionLevel::Store},
        {"fastest", CompressionLevel::Fastest}, {"1", CompressionLevel::Fastest},
        {"fast", CompressionLevel::Fast},       {"2", CompressionLevel::Fast},
        {"normal", CompressionLevel::Normal},   {"3", CompressionLevel::Normal},
        {"good", CompressionLevel::Good},       {"4", CompressionLevel::Good},
        {"max", CompressionLevel::Max},         {"5", CompressionLevel::Max},
        {"ultra", CompressionLevel::Ultra},     {"6", CompressionLevel::Ultra},
    };
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = kNames.find(lower);
    if (it == kNames.end()) {
        fail(ErrorKind::InvalidArgument,
             fmt("unknown compression level {} (use store, 1-6, or ultra)", text));
    }
    return it->second;
}

std::uint32_t alignWindow(std::uint64_t requested) {
    constexpr std::uint64_t kMinWindow = 1u << 16;
    constexpr std::uint64_t kMaxWindow = 512u << 20;
    if (requested <= kMinWindow) return static_cast<std::uint32_t>(kMinWindow);
    if (requested >= kMaxWindow) return static_cast<std::uint32_t>(kMaxWindow);
    std::uint64_t value = kMinWindow;
    while (value < requested) value <<= 1;
    return static_cast<std::uint32_t>(value);
}

void applyOverrides(const ParsedArguments& parsed, CompressionOptions& options) {
    if (parsed.has("level") || parsed.has("l")) {
        options.codec = presetParams(parseLevel(parsed.get("level", parsed.get("l"))));
    }
    if (parsed.has("window") || parsed.has("w")) {
        options.codec.windowSize =
            alignWindow(parseSize(parsed.get("window", parsed.get("w")), "window"));
    }
    if (parsed.has("block") || parsed.has("b")) {

        options.codec.windowSize =
            alignWindow(parseSize(parsed.get("block", parsed.get("b")), "block"));
    }
    if (parsed.has("depth")) {
        const std::int64_t depth = parsed.integer("depth", 0);
        if (depth < 1 || depth > 4096) {
            fail(ErrorKind::InvalidArgument, "--depth must be between 1 and 4096");
        }
        options.codec.searchDepth = static_cast<std::uint32_t>(depth);
    }
    if (parsed.has("nice")) {
        const std::int64_t nice = parsed.integer("nice", 0);
        if (nice < static_cast<std::int64_t>(kMinMatch) ||
            nice > static_cast<std::int64_t>(kMaxMatchLength)) {
            fail(ErrorKind::InvalidArgument,
                 fmt("--nice must be between {} and {}", kMinMatch, kMaxMatchLength));
        }
        options.codec.niceLength = static_cast<std::uint32_t>(nice);
    }
    if (parsed.has("lazy")) {
        const std::string value = parsed.get("lazy", "on");
        options.codec.lazyMatching = !(value == "off" || value == "no" || value == "false");
    }
    if (parsed.has("fast")) {
        options.codec.lazyMatching = false;
    }
    if (parsed.has("threads") || parsed.has("T")) {
        const std::int64_t threads =
            parsed.has("threads") ? parsed.integer("threads", 1) : parsed.integer("T", 1);
        if (threads < 1 || threads > 256) {
            fail(ErrorKind::InvalidArgument, "--threads must be between 1 and 256");
        }
        options.threads = static_cast<unsigned>(threads);
    }
    if (parsed.has("delta")) {
        const std::int64_t distance = parsed.integer("delta", 1);
        if (distance < 1 || distance > 255) {
            fail(ErrorKind::InvalidArgument, "--delta must be between 1 and 255");
        }
        options.codec.filter.kind = FilterKind::Delta;
        options.codec.filter.delta = static_cast<std::uint8_t>(distance);
        options.autoFilter = false;
    }
    if (parsed.has("bcj")) {
        const std::string value = parsed.get("bcj", "on");
        if (value == "off" || value == "no" || value == "false") {
            options.autoFilter = false;
        } else {
            options.codec.filter.kind = FilterKind::X86Bcj;
            options.autoFilter = false;
        }
    }
    if (parsed.has("text")) {
        const std::string value = parsed.get("text", "on");
        if (value == "auto") {
            options.autoText = true;
        } else if (value == "off" || value == "no" || value == "false") {
            options.codec.textMode = false;
            options.autoText = false;
        } else {
            options.codec.textMode = true;
            options.autoText = false;
        }
    }
    if (parsed.has("no-filter")) {
        options.autoFilter = false;
        options.autoText = false;
        options.codec.filter.kind = FilterKind::None;
    }
    if (parsed.has("no-empty-dirs")) {
        options.keepEmptyDirectories = false;
    }
}

class Progress {
public:
    explicit Progress(std::string label) : label_(std::move(label)) {}

    ProgressHandler handler() {
        return [this](const ProgressInfo& info) {
            if (info.totalBytes == 0 || !display::progressEnabled()) return;
            if (!bar_) {
                bar_.reset(new display::ProgressBar(label_, info.totalBytes));
            }
            if (!info.currentPath.empty() && info.currentPath != currentPath_) {
                currentPath_ = info.currentPath;
                display::detail(fmt("Processing {}", info.currentPath));
            }
            bar_->update(info.doneBytes);
        };
    }

    void finish() {
        if (bar_) bar_->finish();
    }

private:
    std::string label_;
    std::string currentPath_;
    std::unique_ptr<display::ProgressBar> bar_;
};

void printCompressionSummary(const ArchiveStats& stats, const std::string& outputPath,
                             const CompressionOptions& options) {
    display::section("Compression complete");
    display::Table table({"Item", "Value"});
    table.addRow({"Output", outputPath});
    table.addRow({"Original size", display::formatSize(stats.inputBytes)});
    table.addRow({"Compressed size", display::formatSize(stats.outputBytes)});
    table.addRow({"Ratio", display::formatRatio(stats.outputBytes, stats.inputBytes)});
    const std::uint64_t saved =
        stats.inputBytes > stats.outputBytes ? stats.inputBytes - stats.outputBytes : 0;
    table.addRow({"Saved", fmt("{} ({})", display::formatSize(saved),
                                          display::formatRatio(saved, stats.inputBytes))});
    table.addRow({"Data", fmt("{} ({} blocks)",
                                            display::formatSize(stats.payloadBytes),
                                            stats.blockCount)});
    table.addRow({"Index", display::formatSize(stats.indexBytes)});
    table.addRow({"Entries", std::to_string(stats.entryCount)});
    table.addRow({"Time", display::formatDuration(stats.seconds)});
    table.addRow({"Speed", display::formatSpeed(stats.inputBytes, stats.seconds)});
    table.addRow({"Level / window", fmt("{} / {}", compressionLevelName(options.codec.level),
                                             display::formatSize(options.codec.windowSize))});
    table.addRow({"Threads", std::to_string(options.threads)});
    table.addRow({"Filter",
                  options.codec.filter.kind == FilterKind::Delta
                      ? fmt("delta (step {})", options.codec.filter.delta)
                      : (options.codec.filter.kind == FilterKind::X86Bcj
                             ? "x86-bcj"
                             : (options.autoFilter ? "auto (x86 BCJ)" : "none"))});
    table.print();

    if (display::verbosity() >= display::Verbosity::Verbose) {
        display::section("Codec stats");
        display::Table codec({"Item", "Value"});
        codec.addRow({"Literals", fmt("{} ({})", stats.codec.literalCount,
                                             display::formatSize(stats.codec.literalBytes))});
        codec.addRow({"Matches", fmt("{}", stats.codec.matchCount)});
        codec.addRow({"Repeated matches", fmt("{}", stats.codec.repMatchCount)});
        codec.addRow({"Matched bytes", display::formatSize(stats.codec.matchBytes)});
        codec.addRow({"Codec input", display::formatSize(stats.codecInputBytes)});
        codec.addRow({"Codec output", display::formatSize(stats.codecOutputBytes)});
        codec.addRow({"Codec ratio",
                      display::formatRatio(stats.codecOutputBytes, stats.codecInputBytes)});
        codec.print();
    }
}

void printExtractionSummary(const ExtractionStats& stats, const std::string& destination) {
    display::section("Extraction complete");
    display::Table table({"Item", "Value"});
    table.addRow({"Destination", destination});
    table.addRow({"Extracted", std::to_string(stats.entriesExtracted)});
    if (stats.entriesSkipped > 0) {
        table.addRow({"Skipped", std::to_string(stats.entriesSkipped)});
    }
    table.addRow({"Written", display::formatSize(stats.bytesWritten)});
    table.addRow({"Time", display::formatDuration(stats.seconds)});
    table.addRow({"Extract speed", display::formatSpeed(stats.bytesWritten, stats.seconds)});
    table.print();
}

bool reportFailures(const std::vector<std::string>& failures) {
    if (failures.empty()) return true;
    display::section(fmt("{} entries failed", failures.size()));
    for (const auto& failure : failures) {
        display::error(failure);
    }
    return false;
}

int compress(const ParsedArguments& parsed) {
    if (parsed.positional.empty()) {
        fail(ErrorKind::InvalidArgument,
             "missing input: compress requires at least one file or directory (see lzpack help compress)");
    }

    CompressionOptions options;

    const bool manyInputs =
        parsed.positional.size() > 1 || fio::isDirectory(parsed.positional.front());
    options.codec = presetParams(manyInputs ? CompressionLevel::Normal : CompressionLevel::Good);
    options.archiveMode = manyInputs;
    applyOverrides(parsed, options);

    std::string outputPath = parsed.get("output", parsed.get("o"));
    if (outputPath.empty()) {
        if (manyInputs) {
            std::string name = fio::toArchivePath(parsed.positional.front());
            while (!name.empty() && name.back() == '/') name.pop_back();
            name = fio::pathFileName(name);
            if (name.empty()) name = "archive";
            outputPath = name + ".lzp";
        } else {
            outputPath = parsed.positional.front() + ".lzp";
        }
    }
    if (fio::isDirectory(outputPath)) {
        fail(ErrorKind::InvalidArgument, fmt("output path is a directory: {}", outputPath));
    }
    if (fio::pathExists(outputPath) && !parsed.has("force")) {
        fail(ErrorKind::InvalidArgument,
             fmt("output already exists: {} (use --force to overwrite)", outputPath));
    }

    display::section("Compressing");
    display::Table plan({"Item", "Value"});
    plan.addRow({"Inputs", std::to_string(parsed.positional.size())});
    plan.addRow({"Output", outputPath});
    plan.addRow({"Level", compressionLevelName(options.codec.level)});
    plan.addRow({"Window", display::formatSize(options.codec.windowSize)});
    plan.addRow({"Block size", display::formatSize(options.codec.blockSize())});
    plan.addRow({"Search depth", std::to_string(options.codec.searchDepth)});
    plan.addRow({"Lazy matching", options.codec.lazyMatching ? "on" : "off"});
    plan.addRow({"Threads", std::to_string(options.threads)});
    plan.print();
    for (const auto& input : parsed.positional) {
        display::detail(fmt("Input: {}", input));
    }

    ArchiveStats stats;
    Progress reporter("Compress");
    createArchive(parsed.positional, outputPath, options, reporter.handler(), &stats);
    reporter.finish();
    printCompressionSummary(stats, outputPath, options);

    if (parsed.has("verify")) {
        display::section("Verifying archive");
        fio::FileReader reader(outputPath);
        std::vector<ArchiveEntry> entries;
        ArchiveHeader header;
        readIndex(reader, entries, &header, ArchiveLimits{});
        ExtractionStats verifyStats;
        verifyArchive(reader, entries, ProgressHandler(), &verifyStats);
        if (!reportFailures(verifyStats.failures)) {
            return static_cast<int>(ExitCode::Corrupt);
        }
        display::success(fmt("verified {} entries", verifyStats.entriesExtracted));
    }

    if (parsed.has("stub")) {
        const std::string stub = parsed.get("stub");
        const std::string target = parsed.get("stub-output", outputPath + ".exe");
        const std::uint64_t size = buildSelfExtractor(stub, outputPath, target);
        display::success(fmt("created self-extracting archive {} ({})", target, display::formatSize(size)));
    }
    return static_cast<int>(ExitCode::Ok);
}

void printEntryTable(const std::vector<ArchiveEntry>& entries, const std::string& sortKey,
                     bool longFormat) {
    std::vector<const ArchiveEntry*> ordered;
    ordered.reserve(entries.size());
    for (const auto& entry : entries) ordered.push_back(&entry);

    if (sortKey == "size") {
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const ArchiveEntry* a, const ArchiveEntry* b) {
                             return a->originalSize > b->originalSize;
                         });
    } else if (sortKey == "ratio") {
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const ArchiveEntry* a, const ArchiveEntry* b) {
                             const double ra = a->originalSize == 0
                                                   ? 0.0
                                                   : static_cast<double>(a->compressedSize) /
                                                         static_cast<double>(a->originalSize);
                             const double rb = b->originalSize == 0
                                                   ? 0.0
                                                   : static_cast<double>(b->compressedSize) /
                                                         static_cast<double>(b->originalSize);
                             return ra < rb;
                         });
    } else if (sortKey == "name") {
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const ArchiveEntry* a, const ArchiveEntry* b) {
                             return a->path < b->path;
                         });
    } else if (sortKey != "order") {
        fail(ErrorKind::InvalidArgument,
             fmt("--sort accepts name, size, ratio, or order; got {}", sortKey));
    }

    display::Table table(longFormat
                             ? std::vector<std::string>{"Size", "Compressed", "Ratio", "Modified",
                                                        "CRC32", "Blocks", "Path"}
                             : std::vector<std::string>{"Size", "Compressed", "Ratio", "Path"});
    for (const auto* entry : ordered) {
        if ((entry->mode & kEntryDirectory) != 0) {
            if (longFormat) {
                table.addRow({"<dir>", "-", "-", display::formatTime(entry->modifiedTime), "-", "-",
                              entry->path + "/"});
            } else {
                table.addRow({"<dir>", "-", "-", entry->path + "/"});
            }
            continue;
        }
        char crcText[16];
        std::snprintf(crcText, sizeof(crcText), "%08X", entry->crc);
        const std::string ratio = display::formatRatio(entry->compressedSize, entry->originalSize);
        if (longFormat) {
            table.addRow({display::formatSize(entry->originalSize),
                          display::formatSize(entry->compressedSize), ratio,
                          display::formatTime(entry->modifiedTime), crcText,
                          std::to_string(entry->blockCount), entry->path});
        } else {
            table.addRow({display::formatSize(entry->originalSize),
                          display::formatSize(entry->compressedSize), ratio, entry->path});
        }
    }
    table.print();
}

int list(const ParsedArguments& parsed) {
    if (parsed.positional.empty()) {
        fail(ErrorKind::InvalidArgument, "missing archive: list requires an .lzp file");
    }
    const std::string archivePath = parsed.positional.front();
    fio::FileReader reader(archivePath);

    std::vector<ArchiveEntry> entries;
    ArchiveHeader header;
    const ArchiveFooter footer = readIndex(reader, entries, &header, ArchiveLimits{});

    const std::string sortKey = parsed.get("sort", "order");
    const bool longFormat =
        parsed.has("long") || parsed.has("L") || display::verbosity() >= display::Verbosity::Verbose;

    std::vector<ArchiveEntry> selected;
    std::uint64_t totalOriginal = 0;
    std::uint64_t totalCompressed = 0;
    std::uint64_t totalFiles = 0;
    std::uint64_t totalDirectories = 0;
    for (const auto& entry : entries) {
        if (!entrySelected(entry.path, parsed.all("include"), parsed.all("exclude"))) continue;
        if ((entry.mode & kEntryDirectory) != 0) {
            ++totalDirectories;
        } else {
            ++totalFiles;
            totalOriginal += entry.originalSize;
            totalCompressed += entry.compressedSize;
        }
        selected.push_back(entry);
    }

    display::section("Archive info");
    display::Table info({"Item", "Value"});
    info.addRow({"Archive", archivePath});
    info.addRow({"File size", display::formatSize(reader.size())});
    info.addRow({"Format version", std::to_string(header.version)});
    info.addRow({"Level", compressionLevelName(header.level)});
    info.addRow({"Default block size",
                 header.blockSize == 0 ? "-" : display::formatSize(header.blockSize)});
    info.addRow({"Flags", (header.flags & kFlagTextMode) ? "text newline normalization" : "-"});
    info.addRow({"Entries", fmt("{} ({} files / {} dirs)", footer.entryCount, totalFiles,
                                         totalDirectories)});
    info.addRow({"Original total", display::formatSize(totalOriginal)});
    info.addRow({"Compressed size", display::formatSize(totalCompressed)});
    info.addRow({"Ratio", display::formatRatio(totalCompressed, totalOriginal)});
    info.addRow({"Index size", display::formatSize(footer.indexSize)});
    info.print();

    display::section(fmt("Entries ({} selected)", selected.size()));
    printEntryTable(selected, sortKey, longFormat);
    return static_cast<int>(ExitCode::Ok);
}

int extract(const ParsedArguments& parsed) {
    if (parsed.positional.empty()) {
        fail(ErrorKind::InvalidArgument, "missing archive: extract requires an .lzp file");
    }
    const std::string archivePath = parsed.positional.front();
    fio::FileReader reader(archivePath);

    std::vector<ArchiveEntry> entries;
    ArchiveHeader header;
    readIndex(reader, entries, &header, ArchiveLimits{});

    ExtractionOptions options;
    options.destination = parsed.get("into", parsed.get("C", "."));
    if (options.destination.empty()) options.destination = ".";
    options.includes = parsed.all("include");
    options.excludes = parsed.all("exclude");
    options.overwrite = !parsed.has("no-overwrite");
    options.restoreTimestamps = !parsed.has("no-time");
    options.verifyCrc = !parsed.has("no-verify");
    options.dryRun = parsed.has("dry-run");
    options.flatten = parsed.has("flatten");

    std::size_t selectedCount = 0;
    for (const auto& entry : entries) {
        if (entrySelected(entry.path, options.includes, options.excludes)) ++selectedCount;
    }
    if (selectedCount == 0) {
        display::warn("no entries matched the include/exclude rules");
        return static_cast<int>(ExitCode::Ok);
    }

    display::section("Extracting");
    display::Table plan({"Item", "Value"});
    plan.addRow({"Archive", archivePath});
    plan.addRow({"Destination", options.destination});
    plan.addRow({"Selected entries", std::to_string(selectedCount)});
    plan.addRow({"Overwrite existing files", options.overwrite ? "yes" : "no"});
    plan.addRow({"Verify CRC", options.verifyCrc ? "yes" : "no"});
    plan.addRow({"Restore timestamps", options.restoreTimestamps ? "yes" : "no"});
    plan.addRow({"Archive entries", std::to_string(entries.size())});
    if (options.dryRun) plan.addRow({"Mode", "dry run (no files written)"});
    plan.print();

    if (!options.dryRun) {
        fio::createDirectories(options.destination);
    }

    ExtractionStats stats;
    Progress reporter("Extract");
    extractArchive(reader, archivePath, entries, options, reporter.handler(), &stats);
    reporter.finish();

    printExtractionSummary(stats, options.destination);
    if (!reportFailures(stats.failures)) {
        return static_cast<int>(ExitCode::Corrupt);
    }
    if (!options.dryRun) {
        display::success(fmt("extracted all {} entries", stats.entriesExtracted));
    }
    return static_cast<int>(ExitCode::Ok);
}

int test(const ParsedArguments& parsed) {
    if (parsed.positional.empty()) {
        fail(ErrorKind::InvalidArgument, "missing archive: test requires an .lzp file");
    }

    int failedArchives = 0;
    for (const auto& archivePath : parsed.positional) {
        display::section(fmt("Verifying {}", archivePath));
        fio::FileReader reader(archivePath);
        std::vector<ArchiveEntry> entries;
        ArchiveHeader header;
        readIndex(reader, entries, &header, ArchiveLimits{});

        ExtractionStats stats;
        Progress reporter("Verify");
        verifyArchive(reader, entries, reporter.handler(), &stats);
        reporter.finish();

        display::Table table({"Item", "Value"});
        table.addRow({"Entries", std::to_string(entries.size())});
        table.addRow({"Passed", std::to_string(stats.entriesExtracted)});
        table.addRow({"Verified bytes", display::formatSize(stats.bytesWritten)});
        table.addRow({"Time", display::formatDuration(stats.seconds)});
        table.addRow({"Verify speed", display::formatSpeed(stats.bytesWritten, stats.seconds)});
        table.print();

        if (!reportFailures(stats.failures)) {
            ++failedArchives;
        } else {
            display::success("Archive is valid");
        }
    }
    return failedArchives == 0 ? static_cast<int>(ExitCode::Ok)
                               : static_cast<int>(ExitCode::Corrupt);
}

int selfTest() {
    display::section("Self-test: compression round trip");

    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> cases;
    cases.push_back({"empty input", {}});
    cases.push_back({"single byte", {0x42}});
    cases.push_back({"2 bytes", {1, 2}});
    cases.push_back({"3 bytes", {1, 2, 3}});
    cases.push_back({"4 bytes", {1, 2, 3, 4}});
    cases.push_back({"5 repeated bytes", {7, 7, 7, 7, 7}});
    cases.push_back({"8 repeated bytes", {7, 7, 7, 7, 7, 7, 7, 7}});
    cases.push_back({"16 repeated bytes", std::vector<std::uint8_t>(16, 7)});
    cases.push_back({"16-byte pattern", {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8}});
    {
        std::vector<std::uint8_t> data;
        for (int i = 0; i < 32; ++i) data.push_back(static_cast<std::uint8_t>(i % 8));
        cases.push_back({"32-byte pattern", data});
    }
    {
        std::vector<std::uint8_t> data;
        for (int i = 0; i < 1000; ++i) data.push_back(static_cast<std::uint8_t>(i % 7));
        cases.push_back({"1000-byte pattern", data});
    }
    cases.push_back({"200 KiB run", std::vector<std::uint8_t>(200000, 0xAB)});
    {
        std::vector<std::uint8_t> data;
        std::uint32_t seed = 12345;
        for (int i = 0; i < 300000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            data.push_back(static_cast<std::uint8_t>(seed >> 24));
        }
        cases.push_back({"300 KB pseudo-random", data});
    }
    {
        const char* text = "The quick brown fox jumps over the lazy dog.\n";
        std::vector<std::uint8_t> data;
        while (data.size() < 150000) data.insert(data.end(), text, text + std::strlen(text));
        cases.push_back({"150 KB repeated text", data});
    }
    {
        std::vector<std::uint8_t> data;
        for (int run = 0; run < 60; ++run) {
            for (int i = 0; i < run * 3 + 1; ++i) {
                data.push_back(static_cast<std::uint8_t>('a' + run % 26));
            }
            data.push_back(static_cast<std::uint8_t>(run));
        }
        cases.push_back({"variable-length runs", data});
    }

    const CompressionLevel kLevels[] = {CompressionLevel::Store, CompressionLevel::Fastest,
                                        CompressionLevel::Normal, CompressionLevel::Good,
                                        CompressionLevel::Ultra};
    int failures = 0;
    display::Table table({"Case", "Level", "Original", "Compressed", "Ratio", "Result"});
    for (const auto& testCase : cases) {
        for (CompressionLevel level : kLevels) {
            CodecParams params = presetParams(level);
            params.windowSize = 1u << 16;
            std::string compressed;
            compressBlock(params, testCase.second.data(), testCase.second.size(), nullptr, 0, false,
                          compressed, ProgressCallback(), nullptr);

            const std::size_t payloadSize = compressed.size() - kBlockHeaderSize;
            std::uint64_t originalSize = 0;
            std::uint64_t storedSize = 0;
            std::uint32_t crc = 0;
            const BlockInfo info = readBlockHeader(
                reinterpret_cast<const std::uint8_t*>(compressed.data()),
                kBlockHeaderSize, &originalSize, &storedSize, &crc);

            std::vector<std::uint8_t> restored;
            bool ok = false;
            std::string note;
            try {
                decompressBlock(info,
                                reinterpret_cast<const std::uint8_t*>(compressed.data()) + kBlockHeaderSize,
                                payloadSize, static_cast<std::size_t>(originalSize), crc, restored,
                                ProgressCallback(), nullptr);
                ok = (restored == testCase.second);
                if (!ok) note = "content mismatch";
            } catch (const CompressError& error) {
                note = error.what();
            }
            if (!ok) ++failures;

            table.addRow({testCase.first, compressionLevelName(level),
                          display::formatSize(testCase.second.size()),
                          display::formatSize(payloadSize),
                          display::formatRatio(payloadSize, testCase.second.size()),
                          ok ? "PASS" : ("FAIL:" + note)});
        }
    }
    table.print();
    if (failures == 0) {
        display::success("All self-tests passed");
        return static_cast<int>(ExitCode::Ok);
    }
    display::error(fmt("{} self-test cases failed", failures));
    return static_cast<int>(ExitCode::Internal);
}

struct CommandHelp {
    std::string name;
    std::string usage;
    std::string summary;
    std::vector<std::string> options;
};

const char* const kCompressOptions =
    "  -l, --level <level>     0=store 1=fastest 2=fast 3=normal 4=good 5=max 6=ultra\n"
    "  -w, --window <size>     Match window and per-block memory limit (e.g. 4M or 64M)\n"
    "  -b, --block <size>      Maximum uncompressed block size (defaults to window size)\n"
    "      --depth <n>         Match-chain search depth; higher values trade speed for ratio\n"
    "      --nice <n>          Stop searching at this match length (4..273)\n"
    "      --lazy <on|off>     Enable lazy matching (default: on)\n"
    "  -T, --threads <n>       Compression threads (block-level, default: 1)\n"
    "      --bcj <on|off>      x86 call/jmp filter (auto-detected by extension)\n"
    "      --delta <n>         Delta filter step, 1..255 (useful for audio and sensor data)\n"
    "      --text <on|off|auto>  Normalize CRLF to LF and restore on extraction\n"
    "      --no-filter         Disable automatic filters\n"
    "      --no-empty-dirs     Do not store empty directories";

std::vector<CommandHelp> buildCommandHelp() {
    std::vector<CommandHelp> help;
    help.push_back({"compress",
                    "lzpack compress [options] <file-or-dir> [more inputs...]",
                    "Create an .lzp archive from files or directories (alias: c)",
                    {"  -o, --output <path>   Archive output path (default: <first input>.lzp)",
                     "      --force            Overwrite an existing output file",
                     "      --verify           Reopen and verify the archive after writing",
                     "      --stub <file>      Append the archive to a self-extracting stub",
                     "      --stub-output <path>  Self-extracting output path (default: <archive>.exe)",
                     kCompressOptions}});
    help.push_back({"extract", "lzpack extract [options] <archive.lzp>",
                    "Extract an archive (alias: x)",
                    {"  -C, --into <dir>      Extraction directory (default: current directory)",
                     "      --include <pattern>  Extract matching entries; may be repeated",
                     "      --exclude <pattern>  Exclude matching entries; may be repeated",
                     "      --flatten          Extract all entries into one directory",
                     "      --no-overwrite     Skip files that already exist",
                     "      --no-verify        Skip CRC checks (faster, not recommended)",
                     "      --no-time          Do not restore timestamps",
                     "      --dry-run          Show what would be extracted without writing files"}});
    help.push_back({"list", "lzpack list [options] <archive.lzp>",
                    "List archive contents and statistics (alias: l)",
                    {"  -L, --long             Show CRCs, timestamps, and block counts",
                     "      --sort <key>        name / size / ratio / order (default: order)",
                     "      --include <pattern>  Show matching entries only",
                     "      --exclude <pattern>  Hide matching entries"}});
    help.push_back({"test", "lzpack test <archive.lzp> [more archives...]",
                    "Verify archive integrity by decoding every block and checking CRCs (alias: t)", {}});
    help.push_back({"selftest", "lzpack selftest",
                    "Run built-in compression and decompression round-trip tests", {}});
    return help;
}

const std::vector<CommandHelp>& commandHelp() {
    static const std::vector<CommandHelp> help = buildCommandHelp();
    return help;
}

std::vector<std::string> knownOptionsFor(const std::string& command) {
    std::vector<std::string> options{"help", "h", "verbose", "quiet", "color", "no-color",
                                     "progress", "no-progress"};
    const auto append = [&options](const std::vector<std::string>& extra) {
        options.insert(options.end(), extra.begin(), extra.end());
    };
    if (command == "compress") {
        append({"output", "o", "level", "l", "window", "w", "depth", "nice", "block", "b",
                "threads", "T", "method", "delta", "bcj", "text", "lazy", "force", "verify",
                "stub", "stub-output", "no-filter", "no-empty-dirs", "fast"});
    } else if (command == "extract") {
        append({"into", "C", "include", "exclude", "flatten", "no-overwrite", "no-verify",
                "no-time", "dry-run", "force"});
    } else if (command == "list") {
        append({"long", "L", "sort", "include", "exclude"});
    }
    return options;
}

void applyGlobalOptions(const ParsedArguments& parsed) {
    if (parsed.has("quiet")) {
        display::setVerbosity(display::Verbosity::Quiet);
        display::setProgressEnabled(false);
    } else if (parsed.has("verbose")) {
        const std::string value = parsed.get("verbose", "on");
        display::setVerbosity((value == "debug" || value == "2") ? display::Verbosity::Debug
                                                                : display::Verbosity::Verbose);
    }
    if (parsed.has("no-color")) display::setColorEnabled(false);
    if (parsed.has("color")) display::setColorEnabled(true);
    if (parsed.has("no-progress")) display::setProgressEnabled(false);
    if (parsed.has("progress")) display::setProgressEnabled(true);
}

}

const std::vector<std::string>& commandNames() {
    static const std::vector<std::string> kNames = {"compress", "extract", "list",
                                                    "test",     "selftest", "help", "version"};
    return kNames;
}

void printHelp() {
    std::printf(
        "%s %s - command-line compressor (LZ77 + adaptive range coding)\n\n"
        "Usage:\n"
        "  %s <command> [options] [args...]\n\n"
        "Commands:\n"
        "  compress (c)   Create an .lzp archive\n"
        "  extract  (x)   Extract an .lzp archive\n"
        "  list     (l)   List archive contents\n"
        "  test     (t)   Verify archive integrity\n"
        "  selftest       Run codec self-tests\n"
        "  help           Show help (use help <command> for details)\n"
        "  version        Show version\n\n"
        "Global options:\n"
        "  -v, --verbose[=debug]  Show more detail\n"
        "  -q, --quiet            Suppress all but errors\n"
        "      --color/--no-color Force color output on or off\n"
        "      --no-progress      Disable progress output\n\n"
        "Examples:\n"
        "  %s c -l6 -T4 -o data.lzp ./data\n"
        "  %s x data.lzp -C ./restore --include \"**/*.txt\"\n"
        "  %s l -L --sort size data.lzp\n"
        "  %s t data.lzp\n",
        kProgramName, kProgramVersion, kProgramName, kProgramName, kProgramName, kProgramName,
        kProgramName);
}

bool printCommandHelp(const std::string& command) {
    for (const auto& help : commandHelp()) {
        if (help.name != command) continue;
        std::printf("Usage:\n  %s\n\nDescription:\n  %s\n", help.usage.c_str(), help.summary.c_str());
        if (!help.options.empty()) {
            std::printf("\nOptions:\n");
            for (const auto& option : help.options) std::printf("%s\n", option.c_str());
        }
        std::printf("\nGlobal options: -v/--verbose, -q/--quiet, --color/--no-color, --no-progress\n");
        return true;
    }
    return false;
}

ExitCode run(const std::vector<std::string>& arguments) {
    display::initialize();

    if (arguments.empty()) {
        printHelp();
        return ExitCode::Ok;
    }

    std::string command = arguments.front();
    if (command == "--help" || command == "-h") {
        printHelp();
        return ExitCode::Ok;
    }
    if (command == "--version" || command == "-V") {
        std::printf("%s %s\n", kProgramName, kProgramVersion);
        return ExitCode::Ok;
    }

    if (command == "version") {
        std::printf("%s %s\n", kProgramName, kProgramVersion);
        return ExitCode::Ok;
    }

    if (command == "help") {
        if (arguments.size() >= 2) {
            std::string topic = arguments[1];
            if (topic == "c" || topic == "a") topic = "compress";
            if (topic == "x" || topic == "e") topic = "extract";
            if (topic == "l" || topic == "ls") topic = "list";
            if (topic == "t") topic = "test";
            if (printCommandHelp(topic)) return ExitCode::Ok;
            fail(ErrorKind::InvalidArgument,
                 fmt("unknown command: {} (run lzpack help for a list)", topic));
        }
        printHelp();
        return ExitCode::Ok;
    }

    const std::vector<std::string> tail(arguments.begin() + 1, arguments.end());

    if (command == "selftest") {
        const ParsedArguments parsed =
            parseArguments(tail, {"color", "no-color", "quiet", "verbose"});
        applyGlobalOptions(parsed);
        return static_cast<ExitCode>(selfTest());
    }

    if (command == "c" || command == "a" || command == "add" || command == "create") {
        command = "compress";
    } else if (command == "x" || command == "e") {
        command = "extract";
    } else if (command == "l" || command == "ls") {
        command = "list";
    } else if (command == "t") {
        command = "test";
    }

    if (command != "compress" && command != "extract" && command != "list" && command != "test") {
        fail(ErrorKind::InvalidArgument,
             fmt("unknown command: {} (run lzpack help for a list)", arguments.front()));
    }

    const std::vector<std::string> known = knownOptionsFor(command);
    const ParsedArguments parsed = parseArguments(tail, known);
    applyGlobalOptions(parsed);
    if (parsed.has("help") || parsed.has("h")) {
        if (printCommandHelp(command)) return ExitCode::Ok;
    }

    if (command == "compress") return static_cast<ExitCode>(compress(parsed));
    if (command == "extract") return static_cast<ExitCode>(extract(parsed));
    if (command == "list") return static_cast<ExitCode>(list(parsed));
    return static_cast<ExitCode>(test(parsed));
}

}
}
