// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/export_content.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <fmt/format.h>

#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"

namespace FileSys {
namespace {

std::string LowerExtension(std::string name) {
    const auto pos = name.rfind('.');
    if (pos == std::string::npos) {
        return {};
    }
    std::string ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string SourceLabel(PatchSource source) {
    switch (source) {
    case PatchSource::NAND:
        return "NAND";
    case PatchSource::SDMC:
        return "SDMC";
    case PatchSource::External:
        return "game directory / picked file";
    case PatchSource::Packed:
        return "packed in ROM";
    case PatchSource::Unknown:
    default:
        return "installed";
    }
}

bool TitleMatches(u64 base_title_id, u64 other) {
    return ClassifyTitleRelation(base_title_id, other) != TitleRelation::Unrelated;
}

} // namespace

ExportContentSession::ExportContentSession()
    : vfs{std::make_shared<RealVfsFilesystem>()},
      manual{std::make_unique<ManualContentProvider>()},
      overlay{std::make_unique<ContentProviderUnion>()} {
    overlay->SetSlot(ContentProviderUnionSlot::FrontendManual, manual.get());
}

ExportContentSession::~ExportContentSession() = default;

bool ExportContentSession::RegisterPath(const std::string& path, bool is_base_rom) {
    auto file = vfs->OpenFile(path, OpenMode::Read);
    if (!file) {
        const auto dir = vfs->OpenDirectory(path, OpenMode::Read);
        if (!dir) {
            return false;
        }
        VirtualDir dir_exefs;
        if (auto main_file = dir->GetFile("main")) {
            file = std::move(main_file);
            dir_exefs = dir;
        } else if (const auto exefs = dir->GetSubdirectory("exefs")) {
            file = exefs->GetFile("main");
            dir_exefs = exefs;
        }
        if (is_base_rom && dir_exefs) {
            directory_exefs = dir_exefs;
            if (auto parent = dir_exefs->GetParentDirectory()) {
                directory_romfs = parent->GetFile("romfs.bin");
                if (!directory_romfs) {
                    directory_romfs = parent->GetFile("romfs");
                }
            }
            if (!directory_romfs) {
                directory_romfs = dir_exefs->GetFile("romfs.bin");
                if (!directory_romfs) {
                    directory_romfs = dir_exefs->GetFile("romfs");
                }
            }
            if (!directory_romfs) {
                directory_romfs = dir->GetFile("romfs.bin");
                if (!directory_romfs) {
                    directory_romfs = dir->GetFile("romfs");
                }
            }
        }
        if (!file) {
            // Extracted directory ROM: PatchManager can still apply NAND/manual addons
            // once title_id is known if we kept the ExeFS directory above.
            return is_base_rom && directory_exefs != nullptr;
        }
    }

    if (manual->AddEntriesFromContainer(file)) {
        return true;
    }

    const auto ext = LowerExtension(file->GetName());
    if (ext == ".nca") {
        const NCA nca{file};
        if (nca.GetStatus() == Loader::ResultStatus::Success) {
            const auto title = nca.GetTitleId();
            const auto record = GetCRTypeFromNCAType(nca.GetType());
            TitleType type = TitleType::Application;
            switch (ClassifyTitleRelation(GetBaseTitleID(title), title)) {
            case TitleRelation::Update:
                type = TitleType::Update;
                break;
            case TitleRelation::Aoc:
                type = TitleType::AOC;
                break;
            default:
                type = TitleType::Application;
                break;
            }
            manual->AddEntry(type, record, title, file);
            return true;
        }
    }

    return is_base_rom;
}

void ExportContentSession::CopyMatchingManualEntries(const ContentProvider& source) {
    static constexpr std::array types{TitleType::Application, TitleType::Update, TitleType::AOC};
    for (const auto title_type : types) {
        for (const auto& entry : source.ListEntriesFilter(title_type, {}, {})) {
            if (!TitleMatches(title_id, entry.title_id)) {
                continue;
            }
            if (auto raw = source.GetEntryRaw(entry)) {
                manual->AddEntry(title_type, entry.type, entry.title_id, raw);
            }
        }
    }
}

bool ExportContentSession::ApplyPatches(Core::System& system) {
    const auto& fsc = system.GetFileSystemController();
    const PatchManager pm{title_id, fsc, *overlay};

    base_program_nca = overlay->GetEntry(title_id, ContentRecordType::Program);
    VirtualDir base_exefs = base_program_nca ? base_program_nca->GetExeFS() : nullptr;
    VirtualFile base_romfs = base_program_nca ? base_program_nca->GetRomFS() : nullptr;
    if (!base_exefs) {
        base_exefs = directory_exefs;
    }
    if (!base_romfs) {
        base_romfs = directory_romfs;
    }

    const u64 update_tid = GetUpdateTitleID(title_id);
    held_update_nca = overlay->GetEntry(update_tid, ContentRecordType::Program);
    VirtualDir update_exefs;
    if (held_update_nca && held_update_nca->GetStatus() == Loader::ResultStatus::Success) {
        update_exefs = held_update_nca->GetExeFS();
    }

    update_exefs_applied = false;
    if (base_exefs) {
        patched_exefs = pm.PatchExeFS(base_exefs);
    } else if (update_exefs && update_exefs->GetFile("main")) {
        patched_exefs = update_exefs;
    } else {
        patched_exefs = nullptr;
    }

    const bool update_present = overlay->HasEntry(update_tid, ContentRecordType::Program);
    // Honest ExeFS replace: PatchExeFS actually returned a different dir, or we
    // substituted the update ExeFS because there was no base.
    update_exefs_applied =
        PatchHandleReplaced(update_present, patched_exefs != nullptr, patched_exefs == base_exefs);

    update_romfs_applied = false;
    if (base_program_nca) {
        patched_romfs =
            pm.PatchRomFS(base_program_nca.get(), base_romfs, ContentRecordType::Program);
    } else {
        // Directory dump: no Program NCA means PatchRomFS cannot BKTR.
        patched_romfs = base_romfs;
    }
    if (!patched_romfs) {
        patched_romfs = base_romfs;
    }
    // Honest RomFS replace: PatchRomFS output is not the same object as the base.
    update_romfs_applied =
        PatchHandleReplaced(update_present, patched_romfs != nullptr, patched_romfs == base_romfs);

    aoc.clear();
    for (const auto& entry :
         overlay->ListEntriesFilter(TitleType::AOC, ContentRecordType::Data, {})) {
        if (ClassifyTitleRelation(title_id, entry.title_id) != TitleRelation::Aoc) {
            continue;
        }
        auto aoc_nca = overlay->GetEntry(entry.title_id, ContentRecordType::Data);
        if (!aoc_nca || aoc_nca->GetStatus() != Loader::ResultStatus::Success) {
            continue;
        }
        const PatchManager aoc_pm{entry.title_id, fsc, *overlay};
        auto aoc_romfs =
            aoc_pm.PatchRomFS(aoc_nca.get(), aoc_nca->GetRomFS(), ContentRecordType::Data);
        if (!aoc_romfs) {
            aoc_romfs = aoc_nca->GetRomFS();
        }
        if (aoc_romfs) {
            aoc.push_back(ExportAocSnapshot{entry.title_id, std::move(aoc_romfs)});
        }
        held_aoc_ncas.push_back(std::move(aoc_nca));
    }

    std::vector<ExportBakeItem> candidates;
    const auto patches = pm.GetPatches();
    for (const auto& patch : patches) {
        if (!patch.enabled) {
            continue;
        }
        if (patch.type == PatchType::Update) {
            ExportBakeItem item;
            item.kind = ExportBakeItem::Kind::Update;
            item.name = patch.version.empty() ? patch.name : fmt::format("{} {}", patch.name, patch.version);
            item.source = SourceLabel(patch.source);
            candidates.push_back(std::move(item));
        } else if (patch.type == PatchType::DLC) {
            ExportBakeItem item;
            item.kind = ExportBakeItem::Kind::Dlc;
            item.name = patch.version.empty() ? patch.name : fmt::format("{} {}", patch.name, patch.version);
            item.source = SourceLabel(patch.source);
            candidates.push_back(std::move(item));
        }
    }
    bake_items = FilterAppliedBakeItems(candidates, false, aoc.size());
    const auto decision = DecideUpdateBake(update_present, base_program_nca != nullptr,
                                           update_exefs_applied, update_romfs_applied);
    if (const char* refusal = UpdateBakeRefusal(decision)) {
        error = refusal;
        status = error;
        return false;
    }
    bake_items = FilterAppliedBakeItems(candidates, decision == UpdateBakeDecision::Applied,
                                        aoc.size());
    status = FormatExportBakeStatus(bake_items);
    return true;
}

bool ExportContentSession::Resolve(Core::System& system, const ExportContentRequest& request) {
    error.clear();
    failed_addon_paths.clear();
    bake_items.clear();
    aoc.clear();
    patched_exefs = nullptr;
    patched_romfs = nullptr;
    base_program_nca.reset();
    held_update_nca.reset();
    held_aoc_ncas.clear();
    directory_exefs = nullptr;
    directory_romfs = nullptr;
    update_exefs_applied = false;
    update_romfs_applied = false;
    title_id = request.title_id;
    status.clear();

    manual->ClearAllEntries();
    overlay = std::make_unique<ContentProviderUnion>();
    overlay->SetSlot(ContentProviderUnionSlot::FrontendManual, manual.get());

    if (request.rom_path.empty()) {
        error = "No ROM path";
        status = error;
        return false;
    }

    if (!RegisterPath(request.rom_path, true)) {
        error = "Could not open the base ROM";
        status = error;
        return false;
    }

    if (title_id == 0) {
        if (auto file = vfs->OpenFile(request.rom_path, OpenMode::Read)) {
            if (auto loader = Loader::GetLoader(system, file)) {
                loader->ReadProgramId(title_id);
            }
        } else if (auto dir = vfs->OpenDirectory(request.rom_path, OpenMode::Read)) {
            FileSys::VirtualFile main_file = dir->GetFile("main");
            if (!main_file) {
                if (const auto exefs = dir->GetSubdirectory("exefs")) {
                    main_file = exefs->GetFile("main");
                }
            }
            if (main_file) {
                if (auto loader = Loader::GetLoader(system, main_file)) {
                    loader->ReadProgramId(title_id);
                }
            }
        }
        title_id = GetBaseTitleID(title_id);
    }

    if (const auto* sys_manual =
            system.GetContentProviderUnion().GetSlotProvider(ContentProviderUnionSlot::FrontendManual);
        sys_manual != nullptr && title_id != 0) {
        CopyMatchingManualEntries(*sys_manual);
    }

    for (const auto& extra : request.extra_addon_paths) {
        if (!RegisterPath(extra, false)) {
            failed_addon_paths.push_back(extra);
        }
    }

    auto& sys_union = system.GetContentProviderUnion();
    if (request.use_nand_addons) {
        overlay->SetSlot(ContentProviderUnionSlot::SysNAND,
                         sys_union.GetSlotProvider(ContentProviderUnionSlot::SysNAND));
        overlay->SetSlot(ContentProviderUnionSlot::UserNAND,
                         sys_union.GetSlotProvider(ContentProviderUnionSlot::UserNAND));
        overlay->SetSlot(ContentProviderUnionSlot::SDMC,
                         sys_union.GetSlotProvider(ContentProviderUnionSlot::SDMC));
    }
    overlay->SetSlot(ContentProviderUnionSlot::External,
                     sys_union.GetSlotProvider(ContentProviderUnionSlot::External));

    if (title_id == 0) {
        error = "Could not determine title ID";
        status = error;
        return false;
    }

    if (!ApplyPatches(system)) {
        if (error.empty()) {
            error = "Failed to apply update/DLC patches";
        }
        status = error;
        return false;
    }

    if (!failed_addon_paths.empty()) {
        error = FormatFailedAddonNote(failed_addon_paths.size());
        if (!status.empty()) {
            status += ' ';
        }
        status += error;
        return false;
    }
    return true;
}

std::string DescribeExportContent(Core::System& system, const ExportContentRequest& request) {
    ExportContentSession session;
    session.Resolve(system, request);
    return session.GetStatus();
}

} // namespace FileSys
