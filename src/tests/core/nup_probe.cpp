// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nup.h"
#ifndef NUP_PARSER_ONLY
#include "core/core.h"
#include "core/file_sys/romfs.h"
#include "core/loader/loader.h"
#endif

namespace {

constexpr u64 TitleId = 0x0100123456780000;
constexpr size_t CompressedSize = 3 * 128 * 1024;
constexpr std::string_view Title = "NUP round-trip fixture";

bool CheckContents(const FileSys::VirtualDir& romfs) {
    const auto dir = romfs ? romfs->GetSubdirectory("nested") : nullptr;
    if (!dir) {
        return false;
    }
    const auto small = dir->GetFile("small.bin");
    const auto empty = dir->GetFile("empty.bin");
    const auto large = dir->GetFile("compressible.bin");
    if (!small || !empty || !large || small->ReadAllBytes() != std::vector<u8>{1, 2, 3} ||
        empty->GetSize() != 0 || large->GetSize() != CompressedSize) {
        return false;
    }

    // Cross decoder chunks, seek backwards, and read through the end of a frame.
    return large->ReadBytes(30, 128 * 1024 - 10) == std::vector<u8>(30, 'x') &&
           large->ReadBytes(5, 3) == std::vector<u8>(5, 'x') &&
           large->ReadBytes(20, CompressedSize - 13) == std::vector<u8>(13, 'x') &&
           large->ReadBytes(10, CompressedSize - 10) == std::vector<u8>(10, 'x') &&
           large->ReadBytes(1, CompressedSize).empty();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    std::ifstream stream(std::filesystem::path(argv[1]), std::ios::binary);
    if (!stream) {
        return 2;
    }
    const std::vector<u8> bytes{std::istreambuf_iterator<char>(stream), {}};
    auto file = std::make_shared<FileSys::VectorVfsFile>(bytes, "fixture.nup");
    const auto packages = Loader::ReadNup(file);
    if (std::string_view(argv[2]) == "invalid") {
        return Loader::HasNupSignature(file) && !packages ? 0 : 1;
    }
    if (!Loader::HasNupSignature(file) || !packages || packages->size() != 2) {
        std::cerr << "Failed to read the NUP package table\n";
        return 1;
    }

    const auto& base = packages->at(0);
    const auto& update = packages->at(1);
    const auto metadata = base.metadata.find(Loader::NupMetadata::TitleName);
    if (base.kind != Loader::NupKind::Base || base.title_id != TitleId || !base.exefs ||
        !base.exefs->GetFile("main") || metadata == base.metadata.end() ||
        metadata->second->ReadAllBytes() != std::vector<u8>(Title.begin(), Title.end()) ||
        !CheckContents(base.romfs) || update.kind != Loader::NupKind::Update ||
        update.title_id != (TitleId | 0x800) || update.version != 65536 || !update.romfs) {
        std::cerr << "NUP member data or metadata did not round-trip\n";
        return 1;
    }

#ifndef NUP_PARSER_ONLY
    Core::System system;
    if (Loader::IdentifyFile(file) != Loader::FileType::NUP ||
        Loader::GuessFromFilename("fixture.NUP") != Loader::FileType::NUP) {
        return 1;
    }
    auto loader = Loader::GetLoader(system, file, TitleId);
    u64 id{};
    std::string title;
    FileSys::VirtualFile romfs;
    if (!loader || loader->GetFileType() != Loader::FileType::NUP ||
        loader->ReadProgramId(id) != Loader::ResultStatus::Success || id != TitleId ||
        loader->ReadTitle(title) != Loader::ResultStatus::Success || title != Title ||
        loader->GetApplicationVersion() != update.version ||
        loader->ReadRomFS(romfs) != Loader::ResultStatus::Success) {
        std::cerr << "The standard loader could not select the NUP application\n";
        return 1;
    }
    const auto effective = FileSys::ExtractRomFS(romfs);
    const auto patched = effective ? effective->GetFile("updated.bin") : nullptr;
    if (!CheckContents(effective) || !patched ||
        patched->ReadAllBytes() != std::vector<u8>{4, 5, 6}) {
        std::cerr << "NUP effective RomFS did not contain the base and update files\n";
        return 1;
    }
#endif
    return 0;
}
