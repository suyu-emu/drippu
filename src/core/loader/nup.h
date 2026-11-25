// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Loader {

enum class NupKind : u8 {
    Base,
    Update,
    AddOn,
    AddOnUpdate,
    System,
};

enum class NupMetadata : u16 {
    Npdm,
    Nacp,
    Icon,
    DisplayVersion,
    TitleName,
};

struct NupPackage {
    u64 title_id{};
    u32 version{};
    NupKind kind{};
    bool standalone_update{};
    FileSys::VirtualDir root;
    FileSys::VirtualDir exefs;
    FileSys::VirtualDir romfs;
    std::map<NupMetadata, FileSys::VirtualFile> metadata;
};

// Match the signature even if the package is damaged or has an unsupported version.
bool HasNupSignature(const FileSys::VirtualFile& file);

// Validate the v1 header, TOC digest, and tree, then expose member files through the VFS.
std::optional<std::vector<NupPackage>> ReadNup(const FileSys::VirtualFile& file);

} // namespace Loader
