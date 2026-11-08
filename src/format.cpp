#include "format.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>

#include "bytes.h"
#include "crc32.h"
#include "display.h"

namespace lzpack {
namespace {

void encodeIndex(const std::vector<ArchiveEntry>& entries, std::string& buffer) {
    ByteWriter writer(buffer);
    writer.u64(entries.size());
    for (const auto& entry : entries) {
        writer.string(entry.path);
        writer.u64(entry.originalSize);
        writer.u64(entry.compressedSize);
        writer.u64(entry.blockOffset);
        writer.u64(entry.blockCount);
        writer.u32(entry.crc);
        writer.u32(entry.mode);
        writer.u64(static_cast<std::uint64_t>(entry.modifiedTime));
        writer.u8(static_cast<std::uint8_t>(entry.level));
        writer.u8(static_cast<std::uint8_t>(entry.filter.kind));
        writer.u8(entry.filter.delta == 0 ? 1 : entry.filter.delta);
        writer.u8(0);
    }
}

std::uint32_t decodeIndex(const std::uint8_t* data, std::size_t size,
                          std::vector<ArchiveEntry>& entries, const ArchiveLimits& limits) {
    ByteReader reader(data, size);
    const std::uint64_t count = reader.u64();
    if (reader.failed() || count > limits.maxEntryCount) {
        fail(ErrorKind::Corrupt,
             fmt("invalid entry count: {} (limit {})", count, limits.maxEntryCount));
    }
    entries.clear();
    entries.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        ArchiveEntry entry;
        entry.path = reader.string();
        entry.originalSize = reader.u64();
        entry.compressedSize = reader.u64();
        entry.blockOffset = reader.u64();
        entry.blockCount = reader.u64();
        entry.crc = reader.u32();
        entry.mode = reader.u32();
        entry.modifiedTime = static_cast<std::int64_t>(reader.u64());
        const std::uint8_t level = reader.u8();
        const std::uint8_t filterKind = reader.u8();
        const std::uint8_t delta = reader.u8();
        reader.u8();
        if (reader.failed()) {
            fail(ErrorKind::Corrupt, fmt("corrupt index at entry {}", i));
        }
        if (entry.path.size() > limits.maxPathLength) {
            fail(ErrorKind::Corrupt, "index path exceeds size limit");
        }
        if (level > static_cast<std::uint8_t>(CompressionLevel::Ultra) ||
            filterKind > static_cast<std::uint8_t>(FilterKind::Delta)) {
            fail(ErrorKind::Corrupt, fmt("invalid codec settings for entry {}", entry.path));
        }
        entry.level = static_cast<CompressionLevel>(level);
        entry.filter.kind = static_cast<FilterKind>(filterKind);
        entry.filter.delta = delta == 0 ? 1 : delta;
        entries.push_back(std::move(entry));
    }
    if (reader.remaining() != 0) {
        fail(ErrorKind::Corrupt, fmt("{} unparsed bytes at end of index", reader.remaining()));
    }
    return Crc32::compute(data, size);
}

std::string encodeArchiveHeader(const ArchiveHeader& header) {
    std::string buffer;
    ByteWriter writer(buffer);
    writer.u32(kArchiveMagic);
    writer.u16(header.version);
    writer.u16(header.flags);
    writer.u32(header.blockSize);
    writer.u8(static_cast<std::uint8_t>(header.level));
    writer.u8(0);
    writer.u16(0);
    writer.u32(0);
    writer.u32(0);
    writer.u32(0);
    return buffer;
}

std::string encodeArchiveFooter(const ArchiveFooter& footer) {
    std::string buffer;
    ByteWriter writer(buffer);
    writer.u32(kArchiveMagic);
    writer.u64(footer.indexOffset);
    writer.u64(footer.indexSize);
    writer.u64(footer.entryCount);
    writer.u32(footer.indexCrc);
    writer.u16(footer.version);
    writer.u16(footer.flags);
    return buffer;
}

std::uint64_t locateFooter(fio::Reader& input) {
    const std::uint64_t size = input.size();
    if (size < kArchiveFooterSize) {
        fail(ErrorKind::Corrupt,
             fmt("{} is not an LZP archive (only {} bytes)", input.path(), size));
    }
    std::uint8_t buffer[512];
    const std::uint64_t window = (std::min)(size, static_cast<std::uint64_t>(sizeof(buffer)));
    const std::uint64_t start = size - window;
    input.seek(start);
    if (input.read(buffer, static_cast<std::size_t>(window)) != window) {
        fail(ErrorKind::Corrupt, "failed to read archive footer");
    }

    for (std::size_t index = static_cast<std::size_t>(window - kArchiveFooterSize);;
         --index) {
        std::uint32_t magic = 0;
        std::memcpy(&magic, buffer + index, sizeof(magic));
        if (magic == kArchiveMagic) return start + index;
        if (index == 0) break;
    }
    fail(ErrorKind::Corrupt,
         fmt("{} has no LZP footer (not an archive or file is truncated)", input.path()));
}

void patchIndexCrc(const std::string& path, std::uint32_t crc) {
    std::uint8_t buffer[4];
    std::memcpy(buffer, &crc, sizeof(buffer));
    fio::FilePatcher patcher(path, kHeaderIndexCrcOffset);
    patcher.write(buffer, sizeof(buffer));
}

void collectPaths(const std::string& path, const CompressionOptions& options,
                  std::vector<std::string>& out) {
    if (fio::isSymlink(path)) {
        return;
    }
    if (fio::isDirectory(path)) {
        const std::vector<std::string> children = fio::listDirectory(path);
        for (const auto& child : children) {
            collectPaths(fio::pathJoin(path, child), options, out);
        }
        if (children.empty() && options.keepEmptyDirectories) {
            out.push_back(path);
        }
        return;
    }
    if (fio::isRegularFile(path)) {
        out.push_back(path);
    }

}

struct InputTask {
    std::string path;
    std::string archivePath;
    std::uint64_t size = 0;
    std::int64_t modifiedTime = 0;
    bool isDirectory = false;
};

std::vector<InputTask> buildInputTasks(const std::vector<std::string>& sources,
                                       const CompressionOptions& options) {
    std::vector<std::string> paths;
    for (const auto& source : sources) {
        if (!fio::pathExists(source)) {
            fail(ErrorKind::NotFound, fmt("input path not found: {}", source));
        }
        const std::size_t before = paths.size();
        collectPaths(source, options, paths);
        if (paths.size() == before) {
            display::warn(fmt("no regular files to compress in {}", source));
        }
    }

    std::vector<InputTask> tasks;
    std::set<std::string> seen;
    for (const auto& path : paths) {
        std::string archivePath = fio::toArchivePath(path);
        if (archivePath.empty() || !fio::isSafeArchivePath(archivePath)) {

            archivePath = fio::pathFileName(path);
        }
        if (!seen.insert(archivePath).second) continue;

        InputTask task;
        task.path = path;
        task.archivePath = archivePath;
        task.isDirectory = fio::isDirectory(path);
        if (!task.isDirectory) {
            task.size = fio::fileSize(path);
            task.modifiedTime = static_cast<std::int64_t>(fio::lastWriteTime(path));
        }
        tasks.push_back(std::move(task));
    }

    std::stable_sort(tasks.begin(), tasks.end(), [](const InputTask& a, const InputTask& b) {
        if (a.isDirectory != b.isDirectory) return !a.isDirectory;
        return a.archivePath < b.archivePath;
    });
    return tasks;
}

bool looksLikeExecutable(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kExtensions[] = {".exe", ".dll", ".sys", ".ocx", ".obj", ".lib", ".pdb"};
    for (const char* extension : kExtensions) {
        const std::size_t length = std::strlen(extension);
        if (lower.size() > length && lower.compare(lower.size() - length, length, extension) == 0) {
            return true;
        }
    }
    return false;
}

bool detectTextFile(const std::string& path) {
    fio::FileReader reader(path);
    std::vector<std::uint8_t> sample(64 * 1024);
    const std::size_t got = reader.read(sample.data(), sample.size());
    return got > 0 && looksLikeText(sample.data(), got);
}

CodecParams effectiveParams(const CodecParams& base, const std::string& path, std::uint64_t size,
                            const CompressionOptions& options) {
    CodecParams params = base;
    if (options.autoFilter && params.filter.isNone() && looksLikeExecutable(path) &&
        size >= kFilterMinSize) {
        params.filter.kind = FilterKind::X86Bcj;
    }
    if (options.autoText) {
        params.textMode = detectTextFile(path);
    }
    return params;
}

void compressBlockChecked(const CodecParams& params, const std::uint8_t* data, std::size_t size,
                          const std::uint8_t* nextData, std::size_t nextSize,
                          bool textNormalized, std::string& output,
                          const ProgressCallback& progress, CodecStats* stats) {
    output.clear();
    compressBlock(params, data, size, nextData, nextSize, textNormalized, output, progress, stats);
    bool valid = false;
    try {
        if (output.size() >= kBlockHeaderSize) {
            std::uint64_t originalSize = 0, compressedSize = 0;
            std::uint32_t crc = 0;
            const BlockInfo info = readBlockHeader(
                reinterpret_cast<const std::uint8_t*>(output.data()), kBlockHeaderSize,
                &originalSize, &compressedSize, &crc);
            if (originalSize == size && compressedSize == output.size() - kBlockHeaderSize) {
                std::vector<std::uint8_t> restored;
                decompressBlock(info,
                    reinterpret_cast<const std::uint8_t*>(output.data()) + kBlockHeaderSize,
                    static_cast<std::size_t>(compressedSize), size, crc, restored,
                    ProgressCallback(), nullptr);
                valid = restored.size() == size &&
                        (size == 0 || std::memcmp(restored.data(), data, size) == 0);
            }
        }
    } catch (const CompressError&) {
        valid = false;
    }
    if (!valid) {
        CodecParams safe = params;
        safe.level = CompressionLevel::Store;
        output.clear();
        compressBlock(safe, data, size, nullptr, 0, textNormalized, output, progress, stats);
    }
}

class ArchiveBuilder {
public:
    ArchiveBuilder(const std::string& path, const CompressionOptions& options)
        : output_(openOutput(path)), path_(path) {
        ArchiveHeader header;
        header.version = kArchiveVersion;
        header.flags = options.codec.textMode ? kFlagTextMode : 0;
        header.blockSize = options.codec.blockSize();
        header.level = options.codec.level;
        headerBytes_ = encodeArchiveHeader(header);
        output_.write(headerBytes_.data(), headerBytes_.size());
    }

    void write(const void* data, std::size_t size) { output_.write(data, size); }
    void write(const std::string& data) { output_.write(data.data(), data.size()); }

    std::uint64_t position() const noexcept { return output_.bytesWritten(); }

    ArchiveFooter finish(const std::vector<ArchiveEntry>& entries) {
        std::string index;
        encodeIndex(entries, index);
        const std::uint64_t indexOffset = position();
        write(index);

        ArchiveFooter footer;
        footer.indexOffset = indexOffset;
        footer.indexSize = index.size();
        footer.entryCount = entries.size();
        footer.indexCrc = Crc32::compute(index.data(), index.size());
        write(encodeArchiveFooter(footer));

        output_.flush();
        patchIndexCrc(path_, footer.indexCrc);
        return footer;
    }

private:

    static fio::FileWriter openOutput(const std::string& path) {
        fio::ensureParentDirectory(path);
        return fio::FileWriter(path);
    }

    fio::FileWriter output_;
    std::string path_;
    std::string headerBytes_;
};

void compressBlocksParallel(const CodecParams& params, fio::FileReader& reader,
                            ArchiveBuilder& builder, std::size_t blockSize, unsigned threads,
                            const ProgressHandler& progress, ArchiveStats* stats,
                            const std::string& displayPath, std::uint64_t totalBytes,
                            std::uint64_t& doneBytes);

ArchiveEntry compressOneFile(ArchiveBuilder& builder, const InputTask& task,
                             const CompressionOptions& options, const ProgressHandler& progress,
                             ArchiveStats* stats, std::uint64_t totalBytes,
                             std::uint64_t& doneBytes) {
    const CodecParams params = effectiveParams(options.codec, task.path, task.size, options);

    ArchiveEntry entry;
    entry.path = task.archivePath;
    entry.modifiedTime = task.modifiedTime;
    entry.level = params.level;
    entry.filter = params.filter;
    entry.blockOffset = builder.position();
    if (params.textMode) entry.mode |= kEntryText;

    const std::size_t blockSize = params.blockSize();

    if (options.threads > 1 && !task.isDirectory && task.size > blockSize &&
        params.filter.isNone() && !params.textMode) {
        fio::FileReader reader(task.path);
        compressBlocksParallel(params, reader, builder, blockSize, options.threads, progress, stats,
                               task.path, totalBytes, doneBytes);
        entry.originalSize = task.size;
        entry.crc = Crc32::computeFile(task.path);
        entry.blockCount = (task.size + blockSize - 1) / blockSize;
        if (task.size == 0) entry.mode |= kEntryEmpty;
        entry.compressedSize = builder.position() - entry.blockOffset;
        return entry;
    }

    fio::FileReader reader(task.path);

    std::vector<std::uint8_t> buffer(blockSize + 1);
    std::vector<std::uint8_t> work;
    std::string block;

    Crc32 fileCrc;
    std::uint64_t originalTotal = 0;
    std::uint64_t blocks = 0;

    for (;;) {
        const std::size_t got = reader.read(buffer.data(), buffer.size());
        if (got == 0) break;
        const bool hasLookahead = got > blockSize;
        const std::size_t blockBytes = hasLookahead ? blockSize : got;
        const std::uint8_t* nextData = hasLookahead ? buffer.data() + blockBytes : nullptr;
        const std::size_t nextSize = hasLookahead ? got - blockBytes : 0;

        work.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(blockBytes));
        if (!params.filter.isNone()) {
            filterEncode(params.filter, work.data(), work.size());
        }
        const bool textNormalized = params.textMode && normalizeCrLf(work);

        block.clear();
        const ProgressCallback blockProgress =
            progress ? ProgressCallback([&](std::uint64_t done, std::uint64_t) {
                ProgressInfo info;
                info.phase = ProgressInfo::Phase::Compress;
                info.currentPath = task.path;
                info.totalBytes = totalBytes;
                info.doneBytes = doneBytes + done;
                progress(info);
            })
                     : ProgressCallback();

        compressBlockChecked(params, work.data(), work.size(), nextData, nextSize, textNormalized, block,
                      blockProgress, stats != nullptr ? &stats->codec : nullptr);
        fileCrc.update(work.data(), work.size());
        builder.write(block);

        originalTotal += work.size();
        ++blocks;
        if (stats != nullptr) {
            stats->blockCount++;
            stats->codecInputBytes += work.size();
            stats->codecOutputBytes += block.size() - kBlockHeaderSize;
        }
        doneBytes += work.size();

        if (progress) {
            ProgressInfo info;
            info.phase = ProgressInfo::Phase::Compress;
            info.currentPath = task.path;
            info.totalBytes = totalBytes;
            info.doneBytes = doneBytes;
            progress(info);
        }
        if (blockBytes < blockSize) break;
    }

    entry.originalSize = originalTotal;
    entry.crc = fileCrc.value();
    entry.blockCount = blocks;
    if (originalTotal == 0) entry.mode |= kEntryEmpty;
    entry.compressedSize = builder.position() - entry.blockOffset;
    return entry;
}

void compressBlocksParallel(const CodecParams& params, fio::FileReader& reader,
                            ArchiveBuilder& builder, std::size_t blockSize, unsigned threads,
                            const ProgressHandler& progress, ArchiveStats* stats,
                            const std::string& displayPath, std::uint64_t totalBytes,
                            std::uint64_t& doneBytes) {
    const std::size_t workerCount = (std::max)(1u, threads);

    struct Slot {
        std::string payload;
        std::uint64_t doneBytes = 0;
        bool ready = false;
    };

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<Slot> slots;
    std::size_t windowSize = 0;
    std::size_t nextBlock = 0;
    std::size_t consumed = 0;
    bool exhausted = false;
    std::exception_ptr workerFailure;
    CodecStats localStats;
    std::mutex statsMutex;

    auto work = [&]() {
        try {

            fio::FileReader workerReader(reader.path());
            std::vector<std::uint8_t> buffer(blockSize + 1);
            for (;;) {
                std::size_t index = 0;
                std::size_t offset = 0;
                std::size_t count = 0;
                bool hasLookahead = false;
                {
                    std::unique_lock<std::mutex> lock(mutex);

                    ready.wait(lock, [&] {
                        return exhausted || (windowSize > 0 && nextBlock < consumed + windowSize);
                    });
                    if (exhausted) return;
                    index = nextBlock % windowSize;
                    offset = nextBlock * blockSize;
                    ++nextBlock;
                }
                if (offset >= workerReader.size()) {
                    hasLookahead = false;
                    count = 0;
                } else {
                    workerReader.seek(offset);
                    const std::size_t got = workerReader.read(buffer.data(), buffer.size());
                    hasLookahead = got > blockSize;
                    count = hasLookahead ? blockSize : got;
                }
                Slot& slot = slots[index];
                slot.payload.clear();
                slot.doneBytes = 0;
                CodecStats blockStats;
                if (count > 0) {
                    compressBlockChecked(params, buffer.data(), count,
                                  hasLookahead ? buffer.data() + blockSize : nullptr,
                                  hasLookahead ? 1 : 0, false, slot.payload,
                                  ProgressCallback(), &blockStats);
                    slot.doneBytes = count;
                }
                slot.ready = true;
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    localStats.literalCount += blockStats.literalCount;
                    localStats.literalBytes += blockStats.literalBytes;
                    localStats.matchCount += blockStats.matchCount;
                    localStats.repMatchCount += blockStats.repMatchCount;
                    localStats.matchBytes += blockStats.matchBytes;
                }
                ready.notify_all();
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!workerFailure) workerFailure = std::current_exception();
            exhausted = true;
            ready.notify_all();
        }
    };

    const std::size_t totalBlocks =
        static_cast<std::size_t>((reader.size() + blockSize - 1) / blockSize);
    if (totalBlocks <= 1 || reader.size() <= blockSize) {

        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(reader.size()));
        reader.seek(0);
        const std::size_t got = reader.read(buffer.data(), buffer.size());
        std::string payload;
        compressBlockChecked(params, buffer.data(), got, nullptr, 0, false, payload, ProgressCallback(),
                      stats != nullptr ? &stats->codec : nullptr);
        builder.write(payload);
        doneBytes += got;
        if (stats != nullptr) {
            stats->blockCount = 1;
            stats->codecInputBytes += got;
            stats->codecOutputBytes += payload.size() - kBlockHeaderSize;
        }
        if (progress) {
            ProgressInfo info;
            info.phase = ProgressInfo::Phase::Compress;
            info.currentPath = displayPath;
            info.totalBytes = totalBytes;
            info.doneBytes = doneBytes;
            progress(info);
        }
        return;
    }

    windowSize = workerCount * 2;
    slots.resize(windowSize);

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) workers.emplace_back(work);

    std::size_t writtenBlocks = 0;
    while (true) {
        Slot& slot = slots[writtenBlocks % windowSize];
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return slot.ready || workerFailure; });
            if (workerFailure && !slot.ready) {
                lock.unlock();
                break;
            }
        }
        if (!slot.payload.empty()) builder.write(slot.payload);
        if (slot.doneBytes > 0) {
            doneBytes += slot.doneBytes;
            if (stats != nullptr) {
                stats->blockCount++;
                stats->codecInputBytes += slot.doneBytes;
                stats->codecOutputBytes += slot.payload.size() - kBlockHeaderSize;
            }
            if (progress) {
                ProgressInfo info;
                info.phase = ProgressInfo::Phase::Compress;
                info.currentPath = displayPath;
                info.totalBytes = totalBytes;
                info.doneBytes = doneBytes;
                progress(info);
            }
        }
        ++writtenBlocks;
        const bool more = writtenBlocks < totalBlocks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            slot.ready = false;

            consumed = writtenBlocks;
            if (!more) {
                exhausted = true;
            }
        }
        ready.notify_all();
        if (!more) break;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        exhausted = true;
    }
    ready.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    if (workerFailure) {
        std::rethrow_exception(workerFailure);
    }
    if (stats != nullptr) {
        stats->codec.literalCount += localStats.literalCount;
        stats->codec.literalBytes += localStats.literalBytes;
        stats->codec.matchCount += localStats.matchCount;
        stats->codec.repMatchCount += localStats.repMatchCount;
        stats->codec.matchBytes += localStats.matchBytes;
    }
}

class BlockStream {
public:
    BlockStream(fio::Reader& input, const ArchiveEntry& entry) : input_(input), entry_(entry) {        input_.seek(entry.blockOffset);
    }

    bool next(BlockRecord& record) {
        if (blocksRead_ >= entry_.blockCount) return false;
        const std::uint64_t headerOffset = input_.tell();
        if (headerOffset + kBlockHeaderSize > input_.size()) {
            fail(ErrorKind::Corrupt, fmt("{}: truncated data section (incomplete block header)", entry_.path));
        }
        std::uint8_t headerBytes[kBlockHeaderSize];
        if (input_.read(headerBytes, kBlockHeaderSize) != kBlockHeaderSize) {
            fail(ErrorKind::Corrupt, fmt("{}: failed to read block header", entry_.path));
        }
        std::uint64_t originalSize = 0;
        std::uint64_t compressedSize = 0;
        std::uint32_t crc = 0;
        const BlockInfo info =
            readBlockHeader(headerBytes, kBlockHeaderSize, &originalSize, &compressedSize, &crc);
        if (compressedSize > input_.size() - (headerOffset + kBlockHeaderSize)) {
            fail(ErrorKind::Corrupt,
                 fmt("{}: compressed size {} exceeds archive bounds", entry_.path, compressedSize));
        }
        record.header = blockHeaderOf(info, originalSize, compressedSize, crc);
        record.payload.resize(static_cast<std::size_t>(compressedSize));
        if (compressedSize > 0 &&
            input_.read(&record.payload[0], record.payload.size()) != compressedSize) {
            fail(ErrorKind::Corrupt, fmt("{}: failed to read compressed data", entry_.path));
        }
        ++blocksRead_;
        return true;
    }

private:
    fio::Reader& input_;
    const ArchiveEntry& entry_;
    std::uint64_t blocksRead_ = 0;
};

void decodeBlock(const BlockRecord& record, bool textMode, bool verifyCrc,
                 std::vector<std::uint8_t>& out) {
    BlockInfo info = blockInfoOf(record.header);
    info.textMode = textMode;
    decompressBlock(info, reinterpret_cast<const std::uint8_t*>(record.payload.data()),
                    record.payload.size(), static_cast<std::size_t>(record.header.originalSize),
                    verifyCrc ? record.header.crc : 0u, out, ProgressCallback(), nullptr);
}

std::string resolveDestination(const std::string& destination, const std::string& entryPath,
                               bool flatten) {
    const std::string relative = flatten ? fio::pathFileName(entryPath) : entryPath;
    if (relative.empty() || !fio::isSafeArchivePath(relative)) {
        fail(ErrorKind::Corrupt, fmt("unsafe archive path; refusing to extract: {}", entryPath));
    }
    return fio::pathJoin(destination, relative);
}

struct ExtractedFile {
    std::uint64_t bytes = 0;
    std::uint32_t crc = 0;
};

ExtractedFile extractEntry(fio::Reader& input, const ArchiveEntry& entry, const std::string& target,
                           bool verifyCrc, const ProgressHandler& progress,
                           std::uint64_t totalBytes, std::uint64_t doneBytes) {
    fio::FileWriter writer(target);
    BlockStream stream(input, entry);
    BlockRecord record;
    std::vector<std::uint8_t> block;
    Crc32 crc;
    ExtractedFile result;
    const bool textMode = (entry.mode & kEntryText) != 0;

    while (stream.next(record)) {
        decodeBlock(record, textMode, verifyCrc, block);
        crc.update(block.data(), block.size());
        writer.write(block.data(), block.size());
        result.bytes += block.size();
        if (progress) {
            ProgressInfo info;
            info.phase = ProgressInfo::Phase::Extract;
            info.currentPath = entry.path;
            info.totalBytes = totalBytes;
            info.doneBytes = doneBytes + result.bytes;
            progress(info);
        }
    }
    writer.flush();
    result.crc = crc.value();

    if (result.bytes != entry.originalSize) {
        fail(ErrorKind::Corrupt, fmt("decoded {} bytes, index declares {}", result.bytes,
                                               entry.originalSize));
    }
    return result;
}

}

void createArchive(const std::vector<std::string>& sources, const std::string& outputPath,
                   const CompressionOptions& options, const ProgressHandler& progress,
                   ArchiveStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    if (stats != nullptr) *stats = ArchiveStats{};

    const std::vector<InputTask> tasks = buildInputTasks(sources, options);
    std::uint64_t totalBytes = 0;
    for (const auto& task : tasks) totalBytes += task.isDirectory ? 0 : task.size;
    stats->inputBytes = totalBytes;

    ArchiveBuilder builder(outputPath, options);
    std::vector<ArchiveEntry> entries;
    entries.reserve(tasks.size());
    std::uint64_t doneBytes = 0;

    for (const auto& task : tasks) {
        if (task.isDirectory) {
            ArchiveEntry entry;
            entry.path = task.archivePath;
            entry.mode = kEntryDirectory;
            entry.blockOffset = builder.position();
            entries.push_back(std::move(entry));
            continue;
        }
        entries.push_back(
            compressOneFile(builder, task, options, progress, stats, totalBytes, doneBytes));
    }

    const ArchiveFooter footer = builder.finish(entries);
    if (stats != nullptr) {
        stats->entryCount = entries.size();
        stats->outputBytes = builder.position();
        stats->indexBytes = footer.indexSize + kArchiveFooterSize;
        stats->payloadBytes = stats->outputBytes - stats->indexBytes - kArchiveHeaderSize;
        stats->seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
}

void compressStream(fio::Reader& input, fio::Writer& output, const std::string& entryPath,
                    std::uint64_t inputSize, std::int64_t modifiedTime,
                    const CompressionOptions& options, const ProgressHandler& progress,
                    ArchiveStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    if (stats != nullptr) *stats = ArchiveStats{};
    stats->inputBytes = inputSize;

    ArchiveHeader header;
    header.version = kArchiveVersion;
    header.flags = options.codec.textMode ? kFlagTextMode : 0;
    header.blockSize = options.codec.blockSize();
    header.level = options.codec.level;
    const std::string headerBytes = encodeArchiveHeader(header);
    output.write(headerBytes.data(), headerBytes.size());
    std::uint64_t position = kArchiveHeaderSize;

    ArchiveEntry entry;
    entry.path = entryPath;
    entry.modifiedTime = modifiedTime;
    entry.level = options.codec.level;
    entry.filter = options.codec.filter;
    entry.blockOffset = position;
    if (options.codec.textMode) entry.mode |= kEntryText;

    const std::size_t blockSize = options.codec.blockSize();
    std::vector<std::uint8_t> buffer(blockSize + 1);
    std::vector<std::uint8_t> work;
    std::string block;
    Crc32 fileCrc;
    std::uint64_t originalTotal = 0;
    std::uint64_t blocks = 0;
    std::uint64_t doneBytes = 0;

    for (;;) {
        const std::size_t got = input.read(buffer.data(), buffer.size());
        if (got == 0) break;
        const bool hasLookahead = got > blockSize;
        const std::size_t blockBytes = hasLookahead ? blockSize : got;
        const std::uint8_t* nextData = hasLookahead ? buffer.data() + blockBytes : nullptr;
        const std::size_t nextSize = hasLookahead ? got - blockBytes : 0;

        work.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(blockBytes));
        if (!options.codec.filter.isNone()) {
            filterEncode(options.codec.filter, work.data(), work.size());
        }
        const bool textNormalized = options.codec.textMode && normalizeCrLf(work);

        block.clear();
        const ProgressCallback blockProgress =
            progress ? ProgressCallback([&](std::uint64_t done, std::uint64_t) {
                ProgressInfo info;
                info.phase = ProgressInfo::Phase::Compress;
                info.currentPath = entryPath;
                info.totalBytes = inputSize;
                info.doneBytes = doneBytes + done;
                progress(info);
            })
                     : ProgressCallback();
        compressBlockChecked(options.codec, work.data(), work.size(), nextData, nextSize, textNormalized,
                      block, blockProgress, stats != nullptr ? &stats->codec : nullptr);
        fileCrc.update(work.data(), work.size());
        output.write(block.data(), block.size());
        position += block.size();

        originalTotal += work.size();
        ++blocks;
        doneBytes += work.size();
        if (stats != nullptr) {
            stats->blockCount++;
            stats->codecInputBytes += work.size();
            stats->codecOutputBytes += block.size() - kBlockHeaderSize;
        }
        if (progress) {
            ProgressInfo info;
            info.phase = ProgressInfo::Phase::Compress;
            info.currentPath = entryPath;
            info.totalBytes = inputSize;
            info.doneBytes = doneBytes;
            progress(info);
        }
        if (blockBytes < blockSize) break;
    }

    entry.originalSize = originalTotal;
    entry.crc = fileCrc.value();
    entry.blockCount = blocks;
    if (originalTotal == 0) entry.mode |= kEntryEmpty;
    entry.compressedSize = position - entry.blockOffset;

    const std::vector<ArchiveEntry> entries{entry};
    std::string index;
    encodeIndex(entries, index);

    ArchiveFooter footer;
    footer.indexOffset = position;
    footer.indexSize = index.size();
    footer.entryCount = 1;
    footer.indexCrc = Crc32::compute(index.data(), index.size());
    output.write(index.data(), index.size());
    const std::string footerBytes = encodeArchiveFooter(footer);
    output.write(footerBytes.data(), footerBytes.size());
    output.flush();

    if (stats != nullptr) {
        stats->entryCount = 1;
        stats->indexBytes = index.size() + footerBytes.size();
        stats->payloadBytes = position - kArchiveHeaderSize;
        stats->outputBytes = position + stats->indexBytes;
        stats->seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
}

ArchiveFooter readIndex(fio::Reader& input, std::vector<ArchiveEntry>& entries,
                        ArchiveHeader* header, const ArchiveLimits& limits) {
    const std::uint64_t footerOffset = locateFooter(input);
    input.seek(footerOffset);
    std::uint8_t footerBytes[kArchiveFooterSize];
    if (input.read(footerBytes, sizeof(footerBytes)) != sizeof(footerBytes)) {
        fail(ErrorKind::Corrupt, "failed to read footer");
    }
    ByteReader reader(footerBytes, sizeof(footerBytes));
    if (reader.u32() != kArchiveMagic) {
        fail(ErrorKind::Corrupt, "invalid footer magic");
    }
    ArchiveFooter footer;
    footer.indexOffset = reader.u64();
    footer.indexSize = reader.u64();
    footer.entryCount = reader.u64();
    footer.indexCrc = reader.u32();
    footer.version = reader.u16();
    footer.flags = reader.u16();
    if (reader.failed()) {
        fail(ErrorKind::Corrupt, "failed to parse footer");
    }
    if (footer.version != kArchiveVersion) {
        fail(ErrorKind::Unsupported,
             fmt("unsupported archive version {} (supported: {})", footer.version,
                           kArchiveVersion));
    }
    if (footer.indexOffset + footer.indexSize > footerOffset) {
        fail(ErrorKind::Corrupt, "index offset out of bounds");
    }
    if (footer.indexSize > (64ull << 20)) {
        fail(ErrorKind::Corrupt, fmt("index too large: {} bytes", footer.indexSize));
    }

    std::vector<std::uint8_t> index(static_cast<std::size_t>(footer.indexSize));
    input.seek(footer.indexOffset);
    if (!index.empty() && input.read(index.data(), index.size()) != index.size()) {
        fail(ErrorKind::Corrupt, "failed to read index");
    }
    const std::uint32_t actualCrc = decodeIndex(index.data(), index.size(), entries, limits);
    if (actualCrc != footer.indexCrc) {
        fail(ErrorKind::Corrupt,
             fmt("index CRC mismatch (expected {:#010x}, got {:#010x})", footer.indexCrc,
                           actualCrc));
    }

    if (header != nullptr) {
        std::uint8_t bytes[kArchiveHeaderSize];
        input.seek(0);
        if (input.read(bytes, sizeof(bytes)) != sizeof(bytes)) {
            fail(ErrorKind::Corrupt, "failed to read archive header");
        }
        ByteReader headerReader(bytes, sizeof(bytes));
        if (headerReader.u32() != kArchiveMagic) {
            fail(ErrorKind::Corrupt, "invalid archive header magic");
        }
        header->version = headerReader.u16();
        header->flags = headerReader.u16();
        header->blockSize = headerReader.u32();
        const std::uint8_t level = headerReader.u8();
        if (headerReader.failed()) {
            fail(ErrorKind::Corrupt, "failed to parse archive header");
        }
        if (level > static_cast<std::uint8_t>(CompressionLevel::Ultra)) {
            fail(ErrorKind::Corrupt, "invalid compression level in archive header");
        }
        header->level = static_cast<CompressionLevel>(level);
        if (header->version != kArchiveVersion) {
            fail(ErrorKind::Unsupported, fmt("unsupported archive version {}", header->version));
        }
        if ((header->flags & ~kArchiveKnownFlags) != 0) {
            fail(ErrorKind::Unsupported,
                 fmt("archive uses unknown feature flags {:#06x}", header->flags));
        }
    }
    return footer;
}

void extractArchive(fio::Reader& input, const std::string& archivePath,
                    const std::vector<ArchiveEntry>& entries, const ExtractionOptions& options,
                    const ProgressHandler& progress, ExtractionStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    if (stats != nullptr) *stats = ExtractionStats{};
    (void)archivePath;

    std::uint64_t totalBytes = 0;
    for (const auto& entry : entries) {
        if ((entry.mode & kEntryDirectory) != 0) continue;
        if (entrySelected(entry.path, options.includes, options.excludes)) {
            totalBytes += entry.originalSize;
        }
    }
    std::uint64_t doneBytes = 0;

    for (const auto& entry : entries) {
        if (!entrySelected(entry.path, options.includes, options.excludes)) {
            if (stats != nullptr) stats->entriesSkipped++;
            continue;
        }

        std::string target;
        try {
            target = resolveDestination(options.destination, entry.path, options.flatten);

            if ((entry.mode & kEntryDirectory) != 0) {
                if (!options.dryRun) fio::createDirectories(target);
                if (stats != nullptr) stats->entriesExtracted++;
                continue;
            }
            if (options.dryRun) {
                display::detail(fmt("extracting {} ({} bytes / {} blocks)", target, entry.originalSize,
                                              entry.blockCount));
                if (stats != nullptr) stats->entriesExtracted++;
                continue;
            }

            fio::ensureParentDirectory(target);
            if (!options.overwrite && fio::pathExists(target)) {
                display::warn(fmt("already exists, skipping: {}", target));
                if (stats != nullptr) stats->entriesSkipped++;
                continue;
            }

            const ExtractedFile result =
                extractEntry(input, entry, target, options.verifyCrc, progress, totalBytes, doneBytes);
            if (options.verifyCrc && result.crc != entry.crc) {
                fail(ErrorKind::Corrupt,
                     fmt("content CRC mismatch (expected {:#010x}, got {:#010x})", entry.crc,
                                   result.crc));
            }

            doneBytes += result.bytes;
            if (stats != nullptr) {
                stats->entriesExtracted++;
                stats->bytesWritten += result.bytes;
            }
            if (options.restoreTimestamps && entry.modifiedTime > 0) {
                fio::setLastWriteTime(target, static_cast<std::time_t>(entry.modifiedTime));
            }
        } catch (const CompressError& error) {
            if (stats != nullptr) {
                stats->failures.push_back(fmt("{}: {}", entry.path, error.what()));
            }

            try {
                if (!target.empty() && fio::isRegularFile(target)) fio::removeFile(target);
            } catch (...) {

            }
        }
    }

    if (stats != nullptr) {
        stats->seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
}

void verifyArchive(fio::Reader& input, const std::vector<ArchiveEntry>& entries,
                   const ProgressHandler& progress, ExtractionStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    if (stats != nullptr) *stats = ExtractionStats{};

    std::uint64_t totalBytes = 0;
    for (const auto& entry : entries) {
        if ((entry.mode & kEntryDirectory) == 0) totalBytes += entry.originalSize;
    }
    std::uint64_t doneBytes = 0;

    for (const auto& entry : entries) {
        if ((entry.mode & kEntryDirectory) != 0) continue;
        try {
            BlockStream stream(input, entry);
            BlockRecord record;
            std::vector<std::uint8_t> block;
            Crc32 crc;
            std::uint64_t produced = 0;
            const bool textMode = (entry.mode & kEntryText) != 0;
            while (stream.next(record)) {
                decodeBlock(record, textMode, true, block);
                crc.update(block.data(), block.size());
                produced += block.size();
            }
            if (produced != entry.originalSize) {
                fail(ErrorKind::Corrupt,
                     fmt("decoded {} bytes, index declares {}", produced, entry.originalSize));
            }
            if (crc.value() != entry.crc) {
                fail(ErrorKind::Corrupt,
                     fmt("content CRC mismatch (expected {:#010x}, got {:#010x})", entry.crc,
                                   crc.value()));
            }
            doneBytes += produced;
            if (stats != nullptr) stats->entriesExtracted++;
            if (progress) {
                ProgressInfo info;
                info.phase = ProgressInfo::Phase::Verify;
                info.currentPath = entry.path;
                info.totalBytes = totalBytes;
                info.doneBytes = doneBytes;
                progress(info);
            }
        } catch (const CompressError& error) {
            if (stats != nullptr) {
                stats->failures.push_back(fmt("{}: {}", entry.path, error.what()));
            }
        }
    }

    if (stats != nullptr) {
        stats->bytesWritten = doneBytes;
        stats->seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
}

namespace {

bool matchSegment(const std::string& pattern, const std::string& text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t starPattern = std::string::npos;
    std::size_t starText = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starPattern = p++;
            starText = t;
        } else if (starPattern != std::string::npos) {
            p = starPattern + 1;
            t = ++starText;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool matchFrom(const std::string& pattern, std::size_t p, const std::string& text, std::size_t t) {
    const std::size_t slash = pattern.find('/', p);
    const std::size_t segmentEnd = (slash == std::string::npos) ? pattern.size() : slash;
    const std::string segment = pattern.substr(p, segmentEnd - p);
    const bool lastPatternSegment = (slash == std::string::npos);

    if (segment == "**") {
        if (lastPatternSegment) return true;
        const std::size_t nextPattern = slash + 1;
        for (std::size_t cut = t; cut <= text.size(); ++cut) {
            if (cut != t && text[cut - 1] != '/') continue;
            if (matchFrom(pattern, nextPattern, text, cut)) return true;
            if (cut == text.size()) break;
        }
        return false;
    }

    const std::size_t textSlash = text.find('/', t);
    const std::size_t textEnd = (textSlash == std::string::npos) ? text.size() : textSlash;
    if (!matchSegment(segment, text.substr(t, textEnd - t))) return false;

    if (lastPatternSegment) return textSlash == std::string::npos;
    if (textSlash == std::string::npos) return false;
    return matchFrom(pattern, slash + 1, text, textSlash + 1);
}

}

bool matchPattern(const std::string& pattern, const std::string& path) {
    if (pattern.empty()) return false;
    return matchFrom(pattern, 0, path, 0);
}

bool entrySelected(const std::string& path, const std::vector<std::string>& includes,
                   const std::vector<std::string>& excludes) {
    bool selected = includes.empty();
    for (const auto& pattern : includes) {
        if (matchPattern(pattern, path)) {
            selected = true;
            break;
        }
    }
    if (!selected) return false;
    for (const auto& pattern : excludes) {
        if (matchPattern(pattern, path)) return false;
    }
    return true;
}

bool normalizeCrLf(std::vector<std::uint8_t>& data) {
    if (data.empty()) return false;
    std::size_t write = 0;
    bool changed = false;
    for (std::size_t read = 0; read < data.size(); ++read) {
        if (data[read] == '\r' && read + 1 < data.size() && data[read + 1] == '\n') {
            data[write++] = '\n';
            ++read;
            changed = true;
        } else {
            data[write++] = data[read];
        }
    }
    if (changed) data.resize(write);
    return changed;
}

void restoreCrLf(std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    std::size_t extra = 0;
    for (std::uint8_t byte : data) {
        if (byte == '\n') ++extra;
    }
    if (extra == 0) return;
    const std::size_t original = data.size();
    data.resize(original + extra);
    std::size_t write = data.size();
    for (std::size_t read = original; read-- > 0;) {
        const std::uint8_t byte = data[read];
        if (byte == '\n') data[--write] = '\r';
        data[--write] = byte;
    }
}

bool looksLikeText(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return false;
    std::size_t printable = 0;
    std::size_t nulBytes = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t byte = data[i];
        if (byte == 0) ++nulBytes;
        if (byte == '\t' || byte == '\n' || byte == '\r' || (byte >= 0x20 && byte <= 0x7E) ||
            byte >= 0x80) {
            ++printable;
        }
    }
    return nulBytes == 0 && printable * 100 >= size * 95;
}

std::uint64_t buildSelfExtractor(const std::string& stubPath, const std::string& archivePath,
                                 const std::string& outputPath) {
    if (!fio::isRegularFile(stubPath)) {
        fail(ErrorKind::NotFound, fmt("self-extracting stub not found: {}", stubPath));
    }
    const std::uint64_t stubSize = fio::copyFile(stubPath, outputPath);

    fio::FileReader archive(archivePath);
    fio::FilePatcher target(outputPath, stubSize);
    std::vector<std::uint8_t> buffer(256 * 1024);
    std::uint64_t appended = 0;
    for (;;) {
        const std::size_t got = archive.read(buffer.data(), buffer.size());
        if (got == 0) break;
        target.write(buffer.data(), got);
        appended += got;
    }
    return stubSize + appended;
}

}
