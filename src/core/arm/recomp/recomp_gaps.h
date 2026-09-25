// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Recorded AOT coverage gaps (recomp_gaps.json).
//
// A Hybrid run records where it had to leave recompiled code, so a later export
// of the same title can seed block discovery at those addresses and a player
// can tell whether a strict static export would run at all. The file holds only
// title and build IDs, module-relative offsets, counts and opcode encodings:
// never game code and never a path, so it can be shared between players.
//
// Standard library only: the recompiler smoke tests build this file on its own.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Core::RecompGaps {

inline constexpr std::string_view kSchemaName = "suyu-recomp-gaps";
inline constexpr std::uint64_t kSchemaVersion = 1;

// Bounds. A file past them keeps what it has and says "truncated".
inline constexpr std::size_t kMaxModules = 64;
inline constexpr std::size_t kMaxOffsetsPerModule = 65536;
inline constexpr std::size_t kMaxOpcodes = 4096;
inline constexpr std::size_t kMaxFileBytes = 64u << 20;

struct ModuleGaps {
    std::string name;     ///< informational only; build_id is the key
    std::string build_id; ///< 64 lower-case hex digits
    std::uint64_t hits = 0;
    /// Module-relative offset -> times execution reached it with no block.
    std::map<std::uint64_t, std::uint64_t> offsets;
};

struct GapData {
    std::string title_id; ///< 16 upper-case hex digits, or empty
    std::uint64_t runs = 0;
    std::uint64_t hybrid_runs = 0;
    std::uint64_t strict_runs = 0;
    /// Runs that left recompiled code for no reason at all.
    std::uint64_t clean_runs = 0;
    bool last_run_strict = false;
    bool truncated = false;
    /// Misses at a PC in no module this run knew about.
    std::uint64_t unattributed_misses = 0;
    /// Misses inside a module that has a recompiled image, keyed by build ID.
    std::map<std::string, ModuleGaps> modules;
    /// Modules that executed without any recompiled image, keyed by build ID.
    std::map<std::string, ModuleGaps> no_image;
    /// Guest encoding -> times it forced a fallback.
    std::map<std::uint32_t, std::uint64_t> unimplemented;

    std::uint64_t GapOffsets() const;
    std::uint64_t Misses() const;
    /// Nothing left recompiled code: no miss of any kind and no opcode.
    bool Clean() const;
};

std::string TitleIdHex(std::uint64_t title_id);
/// 64 lower-case hex digits from raw build ID bytes (zero-padded to 32 bytes).
std::string BuildIdHex(const std::uint8_t* bytes, std::size_t size);
/// Lower-cases and zero-pads to 64 digits. Empty for anything that is not hex,
/// too long, or all zero: none of those identifies a module.
std::string NormalizeBuildId(std::string_view text);
/// Exact match after normalization; an invalid ID matches nothing.
bool BuildIdMatches(std::string_view a, std::string_view b);
/// A module name with any directories removed and only [A-Za-z0-9._-] kept.
std::string SanitizeName(std::string_view name);

std::string Serialize(const GapData& data);
std::optional<GapData> Parse(std::string_view json, std::string* error = nullptr);
/// Adds `from` into `into`: run counts and hits add, offsets and opcodes dedupe.
void Merge(GapData& into, const GapData& from);

/// Recorded offsets for the module with exactly this build ID, ascending.
std::vector<std::uint64_t> RootsFor(const GapData& data, std::string_view build_id);
/// Stable over the (build ID, offset) set only, so extra runs or hits do not
/// change it. Empty when there are no offsets.
std::string Fingerprint(const GapData& data);

std::optional<GapData> ReadFile(const std::filesystem::path& path, std::string* error = nullptr);
/// Writes through a temporary file and a rename, so a reader never sees half.
bool WriteFile(const std::filesystem::path& path, const GapData& data,
               std::string* error = nullptr);

/// One run's gaps, classified as they are recorded. Thread-safe; misses are
/// already the slow path, so a mutex and a map lookup cost nothing measurable.
class SessionRecorder {
public:
    void Begin(std::uint64_t title_id, bool strict);
    /// A module the loader placed at [base, base + size).
    void NoteModule(std::uint64_t base, std::uint64_t size, std::string_view name,
                    std::string build_id);
    void ForgetModule(std::uint64_t base);
    /// The module at `base` runs from a recompiled image named `image_name`.
    void NoteImage(std::uint64_t base, std::string_view image_name);
    void RecordMiss(std::uint64_t pc);
    void RecordUnimplemented(std::uint32_t insn);
    /// This run as a one-run GapData.
    GapData Snapshot() const;
    std::uint64_t TitleId() const;
    /// True once, after anything was recorded since the last call.
    bool TakeDirty();

private:
    struct Module {
        std::uint64_t size = 0;
        std::string name;
        std::string build_id;
        bool has_image = false;
    };
    mutable std::mutex lock;
    std::uint64_t title_id = 0;
    bool strict = false;
    bool dirty = true;
    std::map<std::uint64_t, Module> loaded;
    GapData run;
};

} // namespace Core::RecompGaps
