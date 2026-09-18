// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace FileSys {

/// One update or DLC entry that will be baked into a standalone export snapshot.
struct ExportBakeItem {
    enum class Kind {
        Update,
        Dlc,
    };

    Kind kind{Kind::Update};
    std::string name;   ///< e.g. "Update v1.2.0" or "DLC 1, 2"
    std::string source; ///< e.g. "NAND", "picked file", "game directory"
};

/// User-facing summary of what an export will bake. Header-only so the
/// exporter-smoke target can cover it without linking the emulator.
inline std::string FormatExportBakeStatus(const std::vector<ExportBakeItem>& items) {
    if (items.empty()) {
        return "Baking: base game only (no separate update or DLC). "
               "Standalone snapshot — no NAND install needed after export.";
    }

    std::string out = "Baking: ";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += "; ";
        }
        out += items[i].name;
        if (!items[i].source.empty()) {
            out += " (";
            out += items[i].source;
            out += ')';
        }
    }
    out += ". Standalone snapshot — no NAND install needed after export.";
    return out;
}

inline std::string_view ExportBakeKindName(ExportBakeItem::Kind kind) {
    switch (kind) {
    case ExportBakeItem::Kind::Update:
        return "Update";
    case ExportBakeItem::Kind::Dlc:
        return "DLC";
    }
    return "Add-on";
}

/// GetPatches can list an update even when PatchExeFS was a no-op (null
/// base ExeFS) or PatchRomFS could not BKTR. Only keep Update when the
/// snapshot actually applied both ExeFS replace and RomFS patch.
inline std::vector<ExportBakeItem> FilterAppliedBakeItems(
    const std::vector<ExportBakeItem>& candidates, bool update_exefs_applied,
    std::size_t dumped_aoc_count) {
    std::vector<ExportBakeItem> out;
    out.reserve(candidates.size());
    bool have_dlc = false;
    for (const auto& item : candidates) {
        if (item.kind == ExportBakeItem::Kind::Update) {
            if (update_exefs_applied) {
                out.push_back(item);
            }
        } else if (item.kind == ExportBakeItem::Kind::Dlc) {
            if (dumped_aoc_count > 0) {
                out.push_back(item);
                have_dlc = true;
            }
        }
    }
    if (dumped_aoc_count > 0 && !have_dlc) {
        ExportBakeItem dlc;
        dlc.kind = ExportBakeItem::Kind::Dlc;
        dlc.name = dumped_aoc_count == 1 ? "DLC" : "DLC (" + std::to_string(dumped_aoc_count) + ")";
        dlc.source = "baked snapshot";
        out.push_back(std::move(dlc));
    }
    return out;
}

inline std::string FormatFailedAddonNote(std::size_t failed_count) {
    if (failed_count == 0) {
        return {};
    }
    return std::to_string(failed_count) +
           " extra file(s) could not be read. Export will not continue until they "
           "are removed or readable.";
}

/// Whether an enabled update can honestly be listed as baked.
/// Directory dumps have no Program NCA, so PatchRomFS cannot BKTR; listing
/// the update would ship update NSOs against base romfs.bin.
enum class UpdateBakeDecision {
    NotPresent,
    Applied,
    MissingBaseProgramNca,
    Incomplete,
};

inline UpdateBakeDecision DecideUpdateBake(bool update_present, bool has_base_program_nca,
                                           bool update_exefs_applied, bool update_romfs_applied) {
    if (!update_present) {
        return UpdateBakeDecision::NotPresent;
    }
    if (!has_base_program_nca) {
        return UpdateBakeDecision::MissingBaseProgramNca;
    }
    if (update_exefs_applied && update_romfs_applied) {
        return UpdateBakeDecision::Applied;
    }
    return UpdateBakeDecision::Incomplete;
}

inline const char* UpdateBakeRefusal(UpdateBakeDecision decision) {
    switch (decision) {
    case UpdateBakeDecision::MissingBaseProgramNca:
        return "Cannot bake update onto a directory dump: RomFS patching needs the "
               "base Program NCA (BKTR). Export the original NSP/XCI.";
    case UpdateBakeDecision::Incomplete:
        return "Update was found but ExeFS replace and RomFS patch did not both "
               "apply; refusing a mixed snapshot.";
    default:
        return nullptr;
    }
}

/// True when PatchManager returned a different handle than the base input.
inline bool PatchHandleReplaced(bool update_present, bool has_patched, bool same_as_base) {
    return update_present && has_patched && !same_as_base;
}

} // namespace FileSys
