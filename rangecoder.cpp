#include "rangecoder.h"

#include <cmath>

#include "errors.h"

namespace lzpack {
namespace {

constexpr int kPriceShiftBits = 4;
constexpr std::uint32_t kPriceScale = 1u << kPriceShiftBits;

struct PriceTable {
    std::uint16_t entries[1 << (kProbabilityBits - kPriceShiftBits)][kPriceScale];

    PriceTable() {
        for (std::uint32_t i = 0; i < (1u << (kProbabilityBits - kPriceShiftBits)); ++i) {
            for (std::uint32_t j = 0; j < kPriceScale; ++j) {
                const std::uint32_t probability = (i << kPriceShiftBits) + j;

                double value = 0.0;
                if (probability > 0) {

                    value = -16.0 * (std::log(static_cast<double>(probability) / kProbabilityOne) /
                                     std::log(2.0));
                } else {
                    value = 16.0 * kProbabilityBits;
                }
                if (value < 0.0) value = 0.0;
                if (value > 65535.0) value = 65535.0;
                entries[i][j] = static_cast<std::uint16_t>(value + 0.5);
            }
        }
    }
};

const PriceTable& sharedPriceTable() {
    static const PriceTable table;
    return table;
}

}

std::uint32_t RangeEncoder::bitPrice(Probability prob, unsigned bit) {
    const std::uint32_t probability = (bit == 0) ? prob : (kProbabilityOne - prob);
    const auto& table = sharedPriceTable();
    return table.entries[probability >> kPriceShiftBits][probability & (kPriceScale - 1)];
}

RangeDecoder::RangeDecoder(fio::Reader& in) : in_(in) {

    std::uint8_t first = 0;
    if (in_.read(&first, 1) != 1) {
        fail(ErrorKind::Corrupt, "empty or truncated compressed data");
    }
    for (int i = 0; i < 4; ++i) {
        code_ = (code_ << 8) | readByte();
    }
}

std::uint8_t RangeDecoder::readByte() {
    std::uint8_t value = 0;
    if (in_.read(&value, 1) != 1) {
        fail(ErrorKind::Corrupt, "unexpected end of compressed data");
    }
    return value;
}

}
