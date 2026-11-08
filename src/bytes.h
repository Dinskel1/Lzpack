#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace lzpack {

class ByteWriter {
public:
    explicit ByteWriter(std::string& buffer) : buffer_(buffer) {}

    void u8(std::uint8_t value) { buffer_.push_back(static_cast<char>(value)); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFFu));
        u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    }

    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
        }
    }

    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
        }
    }

    void bytes(const void* data, std::size_t size) {
        buffer_.append(static_cast<const char*>(data), size);
    }

    void string(const std::string& text) {
        u32(static_cast<std::uint32_t>(text.size()));
        buffer_ += text;
    }

private:
    std::string& buffer_;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size), pos_(0) {}

    std::uint8_t u8() {
        if (pos_ + 1 > size_) return failRead();
        return data_[pos_++];
    }

    std::uint16_t u16() {
        std::uint16_t v = u8();
        v |= static_cast<std::uint16_t>(u8()) << 8;
        return v;
    }

    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            v |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return v;
    }

    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            v |= static_cast<std::uint64_t>(u8()) << shift;
        }
        return v;
    }

    void bytes(void* out, std::size_t size) {
        if (pos_ + size > size_) {
            std::memset(out, 0, size);
            failRead();
            return;
        }
        std::memcpy(out, data_ + pos_, size);
        pos_ += size;
    }

    std::string string() {
        const std::uint32_t length = u32();
        if (failed_ || pos_ + length > size_) {
            failRead();
            return {};
        }
        std::string text(reinterpret_cast<const char*>(data_ + pos_), length);
        pos_ += length;
        return text;
    }

    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return size_ - pos_; }
    bool failed() const { return failed_; }

private:
    std::uint8_t failRead() {
        failed_ = true;
        pos_ = size_;
        return 0;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
    bool failed_ = false;
};

}
