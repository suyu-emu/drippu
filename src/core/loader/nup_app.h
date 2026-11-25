// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/loader/loader.h"
#include "core/loader/nup.h"

namespace Loader {

class AppLoader_DeconstructedRomDirectory;

class AppLoader_NUP final : public AppLoader {
public:
    explicit AppLoader_NUP(FileSys::VirtualFile file, u64 program_id, size_t program_index);
    ~AppLoader_NUP() override;

    FileType GetFileType() const override;
    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadProgramIds(std::vector<u64>& out_program_ids) override;
    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    ResultStatus ReadNSOModules(Modules& out_modules) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadIcon(std::vector<u8>& icon) override;
    ResultStatus ReadControlData(FileSys::NACP& control) override;
    bool IsRomFSUpdatable() const override;
    std::optional<u32> GetApplicationVersion() const override;

private:
    std::optional<std::vector<NupPackage>> packages;
    FileSys::VirtualDir exefs;
    FileSys::VirtualFile base_npdm;
    FileSys::VirtualDir romfs_dir;
    FileSys::VirtualFile romfs;
    FileSys::VirtualFile nacp;
    FileSys::VirtualFile name;
    FileSys::VirtualFile icon;
    std::vector<u64> program_ids;
    u64 title_id{};
    u32 version{};
    ResultStatus status{ResultStatus::ErrorBadNUPHeader};
    std::unique_ptr<AppLoader_DeconstructedRomDirectory> application;
};

} // namespace Loader
