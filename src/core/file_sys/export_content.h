// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/export_bake.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core {
class System;
}

namespace FileSys {

class NCA;

/// Inputs for resolving the content snapshot an export should bake.
struct ExportContentRequest {
    std::string rom_path;
    std::vector<std::string> extra_addon_paths;
    bool use_nand_addons{true};
    u64 title_id{}; ///< 0 = detect from the ROM
};

struct ExportAocSnapshot {
    u64 title_id{};
    VirtualFile romfs;
};

/// Holds VFS/provider state for as long as the resolved VirtualFile/Dir
/// objects are used. Destroying the session invalidates those handles.
class ExportContentSession {
public:
    ExportContentSession();
    ~ExportContentSession();

    ExportContentSession(const ExportContentSession&) = delete;
    ExportContentSession& operator=(const ExportContentSession&) = delete;

    /// Register ROM + extras (+ optional NAND) and apply PatchManager.
    bool Resolve(Core::System& system, const ExportContentRequest& request);

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
    [[nodiscard]] const std::string& GetError() const {
        return error;
    }
    [[nodiscard]] u64 GetTitleID() const {
        return title_id;
    }
    [[nodiscard]] VirtualDir GetPatchedExeFS() const {
        return patched_exefs;
    }
    [[nodiscard]] VirtualFile GetPatchedRomFS() const {
        return patched_romfs;
    }
    [[nodiscard]] const std::vector<ExportBakeItem>& GetBakeItems() const {
        return bake_items;
    }
    [[nodiscard]] const std::vector<ExportAocSnapshot>& GetAoc() const {
        return aoc;
    }
    [[nodiscard]] const std::string& GetStatus() const {
        return status;
    }
    [[nodiscard]] const std::vector<std::string>& GetFailedAddonPaths() const {
        return failed_addon_paths;
    }
    [[nodiscard]] bool UpdateExeFSApplied() const {
        return update_exefs_applied;
    }
    [[nodiscard]] bool UpdateRomFSApplied() const {
        return update_romfs_applied;
    }

private:
    bool RegisterPath(const std::string& path, bool is_base_rom);
    void CopyMatchingManualEntries(const class ContentProvider& source);
    bool ApplyPatches(Core::System& system);

    std::shared_ptr<class RealVfsFilesystem> vfs;
    std::unique_ptr<class ManualContentProvider> manual;
    std::unique_ptr<class ContentProviderUnion> overlay;
    std::unique_ptr<NCA> base_program_nca;
    std::unique_ptr<NCA> held_update_nca;
    std::vector<std::unique_ptr<NCA>> held_aoc_ncas;
    VirtualDir directory_exefs;
    VirtualFile directory_romfs;

    u64 title_id{};
    bool update_exefs_applied{false};
    bool update_romfs_applied{false};
    VirtualDir patched_exefs;
    VirtualFile patched_romfs;
    std::vector<ExportBakeItem> bake_items;
    std::vector<ExportAocSnapshot> aoc;
    std::vector<std::string> failed_addon_paths;
    std::string status;
    std::string error;
};

/// Describe what would be baked without keeping file handles. Safe for UI.
[[nodiscard]] std::string DescribeExportContent(Core::System& system,
                                                const ExportContentRequest& request);

} // namespace FileSys
