#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lzpack {

class Crc32 {
public:
    Crc32() = default;

    void update(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        std::uint32_t crc = value_;
        for (std::size_t i = 0; i < size; ++i) {
            crc = table()[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
        }
        value_ = crc;
    }

    std::uint32_t value() const noexcept { return ~value_; }
    void reset() noexcept { value_ = 0xFFFFFFFFu; }

    static std::uint32_t compute(const void* data, std::size_t size) {
        Crc32 crc;
        crc.update(data, size);
        return crc.value();
    }

    static std::uint32_t computeFile(const std::string& path);

private:
    static const std::uint32_t* table();

    std::uint32_t value_ = 0xFFFFFFFFu;
};

}
