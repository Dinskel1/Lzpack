#pragma once

#include <cstddef>
#include <cstdint>

namespace lzpack {

enum class FilterKind : std::uint8_t {
    None = 0,
    X86Bcj = 1,
    Delta = 2,
};

struct FilterSpec {
    FilterKind kind = FilterKind::None;
    std::uint8_t delta = 1;

    bool isNone() const noexcept { return kind == FilterKind::None; }
};

void filterEncode(const FilterSpec& spec, std::uint8_t* data, std::size_t size);

void filterDecode(const FilterSpec& spec, std::uint8_t* data, std::size_t size);

constexpr std::size_t kFilterMinSize = 5;

}
