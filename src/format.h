#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "codec.h"
#include "errors.h"
#include "fio.h"
#include "filter.h"

namespace lzpack {

constexpr std::uint32_t kArchiveMagic = 0x4B505A4Cu;
constexpr std::uint32_t kArchiveVersion = 1;

constexpr std::size_t kArchiveHeaderSize = 28;

constexpr std::size_t kArchiveFooterSize = 36;

constexpr std::size_t kHeaderIndexCrcOffset = 24;

enum ArchiveFlags : std::uint16_t {
    kFlagTextMode = 1u << 0,
};
constexpr std::uint16_t kArchiveKnownFlags = kFlagTextMode;

struct BlockHeader {
    std::uint64_t originalSize = 0;
    std::uint64_t compressedSize = 0;
    std::uint32_t crc = 0;
    std::uint8_t level = 0;
    std::uint8_t windowLog2 = 0;
    std::uint8_t filterKind = 0;
    std::uint8_t delta = 1;
};

inline BlockInfo blockInfoOf(const BlockHeader& header) {
    BlockInfo info;
    info.level = static_cast<CompressionLevel>(header.level);
    info.windowSize = log2ToWindow(header.windowLog2);
    info.filter.kind = static_cast<FilterKind>(header.filterKind);
    info.filter.delta = header.delta == 0 ? 1 : header.delta;
    info.textMode = false;
    return info;
}

inline BlockHeader blockHeaderOf(const BlockInfo& info, std::uint64_t originalSize,
                                 std::uint64_t compressedSize, std::uint32_t crc) {
    BlockHeader header;
    header.originalSize = originalSize;
    header.compressedSize = compressedSize;
    header.crc = crc;
    header.level = static_cast<std::uint8_t>(info.level);
    header.windowLog2 = windowToLog2(info.windowSize);
    header.filterKind = static_cast<std::uint8_t>(info.filter.kind);
    header.delta = info.filter.delta == 0 ? 1 : info.filter.delta;
    return header;
}


struct BlockRecord {
    BlockHeader header;
    std::string payload;
};

struct ArchiveEntry {
    std::string path;
    std::uint64_t originalSize = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t blockOffset = 0;
    std::uint64_t blockCount = 0;
    std::uint32_t crc = 0;
    std::uint32_t mode = 0;
    std::int64_t modifiedTime = 0;
    CompressionLevel level = CompressionLevel::Normal;
    FilterSpec filter;
};

enum EntryMode : std::uint32_t {
    kEntryDirectory = 1u << 0,
    kEntryText = 1u << 1,
    kEntryEmpty = 1u << 2,
};

struct ArchiveHeader {
    std::uint16_t version = kArchiveVersion;
    std::uint16_t flags = 0;
    std::uint32_t blockSize = 0;
    CompressionLevel level = CompressionLevel::Normal;
    std::uint32_t reserved = 0;
};

struct ArchiveFooter {
    std::uint64_t indexOffset = 0;
    std::uint64_t indexSize = 0;
    std::uint64_t entryCount = 0;
    std::uint32_t indexCrc = 0;
    std::uint16_t version = kArchiveVersion;
    std::uint16_t flags = 0;
};

struct ArchiveStats {
    std::uint64_t inputBytes = 0;
    std::uint64_t outputBytes = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t indexBytes = 0;
    std::uint64_t entryCount = 0;
    std::uint64_t blockCount = 0;
    std::uint64_t codecInputBytes = 0;
    std::uint64_t codecOutputBytes = 0;
    double seconds = 0.0;
    CodecStats codec;

    double ratioPercent() const {
        if (inputBytes == 0) return 0.0;
        return 100.0 * static_cast<double>(outputBytes) / static_cast<double>(inputBytes);
    }

    double codecRatioPercent() const {
        if (codecInputBytes == 0) return 0.0;
        return 100.0 * static_cast<double>(codecOutputBytes) / static_cast<double>(codecInputBytes);
    }
};

struct ArchiveLimits {
    std::uint64_t maxEntryCount = 4u << 20;
    std::uint64_t maxPathLength = 1u << 16;
    std::uint64_t maxBlockSize = 1ull << 31;
};

struct ProgressInfo {
    enum class Phase { Analyze, Compress, Write, Extract, Verify } phase = Phase::Compress;
    std::string currentPath;
    std::uint64_t totalBytes = 0;
    std::uint64_t doneBytes = 0;
};
using ProgressHandler = std::function<void(const ProgressInfo&)>;

struct CompressionOptions {
    CodecParams codec;
    bool archiveMode = false;
    bool keepEmptyDirectories = true;
    bool autoFilter = true;
    bool autoText = false;
    bool verifyAfterWrite = false;
    unsigned threads = 1;
};

void createArchive(const std::vector<std::string>& sources, const std::string& outputPath,
                   const CompressionOptions& options, const ProgressHandler& progress,
                   ArchiveStats* stats);

void compressStream(fio::Reader& input, fio::Writer& output, const std::string& entryPath,
                    std::uint64_t inputSize, std::int64_t modifiedTime,
                    const CompressionOptions& options, const ProgressHandler& progress,
                    ArchiveStats* stats);

ArchiveFooter readIndex(fio::Reader& input, std::vector<ArchiveEntry>& entries,
                        ArchiveHeader* header, const ArchiveLimits& limits);

struct ExtractionOptions {
    std::string destination = ".";
    std::vector<std::string> includes;
    std::vector<std::string> excludes;
    bool overwrite = true;
    bool restoreTimestamps = true;
    bool verifyCrc = true;
    bool dryRun = false;
    bool flatten = false;
};

struct ExtractionStats {
    std::uint64_t entriesExtracted = 0;
    std::uint64_t entriesSkipped = 0;
    std::uint64_t bytesWritten = 0;
    double seconds = 0.0;
    std::vector<std::string> failures;
};

void extractArchive(fio::Reader& input, const std::string& archivePath,
                    const std::vector<ArchiveEntry>& entries, const ExtractionOptions& options,
                    const ProgressHandler& progress, ExtractionStats* stats);

void verifyArchive(fio::Reader& input, const std::vector<ArchiveEntry>& entries,
                   const ProgressHandler& progress, ExtractionStats* stats);

bool matchPattern(const std::string& pattern, const std::string& path);

bool entrySelected(const std::string& path, const std::vector<std::string>& includes,
                   const std::vector<std::string>& excludes);

bool normalizeCrLf(std::vector<std::uint8_t>& data);

void restoreCrLf(std::vector<std::uint8_t>& data);

bool looksLikeText(const std::uint8_t* data, std::size_t size);

std::uint64_t buildSelfExtractor(const std::string& stubPath, const std::string& archivePath,
                                 const std::string& outputPath);

}
