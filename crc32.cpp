#include "crc32.h"

#include <vector>

#include "fio.h"

namespace lzpack {
namespace {

struct Crc32Table {
    std::uint32_t entries[256];

    Crc32Table() {
        constexpr std::uint32_t kPolynomial = 0xEDB88320u;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (value >> 1) ^ kPolynomial : (value >> 1);
            }
            entries[i] = value;
        }
    }
};

const Crc32Table& sharedTable() {
    static const Crc32Table table;
    return table;
}

}

const std::uint32_t* Crc32::table() { return sharedTable().entries; }

std::uint32_t Crc32::computeFile(const std::string& path) {
    fio::FileReader reader(path);
    Crc32 crc;
    std::vector<std::uint8_t> buffer(256 * 1024);
    for (;;) {
        const std::size_t got = reader.read(buffer.data(), buffer.size());
        if (got == 0) break;
        crc.update(buffer.data(), got);
    }
    return crc.value();
}

}
