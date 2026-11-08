#pragma once

#include <cstdint>
#include <cstdio>

#include "rangecoder.h"

namespace lzpack {

constexpr std::uint32_t kPositionStateCount = 12;
constexpr std::uint32_t kLiteralContextPrevBits = 4;
constexpr std::uint32_t kLiteralContextPrevCount = 1u << kLiteralContextPrevBits;
constexpr std::uint32_t kLiteralContextCount =
    kLiteralContextPrevCount * kPositionStateCount;
constexpr std::uint32_t kLiteralTreeBits = 8;
constexpr std::uint32_t kLiteralTreeSize = 1u << kLiteralTreeBits;
constexpr std::uint32_t kLiteralModelCount = kLiteralTreeSize - 1;

constexpr std::uint32_t kLiteralMatchedSize = 3u << kLiteralTreeBits;

constexpr std::uint32_t kLengthTierBits = 2;
constexpr std::uint32_t kLengthTier0Bits = 9;
constexpr std::uint32_t kLengthTier1Bits = 10;
constexpr std::uint32_t kLengthTier0Limit = 9;
constexpr std::uint32_t kLengthTier1Limit = 17;

constexpr std::uint32_t kLengthTreeNodes = 1u << kLengthTier1Bits;

constexpr std::uint32_t kLengthCoderRows = 2;

constexpr std::uint32_t kMaxLengthValue = 270;

constexpr std::uint32_t kDistanceSlotMaxBits = 6;
constexpr std::uint32_t kDistanceSlotCount = 1u << kDistanceSlotMaxBits;
constexpr std::uint32_t kDistanceStartModel = 4;

constexpr std::uint32_t kDistanceSpecialSize = (kDistanceSlotCount - kDistanceStartModel) * 32;

struct LzmaModels {

    Probability literal[kLiteralContextCount * kLiteralModelCount];

    Probability literalMatched[kLiteralMatchedSize];

    Probability isMatch[kPositionStateCount];
    Probability isRep[kPositionStateCount];
    Probability isRepG0[kPositionStateCount];
    Probability isRepG1[kPositionStateCount];
    Probability isRepG2[kPositionStateCount];
    Probability isRep0Long[kPositionStateCount];

    Probability lenCoder[kLengthCoderRows][kLengthTreeNodes];
    Probability repLenCoder[kLengthCoderRows][kLengthTreeNodes];

    Probability posSlot[kDistanceStartModel][kDistanceSlotCount];

    Probability distanceSpecial[kDistanceSpecialSize];

    Probability directBits[32];

    static const LzmaModels& initial();
};

struct StateTable {
    std::uint8_t literalNext[kPositionStateCount];
    std::uint8_t matchNext[kPositionStateCount];
    std::uint8_t repNext[kPositionStateCount];
    std::uint8_t shortRepNext[kPositionStateCount];

    static const StateTable& get();
};

inline std::uint32_t positionStateBits(std::uint32_t state) {
    return (state < 7) ? 0u : (state < 10 ? 3u : 0u);
}

template <std::uint32_t NumBits>
inline void encodeBitTree(RangeEncoder& encoder, Probability* tree, std::uint32_t symbol) {
    std::uint32_t node = 1;
    for (int bit = static_cast<int>(NumBits) - 1; bit >= 0; --bit) {
        const unsigned value = (symbol >> bit) & 1u;
        encoder.encodeBit(tree[node], value);
        node = (node << 1) | value;
    }
}

template <std::uint32_t NumBits>
inline void encodeBitTreeReverse(RangeEncoder& encoder, Probability* tree, std::uint32_t symbol) {
    std::uint32_t node = 1;
    for (std::uint32_t i = 0; i < NumBits; ++i) {
        const unsigned value = (symbol >> i) & 1u;
        encoder.encodeBit(tree[node], value);
        node = (node << 1) | value;
    }
}

template <std::uint32_t NumBits>
inline std::uint32_t decodeBitTree(RangeDecoder& decoder, Probability* tree) {
    std::uint32_t node = 1;
    for (std::uint32_t i = 0; i < NumBits; ++i) {
        node = (node << 1) | decoder.decodeBit(tree[node]);
    }
    return node - (1u << NumBits);
}

template <std::uint32_t NumBits>
inline std::uint32_t decodeBitTreeReverse(RangeDecoder& decoder, Probability* tree) {
    std::uint32_t node = 1;
    std::uint32_t symbol = 0;
    for (std::uint32_t i = 0; i < NumBits; ++i) {
        const unsigned bit = decoder.decodeBit(tree[node]);
        node = (node << 1) | bit;
        symbol |= bit << i;
    }
    return symbol;
}

struct LengthEncoder {
    Probability (*rows)[kLengthTreeNodes];

    void encode(RangeEncoder& encoder, std::uint32_t value) {
        if (value < kLengthTier0Limit) {
            encodeBitTree<kLengthTierBits>(encoder, rows[0], 1);
            encodeBitTree<kLengthTier0Bits>(encoder, rows[1], value);
        } else if (value < kLengthTier1Limit) {
            encodeBitTree<kLengthTierBits>(encoder, rows[0], 2);
            encodeBitTree<kLengthTier0Bits>(encoder, rows[1], value - kLengthTier0Limit);
        } else {
            encodeBitTree<kLengthTierBits>(encoder, rows[0], 3);
            encodeBitTree<kLengthTier1Bits>(encoder, rows[1], value - kLengthTier1Limit);
        }
    }
};

struct LengthDecoder {
    Probability (*rows)[kLengthTreeNodes];

    std::uint32_t decode(RangeDecoder& decoder) {
        const std::uint32_t tier = decodeBitTree<kLengthTierBits>(decoder, rows[0]);
        if (tier == 1) return decodeBitTree<kLengthTier0Bits>(decoder, rows[1]);
        if (tier == 2) {
            return kLengthTier0Limit + decodeBitTree<kLengthTier0Bits>(decoder, rows[1]);
        }
        if (tier == 3) {
            return kLengthTier1Limit + decodeBitTree<kLengthTier1Bits>(decoder, rows[1]);
        }
        fail(ErrorKind::Corrupt, fmt("corrupt data: invalid length tier {}", tier));
    }
};

inline std::uint32_t distanceSlotCount(std::uint32_t dictionarySize) {
    std::uint32_t bits = 0;
    while ((1u << bits) < dictionarySize) ++bits;
    if (bits < 1) bits = 1;
    if (bits > kDistanceSlotMaxBits) bits = kDistanceSlotMaxBits;
    return bits;
}

}
