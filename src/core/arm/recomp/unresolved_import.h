// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace suyu::recomp {

using u32 = uint32_t;
using u64 = uint64_t;

constexpr u64 kUnresolvedImportTrap = 0xFFFF'FFFF'0000'0000ULL;

enum class UnresolvedReloc : u32 {
    Abs64 = 0x101,
    GlobDat = 0x401,
    JumpSlot = 0x402,
    Irelative = 0x408,
};

struct UnresolvedImport {
    std::string name;
    u64 module_base = 0;
    u64 offset = 0;
    UnresolvedReloc kind = UnresolvedReloc::JumpSlot;
};

inline const char* UnresolvedRelocName(UnresolvedReloc kind) {
    switch (kind) {
    case UnresolvedReloc::Abs64:
        return "R_AARCH64_ABS64";
    case UnresolvedReloc::GlobDat:
        return "R_AARCH64_GLOB_DAT";
    case UnresolvedReloc::JumpSlot:
        return "R_AARCH64_JUMP_SLOT";
    case UnresolvedReloc::Irelative:
        return "R_AARCH64_IRELATIVE";
    }
    return "R_AARCH64_UNKNOWN";
}

inline u64 UnresolvedSlotTarget() {
    return kUnresolvedImportTrap;
}

// An absent ELF STB_WEAK symbol is an optional import, not a broken import.
// Its symbol value is zero; ABS64 callers still apply their relocation addend.
// Call only after looking for a definition in the loaded modules.
inline std::optional<u64> ResolveUndefinedWeakSymbol(uint8_t symbol_info, u64 addend) {
    constexpr uint8_t kStbWeak = 2;
    if ((symbol_info >> 4) == kStbWeak) {
        return addend;
    }
    return std::nullopt;
}

inline bool IsUnresolvedImportTrap(u64 pc) {
    return pc == kUnresolvedImportTrap;
}

enum class UnresolvedTrapAction {
    Halt,
};

struct UnresolvedTrapResult {
    UnresolvedTrapAction action = UnresolvedTrapAction::Halt;
    u64 x0 = 0;
    u64 pc = 0;
    std::string diagnostic;
};

inline std::string FormatUnresolvedImportDiagnostic(
    std::string_view name, u64 module_base, u64 offset, UnresolvedReloc kind) {
    std::string out = "recomp: unresolved import ";
    out += UnresolvedRelocName(kind);
    out += " '";
    out += name.empty() ? "<no name>" : std::string(name);
    out += "' module_base=0x";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(module_base));
    out += buf;
    out += " offset=0x";
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(offset));
    out += buf;
    return out;
}

inline std::string FormatUnresolvedTrapDiagnostic(u64 lr,
                                                 const std::vector<UnresolvedImport>& recorded) {
    std::string out = "recomp: called through unresolved import (lr=0x";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(lr));
    out += buf;
    out += ")";
    if (recorded.empty()) {
        out += " no reloc records";
        return out;
    }
    out += " pending:";
    const size_t n = recorded.size() < 8 ? recorded.size() : 8;
    for (size_t i = 0; i < n; ++i) {
        out += " [";
        out += FormatUnresolvedImportDiagnostic(recorded[i].name, recorded[i].module_base,
                                                recorded[i].offset, recorded[i].kind);
        out += "]";
    }
    if (recorded.size() > n) {
        out += " ...";
    }
    return out;
}

inline UnresolvedTrapResult TakeUnresolvedImportTrap(u64 x0, u64 lr,
                                                     const std::vector<UnresolvedImport>& recorded) {
    UnresolvedTrapResult r;
    r.action = UnresolvedTrapAction::Halt;
    r.x0 = x0;
    r.pc = kUnresolvedImportTrap;
    r.diagnostic = FormatUnresolvedTrapDiagnostic(lr, recorded);
    return r;
}

}
