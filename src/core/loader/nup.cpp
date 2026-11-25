// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

#include <openssl/evp.h>
#include <zstd.h>

#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nup.h"

namespace Loader {
namespace {

constexpr size_t HeaderSize = 0x80;
constexpr size_t TocHeaderSize = 0x30;
constexpr size_t PackageSize = 0x40;
constexpr size_t DirectorySize = 0x18;
constexpr size_t FileSize = 0x50;
constexpr u32 NoDir = 0xffffffff;
constexpr size_t MaxTocSize = 128 * 1024 * 1024;
constexpr size_t MaxDirectoryDepth = 128;

using Bytes = std::span<const u8>;

template <typename T>
T LittleEndian(Bytes bytes, size_t offset) {
    T value{};
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<T>(bytes[offset + i]) << (8 * i));
    }
    return value;
}

bool Fits(u64 offset, u64 size, u64 limit) {
    return offset <= limit && size <= limit - offset;
}

bool AllZero(Bytes bytes) {
    return std::ranges::all_of(bytes, [](u8 byte) { return byte == 0; });
}

class NupMember final : public FileSys::VfsFile {
public:
    NupMember(FileSys::VirtualFile source_, std::string name_, size_t offset_, size_t stored_,
              size_t size_, bool compressed_ = false)
        : source(std::move(source_)), name(std::move(name_)), offset(offset_), stored(stored_),
          size(size_), compressed(compressed_) {}

    std::string GetName() const override { return name; }
    size_t GetSize() const override { return size; }
    FileSys::VirtualDir GetContainingDirectory() const override { return nullptr; }
    bool IsReadable() const override { return true; }
    bool IsWritable() const override { return false; }
    bool Resize(size_t) override { return false; }
    bool Rename(std::string_view) override { return false; }
    size_t Write(const u8*, size_t, size_t) override { return 0; }

    size_t Read(u8* output, size_t length, size_t position) const override {
        if (position >= size || length == 0) {
            return 0;
        }
        length = (std::min)(length, size - position);
        if (!compressed) {
            return source->Read(output, length, offset + position);
        }

        std::scoped_lock lock{mutex};
        if (position < decoded_offset || (!decoder && !finished)) {
            decoder.reset(ZSTD_createDCtx());
            if (!decoder || ZSTD_isError(ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_windowLogMax, 27))) {
                decoder.reset();
                return 0;
            }
            input = {};
            stored_offset = 0;
            decoded_offset = 0;
            decoded.clear();
            finished = false;
        }

        const size_t end = position + length;
        size_t copied = 0;
        for (;;) {
            const size_t block_end = decoded_offset + decoded.size();
            const size_t first = (std::max)(position, decoded_offset);
            const size_t last = (std::min)(end, block_end);
            if (first < last) {
                std::memcpy(output + first - position, decoded.data() + first - decoded_offset,
                            last - first);
                copied += last - first;
            }
            if (end <= block_end) {
                return copied;
            }

            // Keep the most recent decoded block for nearby reads. A seek backwards
            // beyond that block restarts the single-frame stream.
            decoded_offset = block_end;
            if (!DecodeBlock()) {
                decoder.reset();
                return 0;
            }
        }
    }

private:
    bool Refill() const {
        if (input.pos != input.size || stored_offset == stored) {
            return true;
        }
        const size_t count = (std::min)(stored - stored_offset, ChunkSize);
        compressed_bytes = source->ReadBytes(count, offset + stored_offset);
        if (compressed_bytes.size() != count) {
            return false;
        }
        stored_offset += count;
        input = {compressed_bytes.data(), count, 0};
        return true;
    }

    bool DecodeBlock() const {
        const size_t expected = size - decoded_offset;
        // Leave room for one extra byte when reading the last block so a frame
        // producing more than the declared member size is rejected.
        decoded.resize((std::min)(expected + size_t(expected < ChunkSize), ChunkSize));
        ZSTD_outBuffer block{decoded.data(), decoded.size(), 0};
        do {
            if (!Refill()) {
                return false;
            }
            const size_t input_pos = input.pos;
            const size_t output_pos = block.pos;
            const size_t remaining = ZSTD_decompressStream(decoder.get(), &block, &input);
            if (ZSTD_isError(remaining) || block.pos > expected) {
                return false;
            }
            if (remaining == 0) {
                if (block.pos != expected || stored_offset != stored || input.pos != input.size) {
                    return false;
                }
                finished = true;
                decoder.reset();
                compressed_bytes = {};
                input = {};
                break;
            }
            if (block.pos == output_pos && input.pos == input_pos) {
                return false;
            }
        } while (block.pos < block.size || block.pos == expected);
        decoded.resize(block.pos);
        return true;
    }

    static constexpr size_t ChunkSize = 128 * 1024;
    FileSys::VirtualFile source;
    std::string name;
    size_t offset;
    size_t stored;
    size_t size;
    bool compressed;

    mutable std::mutex mutex;
    mutable std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decoder{nullptr, ZSTD_freeDCtx};
    mutable std::vector<u8> compressed_bytes;
    mutable std::vector<u8> decoded;
    mutable ZSTD_inBuffer input{};
    mutable size_t stored_offset{};
    mutable size_t decoded_offset{};
    mutable bool finished{};
};

struct DirectoryRecord {
    std::string name;
    u32 first;
    u32 count;
};

struct PackageRecord {
    NupPackage package;
    u32 root;
    u32 exefs;
    u32 romfs;
};

class NupTree {
public:
    NupTree(const FileSys::VirtualFile& source_, Bytes pool_, Bytes children_, u64 blob_)
        : source(source_), pool(pool_), children(children_), blob(blob_) {}

    bool ReadDirectories(Bytes table);
    bool ReadFiles(Bytes table);
    bool ReadMetadata(NupPackage& package, u64 start, u64 size) const;
    bool BuildPackage(PackageRecord& record);

    bool FullyReferenced() const {
        return std::ranges::all_of(dirs, [](const auto& dir) { return dir != nullptr; }) &&
               std::ranges::none_of(files, [](const auto& file) { return file != nullptr; }) &&
               std::ranges::all_of(slots_used, [](bool used) { return used; });
    }

private:
    std::optional<std::string> ReadName(Bytes record) const;
    bool BuildDirectory(u32 index, u32 root, size_t depth, size_t path_size);

    const FileSys::VirtualFile& source;
    Bytes pool;
    Bytes children;
    u64 blob;
    std::vector<DirectoryRecord> directory_records;
    std::vector<FileSys::VirtualDir> dirs;
    std::vector<u32> roots;
    std::vector<FileSys::VirtualFile> files;
    std::vector<bool> slots_used;
};

std::optional<std::string> NupTree::ReadName(Bytes record) const {
    const u32 start = LittleEndian<u32>(record, 0);
    const u32 size = LittleEndian<u32>(record, 4);
    if (!Fits(start, size, pool.size()) || size > 0x300) {
        return std::nullopt;
    }
    if (size == 0) {
        return std::string{};
    }
    std::string name(reinterpret_cast<const char*>(pool.data() + start), size);
    if (name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos ||
        name.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return name;
}

bool NupTree::ReadDirectories(Bytes table) {
    const size_t count = table.size() / DirectorySize;
    dirs.resize(count);
    roots.resize(count);
    slots_used.resize(children.size() / sizeof(u32));
    directory_records.reserve(count);
    for (size_t pos = 0; pos < table.size(); pos += DirectorySize) {
        const auto record = table.subspan(pos, DirectorySize);
        auto name = ReadName(record);
        const u32 first = LittleEndian<u32>(record, 8);
        const u32 child_count = LittleEndian<u32>(record, 12);
        if (!name || !Fits(first, child_count, slots_used.size()) || !AllZero(record.subspan(16))) {
            return false;
        }
        directory_records.push_back({std::move(*name), first, child_count});
    }
    return true;
}

bool NupTree::ReadFiles(Bytes table) {
    files.reserve(table.size() / FileSize);
    for (size_t pos = 0; pos < table.size(); pos += FileSize) {
        const auto record = table.subspan(pos, FileSize);
        auto name = ReadName(record);
        const u64 size = LittleEndian<u64>(record, 8);
        const u64 offset = LittleEndian<u64>(record, 16);
        const u64 stored = LittleEndian<u64>(record, 24);
        const u8 codec = record[64];
        if (!name || name->empty() || codec > 1 || !AllZero(record.subspan(65)) ||
            (codec == 0 && size != stored) || !std::in_range<size_t>(size)) {
            return false;
        }
        if (size == 0 ? (offset != 0 || stored != 0 || codec != 0)
                      : (offset < blob || offset % 16 != 0 || stored == 0 ||
                         !Fits(offset, stored, source->GetSize()))) {
            return false;
        }
        files.emplace_back(std::make_shared<NupMember>(
            source, std::move(*name), static_cast<size_t>(offset), static_cast<size_t>(stored),
            static_cast<size_t>(size), codec == 1));
    }
    return true;
}

bool NupTree::ReadMetadata(NupPackage& package, u64 start, u64 size) const {
    if (size && (start < blob || !Fits(start, size, source->GetSize()))) {
        return false;
    }
    while (size != 0) {
        std::array<u8, 8> tlv{};
        if (size < tlv.size() || source->Read(tlv.data(), tlv.size(), start) != tlv.size() ||
            LittleEndian<u16>(tlv, 2) != 0) {
            return false;
        }
        const auto kind = static_cast<NupMetadata>(LittleEndian<u16>(tlv, 0));
        const u32 length = LittleEndian<u32>(tlv, 4);
        start += tlv.size();
        size -= tlv.size();
        if (length > size) {
            return false;
        }
        if (kind <= NupMetadata::TitleName) {
            package.metadata.insert_or_assign(kind, std::make_shared<NupMember>(
                source, "", static_cast<size_t>(start), length, length));
        }
        start += length;
        size -= length;
    }
    return true;
}

bool NupTree::BuildDirectory(u32 index, u32 root, size_t depth, size_t path_size) {
    if (depth > MaxDirectoryDepth || dirs[index]) {
        return false;
    }
    const auto& record = directory_records[index];
    if (index != root && record.name.empty()) {
        return false;
    }
    constexpr size_t MaxRomFsPath = 0x300;
    path_size += 1 + record.name.size();
    if (path_size > MaxRomFsPath) {
        return false;
    }
    auto dir = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, record.name);
    dirs[index] = dir;
    roots[index] = root;
    std::unordered_set<std::string> names;
    for (size_t slot = record.first; slot < size_t(record.first) + record.count; ++slot) {
        if (slots_used[slot]) {
            return false;
        }
        slots_used[slot] = true;
        const u32 child = LittleEndian<u32>(children, slot * sizeof(u32));
        if (child < dirs.size()) {
            if (!names.insert(directory_records[child].name).second ||
                !BuildDirectory(child, root, depth + 1, path_size)) {
                return false;
            }
            dir->AddDirectory(dirs[child]);
        } else {
            const size_t file_index = child - dirs.size();
            if (file_index >= files.size() || !files[file_index] ||
                path_size + 1 + files[file_index]->GetName().size() > MaxRomFsPath ||
                !names.insert(files[file_index]->GetName()).second) {
                return false;
            }
            dir->AddFile(std::move(files[file_index]));
        }
    }
    return true;
}

bool NupTree::BuildPackage(PackageRecord& record) {
    if (record.root >= dirs.size() || !BuildDirectory(record.root, record.root, 0, 0)) {
        return false;
    }
    record.package.root = dirs[record.root];
    for (const auto& [index, target] : {std::pair{record.exefs, &record.package.exefs},
                                       std::pair{record.romfs, &record.package.romfs}}) {
        if (index == NoDir) {
            continue;
        }
        if (index >= dirs.size() || !dirs[index] || roots[index] != record.root) {
            return false;
        }
        *target = dirs[index];
    }
    return true;
}

} // namespace

bool HasNupSignature(const FileSys::VirtualFile& file) {
    std::array<u8, 3> magic{};
    return file && file->Read(magic.data(), magic.size()) == magic.size() &&
           magic == std::array<u8, 3>{'N', 'U', 'P'};
}

std::optional<std::vector<NupPackage>> ReadNup(const FileSys::VirtualFile& file) {
    if (!file) {
        return std::nullopt;
    }
    const auto header = file->ReadBytes(HeaderSize);
    if (header.size() != HeaderSize || std::memcmp(header.data(), "NUP\1", 4) != 0 ||
        LittleEndian<u16>(header, 4) != HeaderSize || LittleEndian<u16>(header, 6) != 1 ||
        !AllZero(Bytes{header}.subspan(96))) {
        return std::nullopt;
    }
    const u64 toc_offset = LittleEndian<u64>(header, 8);
    const u64 toc_size = LittleEndian<u64>(header, 16);
    const u64 blob = LittleEndian<u64>(header, 56);
    const u64 file_size = LittleEndian<u64>(header, 64);
    const u32 packages = LittleEndian<u32>(header, 72);
    if (file_size != file->GetSize() || LittleEndian<u32>(header, 76) != 0 ||
        toc_offset != HeaderSize || toc_size < TocHeaderSize || toc_size > MaxTocSize ||
        blob % 16 != 0 || blob > file_size || !Fits(toc_offset, toc_size, blob)) {
        return std::nullopt;
    }
    const auto toc_data = file->ReadBytes(static_cast<size_t>(toc_size), static_cast<size_t>(toc_offset));
    const Bytes toc{toc_data};
    std::array<u8, 32> hash{};
    unsigned int hash_size{};
    if (toc.size() != toc_size ||
        EVP_Digest(toc.data(), toc.size(), hash.data(), &hash_size, EVP_sha256(), nullptr) != 1 ||
        hash_size != hash.size() || !std::equal(hash.begin(), hash.end(), header.begin() + 24) ||
        std::memcmp(toc.data(), "TOC\1", 4) != 0 || LittleEndian<u32>(toc, 4) != packages ||
        !AllZero(toc.subspan(40, 8))) {
        return std::nullopt;
    }

    const u64 dirs = LittleEndian<u64>(toc, 8);
    const u64 files = LittleEndian<u64>(toc, 16);
    const u64 pool_offset = LittleEndian<u64>(toc, 24);
    const u64 pool_size = LittleEndian<u64>(toc, 32);
    if (!Fits(pool_offset, pool_size, toc_size) || dirs > toc_size / DirectorySize ||
        files > toc_size / FileSize) {
        return std::nullopt;
    }
    const u64 dirs_offset = TocHeaderSize + u64(packages) * PackageSize;
    const u64 files_offset = dirs_offset + dirs * DirectorySize;
    const u64 children_offset = files_offset + files * FileSize;
    if (children_offset > pool_offset || (pool_offset - children_offset) % sizeof(u32) != 0) {
        return std::nullopt;
    }

    NupTree tree{file, toc.subspan(pool_offset, pool_size),
                 toc.subspan(children_offset, pool_offset - children_offset), blob};
    if (!tree.ReadDirectories(toc.subspan(dirs_offset, dirs * DirectorySize)) ||
        !tree.ReadFiles(toc.subspan(files_offset, files * FileSize))) {
        return std::nullopt;
    }

    std::vector<NupPackage> result;
    result.reserve(packages);
    bool has_base = false;
    bool requires_base = false;
    for (size_t pos = TocHeaderSize; pos < dirs_offset; pos += PackageSize) {
        const auto record = toc.subspan(pos, PackageSize);
        const auto kind = static_cast<NupKind>(record[24]);
        const u8 flags = record[25];
        if (kind > NupKind::System || flags > 1 || !AllZero(record.subspan(26, 6)) ||
            !AllZero(record.subspan(44))) {
            return std::nullopt;
        }
        PackageRecord package{
            {.title_id = LittleEndian<u64>(record, 0),
             .version = LittleEndian<u32>(record, 20),
             .kind = kind,
             .standalone_update = flags == 1,
             .root = {}, .exefs = {}, .romfs = {}, .metadata = {}},
            LittleEndian<u32>(record, 8), LittleEndian<u32>(record, 12),
            LittleEndian<u32>(record, 16)};
        has_base |= kind == NupKind::Base;
        requires_base |= kind == NupKind::Update && !package.package.standalone_update;
        if (!tree.BuildPackage(package) ||
            !tree.ReadMetadata(package.package, LittleEndian<u64>(record, 32),
                               LittleEndian<u32>(record, 40))) {
            return std::nullopt;
        }
        result.emplace_back(std::move(package.package));
    }
    if ((requires_base && !has_base) || !tree.FullyReferenced()) {
        return std::nullopt;
    }
    return result;
}

} // namespace Loader
