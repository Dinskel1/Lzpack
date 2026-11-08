#include "matchfinder.h"

#include <cstring>

namespace lzpack {
namespace {

std::uint32_t chooseHashBits(std::uint32_t dictionarySize) {
    std::uint32_t bits = 16;
    while (bits < 24 && (1u << bits) < dictionarySize) ++bits;
    return bits;
}

std::uint64_t loadWord(const std::uint8_t* p, std::uint32_t available) {
    std::uint64_t word = 0;
    const std::uint32_t count = (available < 8) ? available : 8;
    std::memcpy(&word, p, count);
    return word;
}

}

MatchFinder::MatchFinder(std::uint32_t dictionarySize, std::uint32_t dataSize)
    : window_(dictionarySize), bits_(chooseHashBits(dictionarySize)) {
    if (window_ == 0) {
        fail(ErrorKind::Internal, "MatchFinder window size must not be zero");
    }
    hashMask_ = static_cast<std::uint32_t>((1ull << bits_) - 1ull);
    const std::size_t hashSize = static_cast<std::size_t>(1) << bits_;
    hashTable_ = new std::uint32_t[hashSize];
    shortHash_ = new int[kShortHashSize];

    chainSize_ = dataSize > kMinMatch ? dataSize : 1;
    chains_ = new std::uint32_t[chainSize_];
    reset();
}

MatchFinder::~MatchFinder() {
    delete[] hashTable_;
    delete[] chains_;
    delete[] shortHash_;
}

void MatchFinder::reset() noexcept {
    const std::size_t hashSize = static_cast<std::size_t>(1) << bits_;
    std::memset(hashTable_, 0xFF, hashSize * sizeof(std::uint32_t));
    std::memset(shortHash_, 0xFF, kShortHashSize * sizeof(int));
}

std::uint32_t MatchFinder::hashValue(const std::uint8_t* p, std::uint32_t bits) {

    std::uint32_t value = 0;
    std::memcpy(&value, p, 4);
    return (value * 2654435761u) >> (32 - bits);
}

std::uint32_t MatchFinder::shortHashValue(const std::uint8_t* p) {

    std::uint32_t value = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
    return (value * 2654435761u) >> (32 - kShortHashBits);
}

std::uint32_t MatchFinder::matchLength(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t limit) {
    std::uint32_t length = 0;

    while (length + 8 <= limit) {
        std::uint64_t left = 0;
        std::uint64_t right = 0;
        std::memcpy(&left, a + length, 8);
        std::memcpy(&right, b + length, 8);
        if (left != right) {
            const std::uint64_t diff = left ^ right;
#if defined(_MSC_VER)
            unsigned long index = 0;
            _BitScanForward64(&index, diff);
            return length + static_cast<std::uint32_t>(index / 8);
#else
            return length + static_cast<std::uint32_t>(__builtin_ctzll(diff) / 8);
#endif
        }
        length += 8;
    }
    while (length < limit && a[length] == b[length]) ++length;
    return length;
}

std::uint32_t MatchFinder::insert(const std::uint8_t* base, std::uint32_t position, std::uint32_t count) {

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t pos = i + 4;
        if (pos >= position + count) break;
        if (pos >= chainSize_) break;
        const std::uint32_t hash = hashValue(base + pos, bits_);
        chains_[pos - 4] = hashTable_[hash];
        hashTable_[hash] = pos;
        shortHash_[shortHashValue(base + pos)] = static_cast<int>(pos);
    }
    return position + count;
}

Match MatchFinder::findBest(const std::uint8_t* base, std::uint32_t dataSize, std::uint32_t position,
                            std::uint32_t depth, std::uint32_t niceLength) const {
    Match best;
    if (position < kMinMatch || position + kMinMatch > dataSize) return best;

    const std::uint32_t maxLength = (dataSize - position < kMaxMatchLength)
                                        ? (dataSize - position)
                                        : kMaxMatchLength;
    if (maxLength < kMinMatch) return best;

    const std::uint32_t windowStart = (position > window_) ? position - window_ : 0u;

    const int shortCandidate = shortHash_[shortHashValue(base + position)];
    if (shortCandidate < 0 || static_cast<std::uint32_t>(shortCandidate) < windowStart) {
        return best;
    }

    const std::uint32_t wordBytes = (maxLength < 8) ? maxLength : 8;
    const std::uint64_t currentWord = loadWord(base + position, wordBytes);
    std::uint32_t candidate = hashTable_[hashValue(base + position, bits_)];
    std::uint32_t remainingDepth = depth;

    while (remainingDepth-- > 0) {
        if (candidate == kNoMatch || candidate >= position) break;
        if (candidate < windowStart) break;
#ifdef LZPACK_MF_VALIDATE
        if (candidate - 4 >= chainSize_) {
            fail(ErrorKind::Internal,
                 fmt("match chain out of bounds: candidate={}, capacity={}, position={}, dataSize={}",
                               candidate, chainSize_, position, dataSize));
        }
#endif
        const std::uint32_t distance = position - candidate;

        const std::uint64_t candidateWord = loadWord(base + candidate, wordBytes);
        std::uint32_t length = 0;
        if (candidateWord == currentWord && wordBytes < maxLength) {
            length = matchLength(base + candidate, base + position, maxLength);
        } else if (candidateWord == currentWord) {
            length = maxLength;
        } else {
            length = matchLength(base + candidate, base + position, maxLength);
        }
        if (length > best.length) {
            best.length = length;
            best.distance = distance;
            if (length >= niceLength) break;
        }
        candidate = chains_[candidate - 4];
    }
    return best;
}

}
