// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace suyu::recomp {

inline constexpr uint32_t kRecompImageAbiVersion = 1;
inline constexpr uint32_t kRecompRegsPrefixSize = 872;
inline constexpr uint32_t kRecompMaxModules = 16;
inline constexpr uint32_t kRecompBuildIdSize = 32;
inline constexpr uint32_t kRecompModuleNameSize = 32;

inline constexpr const char* kRecompLoadOrder[] = {
    "rtld",    "main",    "subsdk0", "subsdk1", "subsdk2", "subsdk3",
    "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9",
    "sdk",
};

struct RecompImageAbi {
    uint32_t abi_version;
    uint32_t abi_size;
    uint32_t context_size;
    uint32_t regs_prefix_size;
    uint32_t module_index;
    uint8_t build_id[kRecompBuildIdSize];
    char module_name[kRecompModuleNameSize];
};

struct RecompImageIdentity {
    uint32_t module_index = 0;
    uint8_t build_id[kRecompBuildIdSize]{};
};

using RecompImageBlockFn = void (*)(void*);
using RecompImageLookupFn = RecompImageBlockFn (*)(uint64_t);
using RecompImageSetBaseFn = void (*)(uint64_t);
using RecompImageAbiFn = const RecompImageAbi* (*)();

struct RecompImageExports {
    RecompImageLookupFn lookup = nullptr;
    RecompImageSetBaseFn set_base = nullptr;
    RecompImageAbiFn abi = nullptr;
};

struct ImageExpect {
    const uint8_t* build_id = nullptr;
    /// When true, a missing expect.build_id is itself a reject — callers must
    /// supply live guest/ExeFS identities rather than silently skipping the
    /// check because an adjacent JSON manifest omitted the entry.
    bool require_build_id = false;
};

enum class ImageReject {
    Ok = 0,
    MissingLookup,
    MissingSetBase,
    MissingAbi,
    AbiVersion,
    AbiSize,
    RegsPrefix,
    EmptyName,
    BuildId,
    MissingExpectBuildId,
    DuplicateIndex,
    IndexRange,
};

struct RecompModuleSlot {
    RecompImageLookupFn lookup = nullptr;
    RecompImageSetBaseFn set_base = nullptr;
    uint64_t base = 0;
    const RecompImageAbi* abi = nullptr;
};

struct RecompModuleMap {
    RecompModuleSlot slots[kRecompMaxModules]{};
    uint32_t count = 0;
};

inline int ModuleIndexForName(std::string_view name) {
    for (uint32_t i = 0; i < sizeof(kRecompLoadOrder) / sizeof(kRecompLoadOrder[0]); ++i) {
        if (name == kRecompLoadOrder[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline uint32_t ModuleIndexOrUnknown(std::string_view name) {
    const int named = ModuleIndexForName(name);
    return named >= 0 ? static_cast<uint32_t>(named) : kRecompMaxModules;
}

inline const char* WithoutNnPrefix(const char* name) {
    if (!name) {
        return name;
    }
    if (std::strncmp(name, "nn", 2) == 0 || std::strncmp(name, "NN", 2) == 0) {
        return name + 2;
    }
    return name;
}

inline bool ParseBuildIdHex(std::string_view hex, uint8_t out[kRecompBuildIdSize]) {
    if (hex.size() != kRecompBuildIdSize * 2) {
        return false;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (uint32_t i = 0; i < kRecompBuildIdSize; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

inline bool ModuleNameEqual(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*a++)));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(*b++)));
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

inline const char* ImageRejectName(ImageReject r) {
    switch (r) {
    case ImageReject::Ok:
        return "ok";
    case ImageReject::MissingLookup:
        return "missing recomp_image_lookup";
    case ImageReject::MissingSetBase:
        return "missing recomp_image_set_base";
    case ImageReject::MissingAbi:
        return "missing recomp_image_abi";
    case ImageReject::AbiVersion:
        return "ABI version mismatch";
    case ImageReject::AbiSize:
        return "ABI size mismatch";
    case ImageReject::RegsPrefix:
        return "GuestContext prefix size mismatch";
    case ImageReject::EmptyName:
        return "empty module name";
    case ImageReject::BuildId:
        return "content hash mismatch";
    case ImageReject::MissingExpectBuildId:
        return "required live module build_id missing";
    case ImageReject::DuplicateIndex:
        return "duplicate module index";
    case ImageReject::IndexRange:
        return "module index out of range";
    }
    return "unknown";
}

inline ImageReject ValidateImageExports(const RecompImageExports& ex,
                                        const ImageExpect& expect = {}) {
    if (!ex.lookup) {
        return ImageReject::MissingLookup;
    }
    if (!ex.set_base) {
        return ImageReject::MissingSetBase;
    }
    if (!ex.abi) {
        return ImageReject::MissingAbi;
    }
    const RecompImageAbi* abi = ex.abi();
    if (!abi) {
        return ImageReject::MissingAbi;
    }
    if (abi->abi_version != kRecompImageAbiVersion) {
        return ImageReject::AbiVersion;
    }
    if (abi->abi_size != sizeof(RecompImageAbi)) {
        return ImageReject::AbiSize;
    }
    if (abi->regs_prefix_size != kRecompRegsPrefixSize) {
        return ImageReject::RegsPrefix;
    }
    if (abi->module_name[0] == '\0') {
        return ImageReject::EmptyName;
    }
    if (expect.require_build_id && !expect.build_id) {
        return ImageReject::MissingExpectBuildId;
    }
    if (expect.build_id &&
        std::memcmp(abi->build_id, expect.build_id, kRecompBuildIdSize) != 0) {
        return ImageReject::BuildId;
    }
    return ImageReject::Ok;
}

inline ImageReject PlaceLoadedModule(RecompModuleMap& map, const RecompImageExports& ex,
                                     const ImageExpect& expect = {}) {
    const ImageReject rejected = ValidateImageExports(ex, expect);
    if (rejected != ImageReject::Ok) {
        return rejected;
    }
    const RecompImageAbi* abi = ex.abi();
    if (abi->module_index >= kRecompMaxModules) {
        return ImageReject::IndexRange;
    }
    RecompModuleSlot& slot = map.slots[abi->module_index];
    if (slot.lookup || slot.set_base || slot.abi) {
        return ImageReject::DuplicateIndex;
    }
    slot.lookup = ex.lookup;
    slot.set_base = ex.set_base;
    slot.abi = abi;
    if (abi->module_index + 1 > map.count) {
        map.count = abi->module_index + 1;
    }
    return ImageReject::Ok;
}

inline const RecompModuleSlot* SlotByName(const RecompModuleMap& map, const char* name) {
    if (!name || !name[0]) {
        return nullptr;
    }
    const char* stripped = WithoutNnPrefix(name);
    for (uint32_t i = 0; i < kRecompMaxModules; ++i) {
        const RecompImageAbi* abi = map.slots[i].abi;
        if (!abi) {
            continue;
        }
        if (ModuleNameEqual(abi->module_name, name) || ModuleNameEqual(abi->module_name, stripped)) {
            return &map.slots[i];
        }
    }
    return nullptr;
}

inline const RecompModuleSlot* SlotByBuildId(const RecompModuleMap& map,
                                            const uint8_t build_id[kRecompBuildIdSize]) {
    if (!build_id) {
        return nullptr;
    }
    bool any = false;
    for (uint32_t i = 0; i < kRecompBuildIdSize; ++i) {
        if (build_id[i] != 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        return nullptr;
    }
    for (uint32_t i = 0; i < kRecompMaxModules; ++i) {
        const RecompImageAbi* abi = map.slots[i].abi;
        if (!abi) {
            continue;
        }
        if (std::memcmp(abi->build_id, build_id, kRecompBuildIdSize) == 0) {
            return &map.slots[i];
        }
    }
    return nullptr;
}

/// Resolve a loaded image by verified identity. Build ID wins when provided and
/// unique; otherwise the guest/export module name (with optional nn- prefix).
/// Dense runtime indices are never consulted here — those are load-order
/// counters and need not match canonical ABI ordinals (rtld=0 … sdk=12).
inline const RecompModuleSlot* SlotByIdentity(const RecompModuleMap& map, const char* name,
                                             const uint8_t* build_id = nullptr) {
    if (build_id) {
        if (const RecompModuleSlot* by_id = SlotByBuildId(map, build_id)) {
            return by_id;
        }
    }
    return SlotByName(map, name);
}

inline bool SlotNameMatches(const RecompModuleSlot& slot, const char* name) {
    if (!name || !name[0] || !slot.abi) {
        return false;
    }
    const char* stripped = WithoutNnPrefix(name);
    return ModuleNameEqual(slot.abi->module_name, name) ||
           ModuleNameEqual(slot.abi->module_name, stripped);
}

/// Assign a guest module base to the matching exported image.
///
/// Match by name / build ID first. Only fall back to the dense runtime index
/// when that slot is empty or already belongs to the same identity — never
/// overwrite a differently named image that happens to occupy the same
/// ordinal (e.g. sparse rtld,main,subsdk1,sdk must not put SDK's base on
/// ABI slot 3 / subsdk1).
inline void ApplyModuleBase(RecompModuleMap& map, size_t index, const char* name, uint64_t base,
                            const uint8_t* build_id = nullptr) {
    if (const RecompModuleSlot* found = SlotByIdentity(map, name, build_id)) {
        auto& slot = map.slots[static_cast<size_t>(found - map.slots)];
        if (slot.set_base) {
            slot.base = base;
            slot.set_base(base);
        }
        return;
    }
    if (index >= kRecompMaxModules || !map.slots[index].set_base) {
        return;
    }
    const RecompModuleSlot& slot = map.slots[index];
    // Index fallback: only when the slot has no recorded identity yet, or the
    // provided name is empty / already matches. A named guest module must not
    // claim a differently named ABI slot via dense position alone.
    if (slot.abi && slot.abi->module_name[0] != '\0' && name && name[0] &&
        !SlotNameMatches(slot, name)) {
        return;
    }
    map.slots[index].base = base;
    map.slots[index].set_base(base);
}

}
