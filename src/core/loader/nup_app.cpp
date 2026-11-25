// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "core/file_sys/common_funcs.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nup_app.h"

namespace Loader {
namespace {

FileSys::VirtualDir Overlay(const FileSys::VirtualDir& base, const FileSys::VirtualDir& update) {
    if (base && update && base != update) {
        return FileSys::LayeredVfsDirectory::MakeLayeredDirectory({update, base}, base->GetName());
    }
    return update ? update : base;
}

FileSys::VirtualFile Metadata(const NupPackage& package, NupMetadata kind) {
    const auto entry = package.metadata.find(kind);
    return entry == package.metadata.end() ? nullptr : entry->second;
}

bool SameProgram(u64 base, u64 update) {
    return base == update || (base != 0 && update == (base | 0x800));
}

bool HasMagic(const FileSys::VirtualFile& file, const std::array<u8, 4>& magic) {
    std::array<u8, 4> header{};
    return file && file->Read(header.data(), header.size()) == header.size() && header == magic;
}

std::optional<u64> ProgramId(const FileSys::VirtualFile& npdm) {
    if (!npdm || npdm->GetSize() > 1024 * 1024 || !HasMagic(npdm, {'M', 'E', 'T', 'A'})) {
        return {};
    }
    FileSys::ProgramMetadata metadata;
    if (metadata.Load(npdm) != ResultStatus::Success) {
        return {};
    }
    return metadata.GetTitleID();
}

} // namespace

AppLoader_NUP::AppLoader_NUP(FileSys::VirtualFile file_, u64 program_id, size_t program_index)
    : AppLoader(std::move(file_)), packages(ReadNup(file)) {
    if (!packages) {
        return;
    }

    std::vector<const NupPackage*> bases;
    for (const auto& package : *packages) {
        if (package.kind == NupKind::Base ||
            (package.kind == NupKind::Update && package.standalone_update && package.exefs &&
             package.exefs->GetFile("main") && package.exefs->GetFile("main.npdm"))) {
            const auto duplicate = std::ranges::find_if(bases, [&](const NupPackage* base) {
                return SameProgram(base->title_id, package.title_id) ||
                       SameProgram(package.title_id, base->title_id);
            });
            if (duplicate == bases.end()) {
                bases.emplace_back(&package);
            } else if (package.kind == NupKind::Base) {
                *duplicate = &package;
            }
        }
    }

    const NupPackage* selected = nullptr;
    const NupPackage* update = nullptr;
    for (const auto* base : bases) {
        const NupPackage* latest = base;
        for (const auto& package : *packages) {
            if (package.kind == NupKind::Update && SameProgram(base->title_id, package.title_id) &&
                package.version >= base->version &&
                (latest->kind == NupKind::Base || package.version > latest->version)) {
                latest = &package;
            }
        }
        auto candidate = Overlay(base->exefs, latest->exefs);
        if (!candidate || !HasMagic(candidate->GetFile("main"), {'N', 'S', 'O', '0'})) {
            continue;
        }
        const auto npdm = candidate->GetFile("main.npdm");
        const auto original = base->exefs ? base->exefs->GetFile("main.npdm") : nullptr;
        const auto effective_id = ProgramId(npdm);
        const auto original_id = original && original != npdm ? ProgramId(original) : effective_id;
        if (!effective_id || !original_id || !SameProgram(*original_id, *effective_id) ||
            (base->title_id != 0 && !SameProgram(*original_id, base->title_id))) {
            continue;
        }
        const u64 id = *original_id;
        if (std::ranges::find(program_ids, id) != program_ids.end()) {
            continue;
        }
        if ((program_id != 0 && program_id == id) ||
            (program_id == 0 && program_index == program_ids.size())) {
            selected = base;
            update = latest;
            exefs = std::move(candidate);
            base_npdm = original;
            title_id = id;
        }
        program_ids.push_back(id);
    }

    if (!selected) {
        status = ResultStatus::ErrorNUPMissingProgram;
        return;
    }

    romfs_dir = Overlay(selected->romfs, update->romfs);
    nacp = Metadata(*update, NupMetadata::Nacp);
    if (!nacp) {
        nacp = Metadata(*selected, NupMetadata::Nacp);
    }
    if (!nacp) {
        nacp = exefs->GetFile("control.nacp");
    }
    name = Metadata(*selected, NupMetadata::TitleName);
    icon = Metadata(*selected, NupMetadata::Icon);
    version = update->version;
    status = ResultStatus::Success;
}

AppLoader_NUP::~AppLoader_NUP() = default;

FileType AppLoader_NUP::GetFileType() const {
    return FileType::NUP;
}

AppLoader::LoadResult AppLoader_NUP::Load(Kernel::KProcess& process, Core::System& system) {
    if (status != ResultStatus::Success) {
        return {status, {}};
    }
    if (!application) {
        FileSys::VirtualFile romfs_file;
        ReadRomFS(romfs_file);
        std::map<u64, const NupPackage*> addons;
        for (const auto& package : *packages) {
            if (package.kind == NupKind::AddOn && package.romfs &&
                FileSys::ClassifyTitleRelation(title_id, package.title_id) ==
                    FileSys::TitleRelation::Aoc) {
                addons.try_emplace(package.title_id, &package);
            }
        }
        std::vector<std::pair<u64, FileSys::VirtualFile>> baked_aoc;
        for (const auto& [id, base] : addons) {
            const NupPackage* latest = base;
            for (const auto& package : *packages) {
                if (package.kind == NupKind::AddOnUpdate && package.romfs &&
                    package.title_id == id && package.version > latest->version) {
                    latest = &package;
                }
            }
            baked_aoc.emplace_back(id, FileSys::CreateRomFS(Overlay(base->romfs, latest->romfs)));
        }
        application = std::make_unique<AppLoader_DeconstructedRomDirectory>(
            exefs, false, std::move(romfs_file), std::move(baked_aoc), base_npdm);
    }
    return application->Load(process, system);
}

ResultStatus AppLoader_NUP::ReadProgramId(u64& out_program_id) {
    if (status == ResultStatus::Success) {
        out_program_id = title_id;
    }
    return status;
}

ResultStatus AppLoader_NUP::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (status == ResultStatus::Success) {
        out_program_ids = program_ids;
    }
    return status;
}

ResultStatus AppLoader_NUP::ReadRomFS(FileSys::VirtualFile& out_file) {
    if (!romfs && romfs_dir) {
        romfs = FileSys::CreateRomFS(romfs_dir);
    }
    if (!romfs) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadNSOModules(Modules& out_modules) {
    return application ? application->ReadNSOModules(out_modules)
                       : ResultStatus::ErrorNotInitialized;
}

ResultStatus AppLoader_NUP::ReadTitle(std::string& title) {
    if (name && name->GetSize() != 0 && name->GetSize() <= 4096) {
        const auto bytes = name->ReadAllBytes();
        if (bytes.size() == name->GetSize()) {
            title.assign(bytes.begin(), bytes.end());
            return ResultStatus::Success;
        }
    }
    FileSys::NACP control;
    if (ReadControlData(control) == ResultStatus::Success) {
        title = control.GetApplicationName();
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}

ResultStatus AppLoader_NUP::ReadIcon(std::vector<u8>& out_icon) {
    if (!icon || icon->GetSize() == 0 || icon->GetSize() > 4 * 1024 * 1024) {
        return ResultStatus::ErrorNoIcon;
    }
    auto bytes = icon->ReadAllBytes();
    if (bytes.size() != icon->GetSize()) {
        return ResultStatus::ErrorNoIcon;
    }
    out_icon = std::move(bytes);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadControlData(FileSys::NACP& control) {
    if (!nacp || nacp->GetSize() != sizeof(FileSys::RawNACP)) {
        return ResultStatus::ErrorNoControl;
    }
    control = FileSys::NACP(nacp);
    return ResultStatus::Success;
}

std::optional<u32> AppLoader_NUP::GetApplicationVersion() const {
    return status == ResultStatus::Success ? std::optional{version} : std::nullopt;
}

bool AppLoader_NUP::IsRomFSUpdatable() const {
    return false;
}

} // namespace Loader
