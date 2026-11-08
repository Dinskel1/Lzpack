#include "codec.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "bytes.h"
#include "crc32.h"
#include "errors.h"
#include "format.h"
#include "lzma_common.h"
#include "matchfinder.h"
#include "rangecoder.h"

namespace lzpack {

const char* compressionLevelName(CompressionLevel level) {
    switch (level) {
        case CompressionLevel::Store: return "store";
        case CompressionLevel::Fastest: return "fastest";
        case CompressionLevel::Fast: return "fast";
        case CompressionLevel::Normal: return "normal";
        case CompressionLevel::Good: return "good";
        case CompressionLevel::Max: return "max";
        case CompressionLevel::Ultra: return "ultra";
    }
    return "unknown";
}

const LzmaModels& LzmaModels::initial() {
    static const LzmaModels models = [] {
        LzmaModels value;

        auto* begin = reinterpret_cast<Probability*>(&value);
        const std::size_t count = sizeof(LzmaModels) / sizeof(Probability);
        for (std::size_t i = 0; i < count; ++i) begin[i] = kProbabilityInit;
        return value;
    }();
    return models;
}

const StateTable& StateTable::get() {

    static const StateTable table = [] {
        StateTable value{};
        const std::uint8_t literalNext[kPositionStateCount] = {0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5};
        const std::uint8_t matchNext[kPositionStateCount] = {7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10};
        const std::uint8_t repNext[kPositionStateCount] = {8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11};
        const std::uint8_t shortRepNext[kPositionStateCount] = {9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11};
        std::memcpy(value.literalNext, literalNext, sizeof(literalNext));
        std::memcpy(value.matchNext, matchNext, sizeof(matchNext));
        std::memcpy(value.repNext, repNext, sizeof(repNext));
        std::memcpy(value.shortRepNext, shortRepNext, sizeof(shortRepNext));
        return value;
    }();
    return table;
}

namespace {

constexpr std::uint32_t kDeferralGain = 1;

constexpr std::uint32_t kDeferralLengthLimit = 64;

constexpr std::size_t kStoreChunk = 256 * 1024;

inline std::uint32_t minU32(std::uint32_t a, std::uint32_t b) { return a < b ? a : b; }

const std::uint8_t kDistanceSlotTable[kDistanceSlotCount] = {
    0,  1,  2,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7,
    8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11};

const std::uint32_t kDistanceBase[kDistanceSlotCount] = {
    0,      1,      2,      3,      4,      6,      8,      12,
    16,     24,     32,     48,     64,     96,     128,    192,
    256,    384,    512,    768,    1024,   1536,   2048,   3072,
    4096,   6144,   8192,   12288,  16384,  24576,  32768,  49152,
    65536,  98304,  131072, 196608, 262144, 393216, 524288, 786432,
    1048576, 1572864, 2097152, 3145728, 4194304, 6291456, 8388608, 12582912,
    16777216, 25165824, 33554432, 50331648, 67108864, 100663296, 134217728,
    201326592, 268435456, 402653184, 536870912, 805306368, 1073741824,
    1610612736, 2147483648u, 3221225472u};

inline std::uint32_t distanceSlot(std::uint32_t distance) {
    if (distance < 64) return kDistanceSlotTable[distance];
    std::uint32_t bits = 0;
    for (std::uint32_t value = distance; value != 0; value >>= 1) ++bits;
    const std::uint32_t nextBit = (distance >> (bits - 2)) & 1u;
    const std::uint32_t slot = 2u * bits - 2u + nextBit;
    return (slot < kDistanceSlotCount) ? slot : (kDistanceSlotCount - 1);
}

inline std::uint32_t distanceExtraBits(std::uint32_t slot) {
    return (slot < kDistanceStartModel) ? 0u : ((slot >> 1) - 1u);
}

inline std::uint32_t literalContext(const std::uint8_t* data, std::uint32_t position,
                                    std::uint32_t state) {
    (void)state;
    const std::uint32_t previous = (position == 0) ? 0u : data[position - 1];
    return previous >> (8 - kLiteralContextPrevBits);
}

inline std::uint8_t matchedLiteralByte(const std::uint8_t* data, std::uint32_t position,
                                       std::uint32_t rep0) {
    if (position == 0 || rep0 == 0 || rep0 > position) return 0;
    return data[position - rep0];
}

struct StringWriter final : fio::Writer {
    std::string& buffer;
    explicit StringWriter(std::string& target) : buffer(target) {}
    void write(const void* data, std::size_t size) override {
        buffer.append(static_cast<const char*>(data), size);
    }
    void flush() override {}
};

void encodeLiteralByte(RangeEncoder& encoder, LzmaModels& models, bool useMatched,
                       std::uint8_t matchByte, std::uint8_t byte, Probability* tree) {
    std::uint32_t symbol = 1;
    for (int i = 7; i >= 0; --i) {
        const unsigned bit = (byte >> i) & 1u;
        if (useMatched) {
            const unsigned matchBit = (matchByte >> i) & 1u;

            encoder.encodeBit(models.literalMatched[((1 + matchBit) << 8) + symbol], bit);
            if (matchBit != bit) useMatched = false;
        } else {
            encoder.encodeBit(tree[symbol], bit);
        }
        symbol = (symbol << 1) | bit;
    }
}

std::uint32_t literalPrice(const LzmaModels& models, const Probability* tree, bool useMatched,
                           std::uint8_t matchByte, std::uint8_t byte) {
    std::uint32_t price = 0;
    std::uint32_t symbol = 1;
    for (int i = 7; i >= 0; --i) {
        const unsigned bit = (byte >> i) & 1u;
        if (useMatched) {
            const unsigned matchBit = (matchByte >> i) & 1u;
            price += RangeEncoder::bitPrice(models.literalMatched[((1 + matchBit) << 8) + symbol], bit);
            if (matchBit != bit) useMatched = false;
        } else {
            price += RangeEncoder::bitPrice(tree[symbol], bit);
        }
        symbol = (symbol << 1) | bit;
    }
    return price;
}

void encodeBitTreeReverseRuntime(RangeEncoder& encoder, Probability* tree, std::uint32_t symbol,
                                std::uint32_t bits) {
    std::uint32_t node = 1;
    for (std::uint32_t i = 0; i < bits; ++i) {
        const unsigned bit = (symbol >> i) & 1u;
        encoder.encodeBit(tree[node], bit);
        node = (node << 1) | bit;
    }
}

std::uint32_t decodeBitTreeReverseRuntime(RangeDecoder& decoder, Probability* tree,
                                          std::uint32_t bits) {
    std::uint32_t node = 1;
    std::uint32_t symbol = 0;
    for (std::uint32_t i = 0; i < bits; ++i) {
        const unsigned bit = decoder.decodeBit(tree[node]);
        node = (node << 1) | bit;
        symbol |= bit << i;
    }
    return symbol;
}

void encodeDistance(RangeEncoder& encoder, LzmaModels& models, std::uint32_t distance) {
    const std::uint32_t slot = distanceSlot(distance);
#ifdef LZPACK_TRACE
    std::fprintf(stderr, "      DIST-ENC dist=%u slot=%u base=%u extra=%u\n", distance, slot,
                 slot < kDistanceSlotCount ? kDistanceBase[slot] : 0u, distanceExtraBits(slot));
#endif
    encodeBitTree<kDistanceSlotMaxBits>(encoder, models.posSlot[0], slot);
    if (slot < kDistanceStartModel) return;

    const std::uint32_t extraBits = distanceExtraBits(slot);
    const std::uint32_t rest = distance - kDistanceBase[slot];
    Probability* tree = models.distanceSpecial + ((slot - kDistanceStartModel) << 5);

    const std::uint32_t treeBits = (extraBits < 4) ? extraBits : 4;
    encodeBitTreeReverseRuntime(encoder, tree, rest & ((1u << treeBits) - 1u), treeBits);
    std::uint32_t remaining = rest >> treeBits;
    for (std::uint32_t i = treeBits; i < extraBits; ++i) {
        encoder.encodeBit(models.directBits[i], remaining & 1u);
        remaining >>= 1;
    }
}

std::uint32_t decodeDistance(RangeDecoder& decoder, LzmaModels& models) {
    const std::uint32_t slot = decodeBitTree<kDistanceSlotMaxBits>(decoder, models.posSlot[0]);
#ifdef LZPACK_TRACE
    std::fprintf(stderr, "      DIST-DEC slot=%u\n", slot);
#endif
    if (slot >= kDistanceSlotCount) {
        fail(ErrorKind::Corrupt, fmt("corrupt data: distance slot {} is out of range", slot));
    }
    if (slot < kDistanceStartModel) return slot;

    const std::uint32_t extraBits = distanceExtraBits(slot);
    Probability* tree = models.distanceSpecial + ((slot - kDistanceStartModel) << 5);
    const std::uint32_t treeBits = (extraBits < 4) ? extraBits : 4;
    const std::uint32_t low = decodeBitTreeReverseRuntime(decoder, tree, treeBits);
    std::uint32_t high = 0;
    for (std::uint32_t i = treeBits; i < extraBits; ++i) {
        high |= decoder.decodeBit(models.directBits[i]) << (i - treeBits);
    }
    return kDistanceBase[slot] + (high << treeBits) + low;
}

std::uint32_t distancePrice(const LzmaModels& models, std::uint32_t distance) {
    const std::uint32_t slot = distanceSlot(distance);
    std::uint32_t price = 0;
    std::uint32_t node = 1;
    for (int i = kDistanceSlotMaxBits - 1; i >= 0; --i) {
        const unsigned bit = (slot >> i) & 1u;
        price += RangeEncoder::bitPrice(models.posSlot[0][node], bit);
        node = (node << 1) | bit;
    }
    if (slot >= kDistanceStartModel) {

        price += ((slot >> 1) - 1) * kProbabilityOne + 2 * kProbabilityOne;
    }
    return price;
}

std::uint32_t lengthPrice(std::uint32_t value) {
    if (value < kLengthTier0Limit) return (kLengthTierBits + kLengthTier0Bits) * kProbabilityOne / 2;
    if (value < kLengthTier1Limit) return (kLengthTierBits + kLengthTier0Bits) * kProbabilityOne / 2;
    return (kLengthTierBits + kLengthTier1Bits) * kProbabilityOne / 2;
}

struct TokenWriter {
    RangeEncoder& encoder;
    LzmaModels& models;
    const StateTable& states;

    void putLiteral(std::uint32_t& state, const std::uint8_t* data, std::uint32_t position,
                    std::uint8_t byte, std::uint32_t rep0) {
        const std::uint32_t context = literalContext(data, position, state);
        encoder.encodeBit(models.isMatch[state], 0);
        const bool useMatched = state >= 7;
        const std::uint8_t matchByte =
            useMatched ? matchedLiteralByte(data, position, rep0) : static_cast<std::uint8_t>(0);
        encodeLiteralByte(encoder, models, useMatched, matchByte, byte,
                          models.literal + context * kLiteralModelCount);
        state = states.literalNext[state];
    }

    void putRep(std::uint32_t& state, std::uint32_t repIndex, std::uint32_t length) {
#ifdef LZPACK_TRACE
        std::fprintf(stderr, "   >> REP idx=%u len=%u state=%u\n", repIndex, length, state);
#endif
        const bool shortRep = (repIndex == 0 && length == 1);
        encoder.encodeBit(models.isMatch[state], 1);
        encoder.encodeBit(models.isRep[state], 1);
#ifdef LZPACK_TRACE
        std::fprintf(stderr, "   putRep state=%u idx=%u len=%u short=%d\n", state, repIndex, length,
                     shortRep ? 1 : 0);
#endif
        if (repIndex == 0) {
            encoder.encodeBit(models.isRepG0[state], 0);
            encoder.encodeBit(models.isRep0Long[state], shortRep ? 1 : 0);
#ifdef LZPACK_TRACE
            std::fprintf(stderr, "   PUT isRepG0=0 isRep0Long=%d\n", shortRep ? 1 : 0);
#endif
        } else {
            encoder.encodeBit(models.isRepG0[state], 1);
            if (repIndex == 1) {
                encoder.encodeBit(models.isRepG1[state], 0);
            } else {
                encoder.encodeBit(models.isRepG1[state], 1);
                encoder.encodeBit(models.isRepG2[state], repIndex - 2);
            }
        }
        if (!shortRep) {
            LengthEncoder{models.repLenCoder}.encode(encoder, length - 2);
#ifdef LZPACK_TRACE
            std::fprintf(stderr, "   putRep lenValue=%u\n", length - 2);
#endif
        }
        state = shortRep ? states.shortRepNext[state] : states.repNext[state];
    }

    void putMatch(std::uint32_t& state, std::uint32_t length, std::uint32_t distance) {
#ifdef LZPACK_TRACE
        std::fprintf(stderr, "   >> MATCH len=%u dist=%u state=%u\n", length, distance, state);
#endif
        encoder.encodeBit(models.isMatch[state], 1);
        encoder.encodeBit(models.isRep[state], 0);
        LengthEncoder{models.lenCoder}.encode(encoder, length - kMinMatch);
        encodeDistance(encoder, models, distance);
        state = states.matchNext[state];
    }
};

struct Candidate {
    std::uint32_t length = 0;
    std::uint32_t distance = 0;
    std::uint32_t repIndex = 0;
    bool isRep = false;
    std::uint32_t price = 0;

    bool found() const noexcept { return length != 0; }
};

Candidate findRepCandidate(const std::uint32_t* reps, const std::uint8_t* data,
                           std::uint32_t dataSize, std::uint32_t position, std::uint32_t limit) {
    Candidate best;
    const std::uint32_t available = dataSize - position;
    const std::uint32_t maxLength = minU32(available, limit);
    if (maxLength == 0) return best;

    for (std::uint32_t index = 0; index < 4; ++index) {
        const std::uint32_t distance = reps[index];
        if (distance == 0 || distance > position) continue;
        const std::uint8_t* source = data + position - distance;
        std::uint32_t length = 0;
        while (length < maxLength && source[length] == data[position + length]) ++length;
        if (length > best.length) {
            best.length = length;
            best.distance = distance;
            best.repIndex = index;
            best.isRep = true;
        }
    }
    return best;
}

void appendMatch(std::vector<std::uint8_t>& buffer, std::size_t sourceOffset,
                 std::uint32_t length) {
    buffer.reserve(buffer.size() + length);
    for (std::uint32_t i = 0; i < length; ++i) {
        buffer.push_back(buffer[sourceOffset + i]);
    }
}

}

void writeBlockHeader(const BlockInfo& info, std::uint64_t originalSize,
                      std::uint64_t compressedSize, std::uint32_t crc, std::string& buffer) {
    ByteWriter writer(buffer);
    writer.u64(originalSize);
    writer.u64(compressedSize);
    writer.u32(crc);
    writer.u8(static_cast<std::uint8_t>(info.level));
    writer.u8(windowToLog2(info.windowSize));
    writer.u8(static_cast<std::uint8_t>(info.filter.kind));
    writer.u8(info.filter.delta == 0 ? 1 : info.filter.delta);
}

BlockInfo readBlockHeader(const std::uint8_t* data, std::size_t size, std::uint64_t* originalSize,
                          std::uint64_t* compressedSize, std::uint32_t* crc) {
    if (size < kBlockHeaderSize) {
        fail(ErrorKind::Corrupt, "truncated block header");
    }
    ByteReader reader(data, kBlockHeaderSize);
    BlockInfo info;
    *originalSize = reader.u64();
    *compressedSize = reader.u64();
    *crc = reader.u32();
    const std::uint8_t level = reader.u8();
    const std::uint8_t windowLog2 = reader.u8();
    const std::uint8_t filterKind = reader.u8();
    const std::uint8_t delta = reader.u8();
    if (reader.failed()) {
        fail(ErrorKind::Corrupt, "failed to parse block header");
    }
    if (level > static_cast<std::uint8_t>(CompressionLevel::Ultra)) {
        fail(ErrorKind::Corrupt, fmt("invalid compression level in block header: {}", static_cast<unsigned>(level)));
    }
    if (filterKind > static_cast<std::uint8_t>(FilterKind::Delta)) {
        fail(ErrorKind::Corrupt,
             fmt("invalid filter type in block header: {}", static_cast<unsigned>(filterKind)));
    }
    if (windowLog2 > 31) {
        fail(ErrorKind::Corrupt,
             fmt("invalid window exponent in block header: {}", static_cast<unsigned>(windowLog2)));
    }
    info.level = static_cast<CompressionLevel>(level);
    info.windowSize = log2ToWindow(windowLog2);
    info.filter.kind = static_cast<FilterKind>(filterKind);
    info.filter.delta = delta == 0 ? 1 : delta;
    return info;
}

std::uint8_t windowToLog2(std::uint32_t windowSize) {
    std::uint8_t bits = 0;
    std::uint32_t value = windowSize;
    while (value > 1 && bits < 31) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

std::uint32_t log2ToWindow(std::uint8_t log2Value) {
    if (log2Value > 31) {
        fail(ErrorKind::Corrupt, fmt("invalid window exponent: {}", static_cast<unsigned>(log2Value)));
    }
    return 1u << log2Value;
}

std::size_t compressBlock(const CodecParams& params, const std::uint8_t* data, std::size_t size,
                          const std::uint8_t* nextData, std::size_t nextSize, bool textNormalized,
                          std::string& output, const ProgressCallback& progress, CodecStats* stats) {
    (void)nextData;
    if (stats != nullptr) *stats = CodecStats{};

    const std::size_t payloadStart = output.size();
    if (size == 0) {

        if (progress) progress(0, 0);
    } else if (params.level == CompressionLevel::Store) {

        output.append(reinterpret_cast<const char*>(data), size);
        if (progress) progress(size, size);
        if (stats != nullptr) stats->literalBytes = size;
    } else {
        const std::size_t reportStep = 256 * 1024;
        std::size_t nextReport = reportStep;
        const auto report = [&](std::size_t done) {
            if (progress) progress(done, size);
        };

        StringWriter sink(output);
        RangeEncoder encoder(sink);
        LzmaModels models = LzmaModels::initial();
        const StateTable& states = StateTable::get();
        TokenWriter writer{encoder, models, states};

        MatchFinder finder(params.windowSize, static_cast<std::uint32_t>(size));

        std::uint32_t nextInsert = 0;
        std::uint32_t nextValid = 0;
        const std::uint32_t hashableEnd =
            (size >= kMinMatch) ? static_cast<std::uint32_t>(size) - kMinMatch : 0;
        const auto insertUpTo = [&](std::uint32_t limit) {
            if (limit > hashableEnd) limit = hashableEnd;
            if (nextInsert <= limit) {
                nextInsert = finder.insert(data, nextInsert, limit + 1 - nextInsert);
            }
        };

        std::uint32_t reps[4] = {0, 0, 0, 0};
        std::uint32_t state = 0;

        for (std::uint32_t position = 0; position < size;) {
            insertUpTo(nextValid);
            nextValid = position + 1;

            const std::size_t localLimit = std::min<std::size_t>(nextSize, kMaxMatchLength);
            const std::uint32_t maxLength = static_cast<std::uint32_t>(
                std::min<std::size_t>(size - position, kMaxMatchLength) + localLimit);

            const Match match = finder.findBest(data, static_cast<std::uint32_t>(size), position,
                                                params.searchDepth, params.niceLength);
            const Candidate rep =
                findRepCandidate(reps, data, static_cast<std::uint32_t>(size), position, maxLength);

            Candidate best;

            if (match.found() && match.length >= kMinMatch) {
                best.length = match.length;
                best.distance = match.distance;
                best.isRep = false;
                best.price = RangeEncoder::bitPrice(models.isMatch[state], 1) +
                             RangeEncoder::bitPrice(models.isRep[state], 0) +
                             lengthPrice(match.length - kMinMatch) +
                             distancePrice(models, match.distance);
            }
            if (rep.found() && rep.length >= 1) {
                const std::uint32_t repPrice = RangeEncoder::bitPrice(models.isMatch[state], 1) +
                                               RangeEncoder::bitPrice(models.isRep[state], 1) +
                                               lengthPrice(rep.length >= 2 ? rep.length - 2 : 0);

                if (!best.found() || (rep.length >= best.length && repPrice <= best.price)) {
                    best = rep;
                    best.isRep = true;
                    best.price = repPrice;
                }
            }

            bool defer = false;
            if (params.lazyMatching && best.found() && best.length >= 2 &&
                best.length < kDeferralLengthLimit && position + 1 < size) {
                const std::size_t nextLocal =
                    std::min<std::size_t>(nextSize > 1 ? nextSize - 1 : 0, kMaxMatchLength);
                const std::uint32_t nextMaxLength = static_cast<std::uint32_t>(
                    std::min<std::size_t>(size - position - 1, kMaxMatchLength) + nextLocal);
                if (nextMaxLength > best.length) {
                    insertUpTo(position + 1);
                    const Match nextMatch =
                        finder.findBest(data, static_cast<std::uint32_t>(size), position + 1,
                                        params.searchDepth, params.niceLength);
                    const Candidate nextRep = findRepCandidate(
                        reps, data, static_cast<std::uint32_t>(size), position + 1, nextMaxLength);
                    const std::uint32_t nextLength = std::max(nextMatch.length, nextRep.length);
                    if (nextLength > best.length + kDeferralGain) {

                        const std::uint32_t context = literalContext(data, position, state);
                        const bool useMatched = state >= 7;
                        const std::uint8_t matchByte =
                            useMatched ? matchedLiteralByte(data, position, reps[0])
                                       : static_cast<std::uint8_t>(0);
                        const std::uint32_t literal = literalPrice(
                            models, models.literal + context * kLiteralModelCount, useMatched,
                            matchByte, data[position]);
                        const std::uint32_t gain = (nextLength - best.length) * 64;
                        defer = literal <= gain;
                    }
                }
            }

            if (!best.found() || defer) {
#ifdef LZPACK_TRACE
                std::fprintf(stderr, "[enc] pos=%u LIT state=%u\n", position, state);
#endif
                writer.putLiteral(state, data, position, data[position], reps[0]);
                ++position;
                if (stats != nullptr) {
                    ++stats->literalCount;
                    ++stats->literalBytes;
                }
            } else if (best.isRep) {
#ifdef LZPACK_TRACE
                std::fprintf(stderr, "[enc] pos=%u REP state=%u len=%u idx=%u\n", position, state,
                             best.length, best.repIndex);
#endif
                writer.putRep(state, best.repIndex, best.length);
                const std::uint32_t distance = reps[best.repIndex];
                for (std::uint32_t i = best.repIndex; i > 0; --i) reps[i] = reps[i - 1];
                reps[0] = distance;
                position += best.length;
                if (stats != nullptr) {
                    ++stats->repMatchCount;
                    stats->matchBytes += best.length;
                }
            } else {
#ifdef LZPACK_TRACE
                std::fprintf(stderr, "[enc] pos=%u MATCH state=%u len=%u dist=%u\n", position, state,
                             best.length, best.distance);
#endif
                writer.putMatch(state, best.length, best.distance);
                reps[3] = reps[2];
                reps[2] = reps[1];
                reps[1] = reps[0];
                reps[0] = best.distance;
                position += best.length;
                if (stats != nullptr) {
                    ++stats->matchCount;
                    stats->matchBytes += best.length;
                }
            }

            if (position >= nextReport || position == size) {
                report(position);
                nextReport = static_cast<std::size_t>(position) + reportStep;
            }
        }

        encoder.flush();
    }

    const std::size_t payloadSize = output.size() - payloadStart;
    const std::uint32_t crc = Crc32::compute(data, size);
    std::string header;
    writeBlockHeader(BlockInfo{params.level, params.windowSize, params.filter, textNormalized},
                     size, payloadSize, crc, header);
    output.insert(payloadStart, header);
    return output.size() - payloadStart;
}

std::uint32_t decompressBlock(const BlockInfo& info, const std::uint8_t* source, std::size_t size,
                              std::size_t expectedSize, std::uint32_t expectedCrc,
                              std::vector<std::uint8_t>& out, const ProgressCallback& progress,
                              CodecStats* stats) {
    if (stats != nullptr) *stats = CodecStats{};
    out.clear();

    if (expectedSize == 0) {
        if (size != 0) {
            fail(ErrorKind::Corrupt,
                 fmt("{} trailing bytes after empty block", size));
        }
        if (progress) progress(0, 0);
        return Crc32::compute(nullptr, 0);
    }

    if (info.level == CompressionLevel::Store) {
        if (size != expectedSize) {
            fail(ErrorKind::Corrupt,
                 fmt("stored block size mismatch: expected {}, got {} bytes", expectedSize, size));
        }
        out.assign(source, source + size);
        const std::uint32_t crc = Crc32::compute(out.data(), out.size());
        if (expectedCrc != 0 && crc != expectedCrc) {
            fail(ErrorKind::Corrupt, "stored block checksum mismatch");
        }
        if (progress) progress(size, size);
        if (stats != nullptr) stats->literalBytes = size;
        return crc;
    }

    out.reserve(expectedSize);
    fio::MemoryReader input(source, size);
    RangeDecoder decoder(input);
    LzmaModels models = LzmaModels::initial();
    const StateTable& states = StateTable::get();
    std::uint32_t reps[4] = {0, 0, 0, 0};
    std::uint32_t state = 0;

    const std::size_t reportStep = 256 * 1024;
    std::size_t nextReport = reportStep;

    while (out.size() < expectedSize) {
        const std::size_t current = out.size();
        const std::uint32_t position = static_cast<std::uint32_t>(current);
        const std::uint32_t maxLength = static_cast<std::uint32_t>(
            std::min<std::size_t>(expectedSize - current, kMaxMatchLength));

        if (decoder.decodeBit(models.isMatch[state]) == 0) {
#ifdef LZPACK_TRACE
            std::fprintf(stderr, "[dec] pos=%u LIT state=%u\n", position, state);
#endif
            const std::uint32_t context = literalContext(out.data(), position, state);
            Probability* tree = models.literal + context * kLiteralModelCount;
            std::uint8_t byte = 0;
            std::uint32_t symbol = 1;
            if (state >= 7) {
                const std::uint8_t matchByte = out[position - reps[0]];
                for (int i = 7; i >= 0; --i) {
                    const unsigned matchBit = (matchByte >> i) & 1u;
                    const unsigned bit =
                        decoder.decodeBit(models.literalMatched[((1 + matchBit) << 8) + symbol]);
                    symbol = (symbol << 1) | bit;
                    byte = static_cast<std::uint8_t>((byte << 1) | bit);
                    if (matchBit != bit) {
                        for (int j = i - 1; j >= 0; --j) {
                            const unsigned tail = decoder.decodeBit(tree[symbol]);
                            symbol = (symbol << 1) | tail;
                            byte = static_cast<std::uint8_t>((byte << 1) | tail);
                        }
                        break;
                    }
                }
            } else {
                for (int i = 7; i >= 0; --i) {
                    const unsigned bit = decoder.decodeBit(tree[symbol]);
                    symbol = (symbol << 1) | bit;
                    byte = static_cast<std::uint8_t>((byte << 1) | bit);
                }
            }
            out.push_back(byte);
            state = states.literalNext[state];
            if (stats != nullptr) {
                ++stats->literalCount;
                ++stats->literalBytes;
            }
        } else if (decoder.decodeBit(models.isRep[state]) != 0) {

            std::uint32_t length = 1;
            bool shortRep = false;
            const unsigned isRepG0Bit = decoder.decodeBit(models.isRepG0[state]);
            if (isRepG0Bit == 0) {
                const unsigned isRep0LongBit = decoder.decodeBit(models.isRep0Long[state]);
#ifdef LZPACK_TRACE
                std::fprintf(stderr, "   GET isRepG0=0 isRep0Long=%u\n", isRep0LongBit);
#endif
                if (isRep0LongBit != 0) {
                    shortRep = true;
                }
            } else {                std::uint32_t index = 1;
                if (decoder.decodeBit(models.isRepG1[state]) != 0) {
                    index = 2 + decoder.decodeBit(models.isRepG2[state]);
                }
                const std::uint32_t distance = reps[index];
                for (std::uint32_t i = index; i > 0; --i) reps[i] = reps[i - 1];
                reps[0] = distance;
            }
            if (!shortRep) {
                length = LengthDecoder{models.repLenCoder}.decode(decoder) + 2;
#ifdef LZPACK_TRACE
                std::fprintf(stderr, "   getRep lenValue=%u\n", length - 2);
#endif
            }
#ifdef LZPACK_TRACE
            std::fprintf(stderr, "[dec] pos=%u REP state=%u short=%d len=%u dist=%u\n", position,
                         state, shortRep ? 1 : 0, length, reps[0]);
#endif
            if (length > maxLength) {
                fail(ErrorKind::Corrupt, "corrupt data: repeated match exceeds block size");
            }
            const std::uint32_t distance = reps[0];
            if (distance == 0 || distance > current) {
                fail(ErrorKind::Corrupt,
                     fmt("corrupt data: distance {} exceeds {} decoded bytes", distance, current));
            }
            appendMatch(out, current - distance, length);
            state = shortRep ? states.shortRepNext[state] : states.repNext[state];
            if (stats != nullptr) {
                ++stats->repMatchCount;
                stats->matchBytes += length;
            }
        } else {

            const std::uint32_t length = LengthDecoder{models.lenCoder}.decode(decoder) + kMinMatch;
            if (length > maxLength) {
                fail(ErrorKind::Corrupt, "corrupt data: match exceeds block size");
            }
            const std::uint32_t distance = decodeDistance(decoder, models);
#ifdef LZPACK_TRACE
            std::fprintf(stderr, "[dec] pos=%u MATCH state=%u len=%u dist=%u\n", position, state,
                         length, distance);
#endif
            if (distance == 0 || distance > current) {
                fail(ErrorKind::Corrupt,
                     fmt("corrupt data: distance {} exceeds {} decoded bytes", distance, current));
            }
            reps[3] = reps[2];
            reps[2] = reps[1];
            reps[1] = reps[0];
            reps[0] = distance;
            appendMatch(out, current - distance, length);
            state = states.matchNext[state];
            if (stats != nullptr) {
                ++stats->matchCount;
                stats->matchBytes += length;
            }
        }

        if (out.size() >= nextReport || out.size() == expectedSize) {
            if (progress) progress(out.size(), expectedSize);
            nextReport = out.size() + reportStep;
        }
    }

    if (out.size() != expectedSize) {
        fail(ErrorKind::Corrupt, fmt("decoded size mismatch: expected {} bytes, got {}",
                                               expectedSize, out.size()));
    }

    const std::uint32_t crc = Crc32::compute(out.data(), out.size());
    if (expectedCrc != 0 && crc != expectedCrc) {
        fail(ErrorKind::Corrupt,
             fmt("block CRC mismatch (expected {:#010x}, got {:#010x})", expectedCrc, crc));
    }

    filterDecode(info.filter, out.data(), out.size());
    if (info.textMode) restoreCrLf(out);
    return crc;
}

}
