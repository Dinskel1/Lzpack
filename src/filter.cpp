#include "filter.h"

#include <cstring>

namespace lzpack {
namespace {

bool isRelocatable(std::uint32_t value) {
    return (value & 0xFFFFFF00u) == 0u || (value & 0xFFFFFF00u) == 0xFF000000u;
}

void x86BcjEncode(std::uint8_t* p, std::size_t size) {
    constexpr std::size_t kOperandSize = 4;
    if (size < kFilterMinSize) return;

    const std::size_t limit = size - kFilterMinSize;
    for (std::size_t i = 0; i < limit;) {
        if ((p[i] & 0xFEu) != 0xE8u) {
            ++i;
            continue;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, p + i + 1, kOperandSize);
        if (!isRelocatable(value)) {
            ++i;
            continue;
        }
        const std::uint32_t absolute =
            static_cast<std::uint32_t>(value + static_cast<std::uint32_t>(i) + 5);
        std::memcpy(p + i + 1, &absolute, kOperandSize);
        i += 5;
    }
}

void x86BcjDecode(std::uint8_t* p, std::size_t size) {
    constexpr std::size_t kOperandSize = 4;
    if (size < kFilterMinSize) return;

    const std::size_t limit = size - kFilterMinSize;
    for (std::size_t i = 0; i < limit;) {
        if ((p[i] & 0xFEu) != 0xE8u) {
            ++i;
            continue;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, p + i + 1, kOperandSize);
        if (!isRelocatable(value)) {
            ++i;
            continue;
        }
        const std::uint32_t relative =
            static_cast<std::uint32_t>(value - (static_cast<std::uint32_t>(i) + 5));
        std::memcpy(p + i + 1, &relative, kOperandSize);
        i += 5;
    }
}

void deltaEncode(std::uint8_t* p, std::size_t size, std::size_t delta) {
    if (delta == 0 || size <= delta) return;
    for (std::size_t i = delta; i < size; ++i) {
        p[i] = static_cast<std::uint8_t>(p[i] - p[i - delta]);
    }
}

void deltaDecode(std::uint8_t* p, std::size_t size, std::size_t delta) {
    if (delta == 0 || size <= delta) return;
    for (std::size_t i = size; i-- > delta;) {
        p[i] = static_cast<std::uint8_t>(p[i] + p[i - delta]);
    }
}

}

void filterEncode(const FilterSpec& spec, std::uint8_t* data, std::size_t size) {
    switch (spec.kind) {
        case FilterKind::None: break;
        case FilterKind::X86Bcj: x86BcjEncode(data, size); break;
        case FilterKind::Delta: deltaEncode(data, size, spec.delta == 0 ? 1 : spec.delta); break;
    }
}

void filterDecode(const FilterSpec& spec, std::uint8_t* data, std::size_t size) {
    switch (spec.kind) {
        case FilterKind::None: break;
        case FilterKind::X86Bcj: x86BcjDecode(data, size); break;
        case FilterKind::Delta: deltaDecode(data, size, spec.delta == 0 ? 1 : spec.delta); break;
    }
}

}
