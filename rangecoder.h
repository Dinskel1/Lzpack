#pragma once

#include <cstdint>
#include <cstdio>

#include "fio.h"

namespace lzpack {

using Probability = std::uint16_t;

constexpr Probability kProbabilityBits = 11;
constexpr Probability kProbabilityOne = 1u << kProbabilityBits;
constexpr Probability kProbabilityInit = kProbabilityOne / 2;

constexpr int kProbabilityMoveBits = 5;

class RangeEncoder {
public:
    explicit RangeEncoder(fio::Writer& out) : out_(out) {}

    RangeEncoder(const RangeEncoder&) = delete;
    RangeEncoder& operator=(const RangeEncoder&) = delete;

    void encodeBit(Probability& prob, unsigned bit) {
#if defined(LZPACK_TRACE) && defined(LZPACK_TRACE_BITS)
        static long counter = 0;
        std::fprintf(stderr, "      ENC #%ld bit=%u prob=%u\n", counter++, bit, prob);
#endif
        const std::uint32_t bound = (range_ >> kProbabilityBits) * prob;
        if (bit == 0) {
            range_ = bound;
            prob += static_cast<Probability>((kProbabilityOne - prob) >> kProbabilityMoveBits);
        } else {
            low_ += bound;
            range_ -= bound;
            prob -= static_cast<Probability>(prob >> kProbabilityMoveBits);
        }
        if (range_ < kTopValue) {
            range_ <<= 8;
            shiftLow();
        }
    }

    void flush() {
        for (int i = 0; i < 5; ++i) shiftLow();
    }

    static std::uint32_t bitPrice(Probability prob, unsigned bit);

private:

    void shiftLow() {
        if (static_cast<std::uint32_t>(low_) < 0xFF000000u ||
            static_cast<std::uint32_t>(low_ >> 32) != 0) {
            do {

                const std::uint8_t byte =
                    static_cast<std::uint8_t>(cache_ + static_cast<std::uint8_t>(low_ >> 32));
                out_.write(&byte, 1);
                cache_ = 0xFF;
            } while (--cacheSize_ != 0);
            cache_ = static_cast<std::uint8_t>((low_ >> 24) & 0xFFu);
        }
        ++cacheSize_;
        low_ = (low_ & 0x00FFFFFFULL) << 8;
    }

    static constexpr std::uint32_t kTopValue = 1u << 24;

    fio::Writer& out_;
    std::uint64_t low_ = 0;
    std::uint32_t range_ = 0xFFFFFFFFu;
    std::uint8_t cache_ = 0;
    std::uint64_t cacheSize_ = 1;
};

class RangeDecoder {
public:
    explicit RangeDecoder(fio::Reader& in);

    RangeDecoder(const RangeDecoder&) = delete;
    RangeDecoder& operator=(const RangeDecoder&) = delete;

    unsigned decodeBit(Probability& prob) {
        const std::uint32_t bound = (range_ >> kProbabilityBits) * prob;
        unsigned bit = 0;
        if (code_ < bound) {
            range_ = bound;
            prob += static_cast<Probability>((kProbabilityOne - prob) >> kProbabilityMoveBits);
        } else {
            code_ -= bound;
            range_ -= bound;
            prob -= static_cast<Probability>(prob >> kProbabilityMoveBits);
            bit = 1;
        }
        if (range_ < kTopValue) {
            range_ <<= 8;
            code_ = (code_ << 8) | readByte();
        }
#if defined(LZPACK_TRACE) && defined(LZPACK_TRACE_BITS)
        static long counter = 0;
        std::fprintf(stderr, "      DEC #%ld bit=%u prob=%u\n", counter++, bit, prob);
#endif
        return bit;
    }


private:
    std::uint8_t readByte();

    static constexpr std::uint32_t kTopValue = 1u << 24;

    fio::Reader& in_;
    std::uint32_t range_ = 0xFFFFFFFFu;
    std::uint32_t code_ = 0;
};

}
