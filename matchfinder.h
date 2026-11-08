#pragma once

#include <cstddef>
#include <cstdint>

#include "errors.h"

namespace lzpack {

constexpr std::uint32_t kMinMatch = 4;
constexpr std::uint32_t kMaxMatchLength = 273;
constexpr std::uint32_t kNoMatch = 0xFFFFFFFFu;

struct Match {
    std::uint32_t length = 0;
    std::uint32_t distance = 0;

    bool found() const noexcept { return length != 0; }
};

constexpr std::uint32_t kShortHashBits = 16;
constexpr std::uint32_t kShortHashSize = 1u << kShortHashBits;

class MatchFinder {
public:

    MatchFinder(std::uint32_t dictionarySize, std::uint32_t dataSize);
    ~MatchFinder();

    MatchFinder(const MatchFinder&) = delete;
    MatchFinder& operator=(const MatchFinder&) = delete;

    void reset() noexcept;

    std::uint32_t insert(const std::uint8_t* base, std::uint32_t position, std::uint32_t count);

    Match findBest(const std::uint8_t* base, std::uint32_t dataSize, std::uint32_t position,
                   std::uint32_t depth, std::uint32_t niceLength) const;



private:
    static std::uint32_t hashValue(const std::uint8_t* p, std::uint32_t bits);
    static std::uint32_t shortHashValue(const std::uint8_t* p);

    static std::uint32_t matchLength(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t limit);

    std::uint32_t window_;
    std::uint32_t bits_;
    std::uint32_t hashMask_;
    std::uint32_t* hashTable_ = nullptr;
    std::uint32_t* chains_ = nullptr;
    std::uint32_t chainSize_ = 0;
    int* shortHash_ = nullptr;

#ifdef LZPACK_MF_VALIDATE
public:

    struct Diagnostics {
        std::uint32_t chainCapacity = 0;
        std::uint32_t hashBits = 0;
        std::uint32_t dictionarySize = 0;
    };
    Diagnostics diagnostics() const {
        return Diagnostics{chainSize_, bits_, window_};
    }
#endif
};

}
