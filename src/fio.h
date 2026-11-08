#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "errors.h"

namespace lzpack {
namespace fio {

constexpr std::size_t kStreamBufferBytes = 256 * 1024;

constexpr std::size_t kCopyBufferBytes = 256 * 1024;

class Reader {
public:
    virtual ~Reader() = default;

    virtual std::size_t read(void* out, std::size_t size) = 0;

    virtual void seek(std::uint64_t offset) = 0;

    virtual std::uint64_t tell() const = 0;

    virtual std::uint64_t size() const = 0;

    virtual const std::string& path() const;
};

class Writer {
public:
    virtual ~Writer() = default;

    virtual void write(const void* data, std::size_t size) = 0;

    virtual void flush() = 0;
};

class FileReader final : public Reader {
public:
    explicit FileReader(const std::string& path);
    ~FileReader() override;

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    std::size_t read(void* out, std::size_t size) override;

    void seek(std::uint64_t offset) override;
    std::uint64_t tell() const override;

    std::uint64_t size() const noexcept override { return size_; }
    const std::string& path() const noexcept override { return path_; }

private:

    FileReader(const std::string& path, std::FILE* file);

    void ensureBuffer();

    std::string path_;
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
    std::uint8_t* buffer_ = nullptr;
    std::size_t bufferSize_ = 0;
    std::size_t bufferPos_ = 0;
    std::uint64_t bufferStart_ = 0;
};

class FileWriter final : public Writer {
public:
    explicit FileWriter(const std::string& path);
    ~FileWriter() override;

    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    void write(const void* data, std::size_t size) override;
    void flush() override;

    std::uint64_t bytesWritten() const noexcept { return bytesWritten_; }

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    std::uint64_t bytesWritten_ = 0;
    std::uint8_t* buffer_ = nullptr;
    std::size_t bufferSize_ = 0;
};

class FilePatcher final {
public:
    FilePatcher(const std::string& path, std::uint64_t offset);
    ~FilePatcher();

    FilePatcher(const FilePatcher&) = delete;
    FilePatcher& operator=(const FilePatcher&) = delete;

    void write(const void* data, std::size_t size);

private:
    std::string path_;
    std::FILE* file_ = nullptr;
};

class MemoryReader final : public Reader {
public:
    MemoryReader(const void* data, std::size_t size)
        : data_(static_cast<const std::uint8_t*>(data)), size_(size) {}

    std::size_t read(void* out, std::size_t size) override;
    void seek(std::uint64_t offset) override;
    std::uint64_t tell() const override { return position_; }
    std::uint64_t size() const noexcept override { return size_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

class MemoryWriter final : public Writer {
public:
    void write(const void* data, std::size_t size) override;
    void flush() override {}

    const std::string& data() const noexcept { return data_; }
    std::string take() { return std::move(data_); }

private:
    std::string data_;
};

bool pathExists(const std::string& path);
bool isDirectory(const std::string& path);
bool isRegularFile(const std::string& path);
bool isSymlink(const std::string& path);
std::uint64_t fileSize(const std::string& path);
void createDirectories(const std::string& path);
void ensureParentDirectory(const std::string& path);
void removeFile(const std::string& path);
void renameFile(const std::string& from, const std::string& to);

std::vector<std::string> listDirectory(const std::string& path);

std::uint64_t copyFile(const std::string& from, const std::string& to);
std::time_t lastWriteTime(const std::string& path);
void setLastWriteTime(const std::string& path, std::time_t time);

std::string describeSystemError();

std::string pathFileName(const std::string& path);
std::string pathParent(const std::string& path);
std::string pathJoin(const std::string& directory, const std::string& relative);

std::string toArchivePath(const std::string& path);

bool isSafeArchivePath(const std::string& archivePath);

}
}
