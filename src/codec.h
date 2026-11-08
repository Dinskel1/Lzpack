#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "filter.h"

namespace lzpack {

enum class CompressionLevel : int {
    Store = 0,
    Fastest = 1,
    Fast = 2,
    Normal = 3,
    Good = 4,
    Max = 5,
    Ultra = 6,
};

const char* compressionLevelName(CompressionLevel level);

struct CodecParams {
    CompressionLevel level = CompressionLevel::Normal;

    std::uint32_t windowSize = 8u << 20;

    std::uint32_t searchDepth = 48;

    std::uint32_t niceLength = 96;

    bool lazyMatching = true;

    FilterSpec filter;

    bool textMode = false;

    std::uint32_t blockSize() const {
        const std::uint32_t limit = 1u << 30;
        return windowSize == 0 ? (1u << 20) : (windowSize > limit ? limit : windowSize);
    }
};

struct BlockInfo {
    CompressionLevel level = CompressionLevel::Normal;
    std::uint32_t windowSize = 0;
    FilterSpec filter;
    bool textMode = false;
};

using ProgressCallback = std::function<void(std::uint64_t done, std::uint64_t total)>;

struct CodecStats {
    std::uint64_t literalCount = 0;
    std::uint64_t literalBytes = 0;
    std::uint64_t matchCount = 0;
    std::uint64_t repMatchCount = 0;
    std::uint64_t matchBytes = 0;
};

constexpr std::size_t kBlockHeaderSize = 24;

void writeBlockHeader(const BlockInfo& info, std::uint64_t originalSize,
                      std::uint64_t compressedSize, std::uint32_t crc, std::string& buffer);

BlockInfo readBlockHeader(const std::uint8_t* data, std::size_t size,
                          std::uint64_t* originalSize, std::uint64_t* compressedSize,
                          std::uint32_t* crc);

std::uint8_t windowToLog2(std::uint32_t windowSize);
std::uint32_t log2ToWindow(std::uint8_t log2Value);

std::size_t compressBlock(const CodecParams& params, const std::uint8_t* data, std::size_t size,
                          const std::uint8_t* nextData, std::size_t nextSize, bool textNormalized,
                          std::string& output, const ProgressCallback& progress, CodecStats* stats);

std::uint32_t decompressBlock(const BlockInfo& info, const std::uint8_t* source, std::size_t size,
                              std::size_t expectedSize, std::uint32_t expectedCrc,
                              std::vector<std::uint8_t>& out, const ProgressCallback& progress,
                              CodecStats* stats);

}
