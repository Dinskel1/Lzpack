#ifdef __unix__
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE 1
#endif
#endif

#include "fio.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <cstdio>
#endif

#ifdef _WIN32
using lzpack_file_offset = __int64;
static int lzpack_fseek64(std::FILE* file, lzpack_file_offset offset, int origin) {
    return _fseeki64(file, offset, origin);
}
static lzpack_file_offset lzpack_ftell64(std::FILE* file) {
    return _ftelli64(file);
}
#else
using lzpack_file_offset = off_t;
static int lzpack_fseek64(std::FILE* file, lzpack_file_offset offset, int origin) {
    return ::fseeko(file, offset, origin);
}
static lzpack_file_offset lzpack_ftell64(std::FILE* file) {
    return static_cast<lzpack_file_offset>(::ftello(file));
}
#endif

namespace lzpack {
namespace fio {
namespace {

#ifdef _WIN32

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], length);
    return wide;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &utf8[0], length,
                        nullptr, nullptr);
    return utf8;
}

std::wstring stripTrailingSeparators(std::wstring path) {
    while (path.size() > 1) {
        const wchar_t last = path.back();
        if (last != L'\\' && last != L'/') break;
        if (path.size() == 3 && path[1] == L':') break;
        path.pop_back();
    }
    return path;
}
#endif

bool queryStat(const std::string& path, struct stat* info) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return false;
    return _wstat64(wide.c_str(), reinterpret_cast<struct _stat64*>(info)) == 0;
#else
    return ::stat(path.c_str(), info) == 0;
#endif
}
std::FILE* openBinaryFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring widePath = toWide(path);
    const std::wstring wideMode(mode, mode + std::strlen(mode));
    std::FILE* file = _wfopen(widePath.c_str(), wideMode.c_str());
#else
    std::FILE* file = std::fopen(path.c_str(), mode);
#endif
    if (file == nullptr) {
        fail(ErrorKind::Io, fmt("cannot open file {} ({})", path, describeSystemError()));
    }
    return file;
}

std::string replaceAll(std::string text, char from, char to) {
    std::replace(text.begin(), text.end(), from, to);
    return text;
}

std::string stripLeadingSlashes(std::string path) {
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path.erase(0, 2);
    }
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    return path;
}

}

const std::string& Reader::path() const {
    static const std::string kEmpty;
    return kEmpty;
}

std::string describeSystemError() {
#ifdef _WIN32
    const DWORD code = GetLastError();
    if (code == 0) return "unknown error";
    LPWSTR message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::string text;
    if (length != 0 && message != nullptr) {
        text = toUtf8(std::wstring(message, length));
        LocalFree(message);
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) text = "system error";
    return fmt("{} (error code {})", text, static_cast<unsigned long>(code));
#else
    const int code = errno;
    return fmt("{} (errno {})", std::strerror(code), code);
#endif
}

FileReader::FileReader(const std::string& path) : FileReader(path, openBinaryFile(path, "rb")) {}

FileReader::FileReader(const std::string& path, std::FILE* file) : path_(path), file_(file) {
    if (lzpack_fseek64(file_, 0, SEEK_END) != 0) {
        std::fclose(file_);
        file_ = nullptr;
        fail(ErrorKind::Io, fmt("cannot seek to end of file {}", path));
    }
    const lzpack_file_offset end = lzpack_ftell64(file_);
    if (end < 0) {
        std::fclose(file_);
        file_ = nullptr;
        fail(ErrorKind::Io, fmt("cannot get size of {}", path));
    }
    size_ = static_cast<std::uint64_t>(end);
    std::rewind(file_);
    buffer_ = new std::uint8_t[kStreamBufferBytes];
}

FileReader::~FileReader() {
    if (file_ != nullptr) std::fclose(file_);
    delete[] buffer_;
}

void FileReader::seek(std::uint64_t offset) {
    if (offset > size_) {
        fail(ErrorKind::Corrupt,
             fmt("seek past end of {} ({} > {})", path_, offset, size_));
    }

    if (offset >= bufferStart_ && offset < bufferStart_ + bufferSize_) {
        bufferPos_ = static_cast<std::size_t>(offset - bufferStart_);
        position_ = offset;
        return;
    }
    if (lzpack_fseek64(file_, static_cast<lzpack_file_offset>(offset), SEEK_SET) != 0) {
        fail(ErrorKind::Io, fmt("failed to seek in {}", path_));
    }
    bufferSize_ = 0;
    bufferPos_ = 0;
    bufferStart_ = offset;
    position_ = offset;
}

std::uint64_t FileReader::tell() const { return position_; }

void FileReader::ensureBuffer() {
    if (bufferPos_ < bufferSize_) return;
    bufferStart_ = position_;
    bufferSize_ = std::fread(buffer_, 1, kStreamBufferBytes, file_);
    bufferPos_ = 0;
    if (bufferSize_ < kStreamBufferBytes && std::ferror(file_) != 0) {
        fail(ErrorKind::Io, fmt("failed to read {} ({})", path_, describeSystemError()));
    }
}

std::size_t FileReader::read(void* out, std::size_t size) {
    auto* destination = static_cast<std::uint8_t*>(out);
    std::size_t copied = 0;
    while (copied < size) {
        ensureBuffer();
        if (bufferSize_ == 0) break;
        const std::size_t chunk = (std::min)(bufferSize_ - bufferPos_, size - copied);
        std::memcpy(destination + copied, buffer_ + bufferPos_, chunk);
        bufferPos_ += chunk;
        position_ += chunk;
        copied += chunk;
    }
    return copied;
}

FileWriter::FileWriter(const std::string& path) : path_(path) {
    buffer_ = new std::uint8_t[kStreamBufferBytes];
    file_ = openBinaryFile(path, "wb");
}

FileWriter::~FileWriter() {
    if (file_ != nullptr) {
        if (bufferSize_ > 0) std::fwrite(buffer_, 1, bufferSize_, file_);
        std::fclose(file_);
    }
    delete[] buffer_;
}

void FileWriter::flush() {
    if (bufferSize_ == 0) return;
    const std::size_t written = std::fwrite(buffer_, 1, bufferSize_, file_);
    if (written != bufferSize_) {
        fail(ErrorKind::Io, fmt("failed to write {} ({})", path_, describeSystemError()));
    }
    bufferSize_ = 0;
}

void FileWriter::write(const void* data, std::size_t size) {
    const auto* source = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        if (bufferSize_ == kStreamBufferBytes) flush();
        const std::size_t chunk = (std::min)(size, kStreamBufferBytes - bufferSize_);
        std::memcpy(buffer_ + bufferSize_, source, chunk);
        bufferSize_ += chunk;
        bytesWritten_ += chunk;
        source += chunk;
        size -= chunk;
    }
}

FilePatcher::FilePatcher(const std::string& path, std::uint64_t offset) : path_(path) {
    file_ = openBinaryFile(path, "r+b");
    if (lzpack_fseek64(file_, static_cast<lzpack_file_offset>(offset), SEEK_SET) != 0) {
        std::fclose(file_);
        file_ = nullptr;
        fail(ErrorKind::Io, fmt("failed to seek in {}", path));
    }
}

FilePatcher::~FilePatcher() {
    if (file_ != nullptr) std::fclose(file_);
}

void FilePatcher::write(const void* data, std::size_t size) {
    if (std::fwrite(data, 1, size, file_) != size) {
        fail(ErrorKind::Io, fmt("failed to patch {} ({})", path_, describeSystemError()));
    }
}

std::size_t MemoryReader::read(void* out, std::size_t size) {
    const std::size_t chunk = (std::min)(size_ - position_, size);
    if (chunk > 0) {
        std::memcpy(out, data_ + position_, chunk);
        position_ += chunk;
    }
    return chunk;
}

void MemoryReader::seek(std::uint64_t offset) {
    if (offset > size_) {
        fail(ErrorKind::Corrupt, fmt("memory reader seek out of bounds ({} > {})", offset, size_));
    }
    position_ = static_cast<std::size_t>(offset);
}

void MemoryWriter::write(const void* data, std::size_t size) {
    data_.append(static_cast<const char*>(data), size);
}

bool pathExists(const std::string& path) {
    struct stat info;
    return queryStat(path, &info);
}

bool isDirectory(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    return queryStat(path, &info) && S_ISDIR(info.st_mode);
#endif
}

bool isRegularFile(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) == 0;
#else
    struct stat info;
    return queryStat(path, &info) && S_ISREG(info.st_mode);
#endif
}

bool isSymlink(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat info;
    return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

std::uint64_t fileSize(const std::string& path) {
    struct stat info;
    if (!queryStat(path, &info)) {
        fail(ErrorKind::Io, fmt("cannot get size of {} ({})", path, describeSystemError()));
    }
    return static_cast<std::uint64_t>(info.st_size);
}

void createDirectories(const std::string& path) {
    if (path.empty() || path == ".") return;
    if (isDirectory(path)) return;

    const std::string parent = pathParent(path);
    if (!parent.empty() && parent != path) {
        createDirectories(parent);
    }
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) {
        fail(ErrorKind::Io, fmt("cannot convert path {} to a system path", path));
    }
    if (_wmkdir(wide.c_str()) != 0 && errno != EEXIST) {
        fail(ErrorKind::Io, fmt("cannot create directory {} ({})", path, describeSystemError()));
    }
#else
    if (::mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
        fail(ErrorKind::Io, fmt("cannot create directory {} ({})", path, describeSystemError()));
    }
#endif
}

void ensureParentDirectory(const std::string& path) {
    const std::string parent = pathParent(path);
    if (!parent.empty()) createDirectories(parent);
}

void removeFile(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty() || _wremove(wide.c_str()) != 0) {
        fail(ErrorKind::Io, fmt("cannot remove {} ({})", path, describeSystemError()));
    }
#else
    if (::remove(path.c_str()) != 0) {
        fail(ErrorKind::Io, fmt("cannot remove {} ({})", path, describeSystemError()));
    }
#endif
}

void renameFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    const std::wstring wideFrom = toWide(from);
    const std::wstring wideTo = toWide(to);
    if (wideFrom.empty() || wideTo.empty() || _wrename(wideFrom.c_str(), wideTo.c_str()) != 0) {
        fail(ErrorKind::Io, fmt("cannot rename {} to {} ({})", from, to, describeSystemError()));
    }
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        fail(ErrorKind::Io, fmt("cannot rename {} to {} ({})", from, to, describeSystemError()));
    }
#endif
}

std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> children;
#ifdef _WIN32

    std::wstring pattern = toWide(path);
    if (pattern.empty()) return children;
    pattern = stripTrailingSeparators(pattern);
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW entry{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_NO_MORE_FILES) return children;
        fail(ErrorKind::Io,
             fmt("cannot list directory {} (error {})", path, static_cast<unsigned long>(code)));
    }
    do {
        const std::wstring name(entry.cFileName);
        if (name == L"." || name == L"..") continue;
        children.push_back(toUtf8(name));
        if (children.size() > (1u << 22)) {
            FindClose(handle);
            fail(ErrorKind::Io, fmt("too many entries in directory {}; aborted", path));
        }
    } while (FindNextFileW(handle, &entry) != 0);
    FindClose(handle);
#else
    DIR* directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        fail(ErrorKind::Io, fmt("cannot list directory {} ({})", path, describeSystemError()));
    }
    while (struct dirent* item = ::readdir(directory)) {
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        children.push_back(name);
    }
    ::closedir(directory);
#endif
    std::sort(children.begin(), children.end());
    return children;
}

std::uint64_t copyFile(const std::string& from, const std::string& to) {
    FileReader reader(from);
    FileWriter writer(to);
    std::vector<std::uint8_t> buffer(kCopyBufferBytes);
    std::uint64_t total = 0;
    for (;;) {
        const std::size_t got = reader.read(buffer.data(), buffer.size());
        if (got == 0) break;
        writer.write(buffer.data(), got);
        total += got;
    }
    writer.flush();
    return total;
}

std::time_t lastWriteTime(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &info) == 0) return 0;
    ULARGE_INTEGER ticks;
    ticks.HighPart = info.ftLastWriteTime.dwHighDateTime;
    ticks.LowPart = info.ftLastWriteTime.dwLowDateTime;
    constexpr std::uint64_t kTicksPerSecond = 10000000ULL;
    constexpr std::uint64_t kWindowsToUnixSeconds = 11644473600ULL;
    const std::uint64_t seconds = ticks.QuadPart / kTicksPerSecond;
    if (seconds <= kWindowsToUnixSeconds) return 0;
    return static_cast<std::time_t>(seconds - kWindowsToUnixSeconds);
#else
    struct stat info;
    if (!queryStat(path, &info)) return 0;
    return info.st_mtime;
#endif
}

void setLastWriteTime(const std::string& path, std::time_t time) {
    if (time <= 0) return;
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return;
    HANDLE handle = CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    constexpr std::uint64_t kTicksPerSecond = 10000000ULL;
    constexpr std::uint64_t kWindowsToUnixSeconds = 11644473600ULL;
    ULARGE_INTEGER ticks;
    ticks.QuadPart = (static_cast<std::uint64_t>(time) + kWindowsToUnixSeconds) * kTicksPerSecond;
    FILETIME fileTime;
    fileTime.dwLowDateTime = ticks.LowPart;
    fileTime.dwHighDateTime = ticks.HighPart;
    SetFileTime(handle, nullptr, nullptr, &fileTime);
    CloseHandle(handle);
#else
    struct utimbuf times;
    times.actime = time;
    times.modtime = time;
    ::utime(path.c_str(), &times);
#endif
}

std::string pathFileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string pathParent(const std::string& path) {
    std::string trimmed = path;
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    const std::size_t slash = trimmed.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    if (slash == 2 && trimmed[1] == ':') return trimmed.substr(0, 3);
    return trimmed.substr(0, slash);
}

std::string pathJoin(const std::string& directory, const std::string& relative) {
    if (directory.empty() || directory == ".") return relative;
    if (relative.empty()) return directory;
    std::string result = directory;
    const char last = result.back();
    if (last != '/' && last != '\\') result += '/';
    result += relative;
    return result;
}

std::string toArchivePath(const std::string& path) {
    std::string normalized = replaceAll(path, '\\', '/');
    normalized = stripLeadingSlashes(normalized);

    if (normalized.size() >= 2 && normalized[1] == ':') {
        normalized = stripLeadingSlashes(normalized.substr(2));
    }

    std::string result;
    result.reserve(normalized.size());
    std::size_t i = 0;
    while (i < normalized.size()) {
        const std::size_t next = normalized.find('/', i);
        const std::string segment = normalized.substr(
            i, next == std::string::npos ? std::string::npos : next - i);
        if (segment.empty() || segment == ".") {

        } else if (segment == ".." && !result.empty()) {
            const std::size_t slash = result.rfind('/');
            result.erase(slash == std::string::npos ? 0 : slash + 1);
        } else {
            if (!result.empty()) result.push_back('/');
            result += segment;
        }
        if (next == std::string::npos) break;
        i = next + 1;
    }
    return result;
}

bool isSafeArchivePath(const std::string& archivePath) {
    if (archivePath.empty()) return false;
    if (archivePath.front() == '/') return false;
    if (archivePath.find('\\') != std::string::npos) return false;
    if (archivePath.size() >= 2 && archivePath[1] == ':') return false;

    std::size_t i = 0;
    while (i <= archivePath.size()) {
        const std::size_t next = archivePath.find('/', i);
        const std::string segment =
            archivePath.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (segment == "..") return false;
        if (next == std::string::npos) break;
        i = next + 1;
    }
    return true;
}

}
}
