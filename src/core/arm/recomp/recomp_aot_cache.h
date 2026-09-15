// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/arm/recomp/recomp_image_abi.h"

namespace suyu::recomp {

/// Bump when emitted C / runtime contract changes in a way that stale caches
/// must not be reused (independent of the on-disk image ABI version).
inline constexpr uint32_t kRecompEmitterRevision = 1;

/// Manifest schema written by the exporter after this identity-aware reuse
/// policy. Older manifests without emitter_revision / module build IDs are
/// treated as non-reusable.
inline constexpr uint32_t kRecompAotManifestVersion = 3;

struct AotCacheModuleIdentity {
    std::string name;
    std::string build_id_hex; // 64 lowercase/uppercase hex chars
};

struct AotCacheReuseRequest {
    bool full_scan = false;
    std::string_view effective_backend = "dynarmic";
    uint32_t emitter_revision = kRecompEmitterRevision;
    uint32_t abi_version = kRecompImageAbiVersion;
    bool wants_compiled = false;
    bool has_recompiled_project = false;
    bool has_required_launcher = false;
    /// Identities of the *currently selected* ExeFS / title modules. Empty
    /// means the caller could not establish live content — reuse is refused.
    std::vector<AotCacheModuleIdentity> current_modules;
};

enum class AotCacheReject : uint8_t {
    Ok = 0,
    MissingManifest,
    ManifestParse,
    ManifestVersion,
    FullScan,
    Backend,
    EmitterRevision,
    AbiVersion,
    MissingProject,
    MissingLauncher,
    MissingRequiredIdentity,
    ModuleCount,
    ModuleIdentity,
};

inline const char* AotCacheRejectName(AotCacheReject r) {
    switch (r) {
    case AotCacheReject::Ok:
        return "ok";
    case AotCacheReject::MissingManifest:
        return "missing aot_manifest.json";
    case AotCacheReject::ManifestParse:
        return "manifest parse failure";
    case AotCacheReject::ManifestVersion:
        return "manifest version mismatch";
    case AotCacheReject::FullScan:
        return "full_scan mismatch";
    case AotCacheReject::Backend:
        return "effective_backend mismatch";
    case AotCacheReject::EmitterRevision:
        return "emitter_revision mismatch";
    case AotCacheReject::AbiVersion:
        return "abi_version mismatch";
    case AotCacheReject::MissingProject:
        return "missing recompiled project";
    case AotCacheReject::MissingLauncher:
        return "missing compiled launcher";
    case AotCacheReject::MissingRequiredIdentity:
        return "current module identities unavailable";
    case AotCacheReject::ModuleCount:
        return "module count mismatch";
    case AotCacheReject::ModuleIdentity:
        return "module build_id/name mismatch";
    }
    return "unknown";
}

struct AotCacheReuseResult {
    AotCacheReject reason = AotCacheReject::MissingManifest;
    /// Short stable token for logs/tests (often the reject name, sometimes a
    /// more specific field such as a module name).
    std::string detail;

    bool ok() const {
        return reason == AotCacheReject::Ok;
    }
};

namespace detail {

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline std::string_view SkipWs(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) {
        s.remove_prefix(1);
    }
    return s;
}

inline bool Consume(std::string_view& s, std::string_view lit) {
    s = SkipWs(s);
    if (s.size() < lit.size() || s.substr(0, lit.size()) != lit) {
        return false;
    }
    s.remove_prefix(lit.size());
    return true;
}

inline bool ExtractJsonStringAfterKey(std::string_view json, std::string_view key,
                                      std::string& out) {
    const std::string pattern = std::string("\"") + std::string(key) + "\"";
    const size_t at = json.find(pattern);
    if (at == std::string_view::npos) {
        return false;
    }
    std::string_view rest = json.substr(at + pattern.size());
    if (!Consume(rest, ":")) {
        return false;
    }
    rest = SkipWs(rest);
    if (rest.empty() || rest.front() != '"') {
        return false;
    }
    rest.remove_prefix(1);
    std::string value;
    while (!rest.empty() && rest.front() != '"') {
        if (rest.front() == '\\' && rest.size() >= 2) {
            value.push_back(rest[1]);
            rest.remove_prefix(2);
            continue;
        }
        value.push_back(rest.front());
        rest.remove_prefix(1);
    }
    if (rest.empty()) {
        return false;
    }
    out = std::move(value);
    return true;
}

inline bool ExtractJsonBoolAfterKey(std::string_view json, std::string_view key, bool& out) {
    const std::string pattern = std::string("\"") + std::string(key) + "\"";
    const size_t at = json.find(pattern);
    if (at == std::string_view::npos) {
        return false;
    }
    std::string_view rest = json.substr(at + pattern.size());
    if (!Consume(rest, ":")) {
        return false;
    }
    rest = SkipWs(rest);
    if (rest.size() >= 4 && rest.substr(0, 4) == "true") {
        out = true;
        return true;
    }
    if (rest.size() >= 5 && rest.substr(0, 5) == "false") {
        out = false;
        return true;
    }
    return false;
}

inline bool ExtractJsonUintAfterKey(std::string_view json, std::string_view key, uint32_t& out) {
    const std::string pattern = std::string("\"") + std::string(key) + "\"";
    const size_t at = json.find(pattern);
    if (at == std::string_view::npos) {
        return false;
    }
    std::string_view rest = json.substr(at + pattern.size());
    if (!Consume(rest, ":")) {
        return false;
    }
    rest = SkipWs(rest);
    if (rest.empty() || (rest.front() < '0' || rest.front() > '9')) {
        return false;
    }
    uint32_t value = 0;
    while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') {
        value = value * 10u + static_cast<uint32_t>(rest.front() - '0');
        rest.remove_prefix(1);
    }
    out = value;
    return true;
}

inline bool HexEqualsInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto norm = [](char c) -> char {
            if (c >= 'A' && c <= 'F') {
                return static_cast<char>(c - 'A' + 'a');
            }
            return c;
        };
        if (norm(a[i]) != norm(b[i])) {
            return false;
        }
    }
    return true;
}

inline bool ParseModulesArray(std::string_view json, std::vector<AotCacheModuleIdentity>& out) {
    const size_t key = json.find("\"modules\"");
    if (key == std::string_view::npos) {
        return false;
    }
    std::string_view rest = json.substr(key);
    const size_t arr = rest.find('[');
    if (arr == std::string_view::npos) {
        return false;
    }
    rest.remove_prefix(arr + 1);
    out.clear();
    while (true) {
        rest = SkipWs(rest);
        if (rest.empty()) {
            return false;
        }
        if (rest.front() == ']') {
            return true;
        }
        const size_t obj = rest.find('{');
        if (obj == std::string_view::npos) {
            return false;
        }
        rest.remove_prefix(obj);
        const size_t end = rest.find('}');
        if (end == std::string_view::npos) {
            return false;
        }
        const std::string_view obj_json = rest.substr(0, end + 1);
        AotCacheModuleIdentity mod;
        if (!ExtractJsonStringAfterKey(obj_json, "name", mod.name) ||
            !ExtractJsonStringAfterKey(obj_json, "build_id", mod.build_id_hex) || mod.name.empty() ||
            mod.build_id_hex.size() != kRecompBuildIdSize * 2) {
            return false;
        }
        out.push_back(std::move(mod));
        rest.remove_prefix(end + 1);
        rest = SkipWs(rest);
        if (!rest.empty() && rest.front() == ',') {
            rest.remove_prefix(1);
        }
    }
}

} // namespace detail

/// Decide whether an existing AOT cache may be reused for the current export
/// request. Never returns Ok when current module identities are missing, or
/// when the manifest lacks per-module build IDs / emitter revision.
inline AotCacheReuseResult EvaluateAotCacheReuse(std::string_view manifest_json,
                                                 const AotCacheReuseRequest& req) {
    AotCacheReuseResult result;
    if (manifest_json.empty()) {
        result.reason = AotCacheReject::MissingManifest;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }
    if (req.current_modules.empty()) {
        result.reason = AotCacheReject::MissingRequiredIdentity;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }
    for (const auto& mod : req.current_modules) {
        if (mod.name.empty() || mod.build_id_hex.size() != kRecompBuildIdSize * 2) {
            result.reason = AotCacheReject::MissingRequiredIdentity;
            result.detail = mod.name.empty() ? "unnamed module" : mod.name;
            return result;
        }
    }

    uint32_t version = 0;
    if (!detail::ExtractJsonUintAfterKey(manifest_json, "version", version)) {
        result.reason = AotCacheReject::ManifestParse;
        result.detail = "version";
        return result;
    }
    if (version < kRecompAotManifestVersion) {
        result.reason = AotCacheReject::ManifestVersion;
        result.detail = "version";
        return result;
    }

    bool full_scan = false;
    if (!detail::ExtractJsonBoolAfterKey(manifest_json, "full_scan", full_scan) ||
        full_scan != req.full_scan) {
        result.reason = AotCacheReject::FullScan;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }

    std::string backend;
    if (!detail::ExtractJsonStringAfterKey(manifest_json, "effective_backend", backend) ||
        backend != req.effective_backend) {
        result.reason = AotCacheReject::Backend;
        result.detail = backend.empty() ? "effective_backend" : backend;
        return result;
    }

    uint32_t emitter = 0;
    if (!detail::ExtractJsonUintAfterKey(manifest_json, "emitter_revision", emitter) ||
        emitter != req.emitter_revision) {
        result.reason = AotCacheReject::EmitterRevision;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }

    uint32_t abi = 0;
    if (!detail::ExtractJsonUintAfterKey(manifest_json, "abi_version", abi) ||
        abi != req.abi_version) {
        result.reason = AotCacheReject::AbiVersion;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }

    if (!req.has_recompiled_project) {
        result.reason = AotCacheReject::MissingProject;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }
    if (req.wants_compiled && !req.has_required_launcher) {
        result.reason = AotCacheReject::MissingLauncher;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }

    std::vector<AotCacheModuleIdentity> cached;
    if (!detail::ParseModulesArray(manifest_json, cached)) {
        result.reason = AotCacheReject::ManifestParse;
        result.detail = "modules";
        return result;
    }
    if (cached.size() != req.current_modules.size()) {
        result.reason = AotCacheReject::ModuleCount;
        result.detail = AotCacheRejectName(result.reason);
        return result;
    }

    for (const auto& want : req.current_modules) {
        bool found = false;
        for (const auto& have : cached) {
            if (have.name != want.name) {
                continue;
            }
            found = true;
            if (!detail::HexEqualsInsensitive(have.build_id_hex, want.build_id_hex)) {
                result.reason = AotCacheReject::ModuleIdentity;
                result.detail = want.name;
                return result;
            }
            break;
        }
        if (!found) {
            result.reason = AotCacheReject::ModuleIdentity;
            result.detail = want.name;
            return result;
        }
    }

    result.reason = AotCacheReject::Ok;
    result.detail = AotCacheRejectName(result.reason);
    return result;
}

/// Read an NSO0 header's build_id. Returns false when the buffer is not a
/// plausible NSO (too small / bad magic).
inline bool ReadNsoBuildId(const void* data, size_t size, uint8_t out[kRecompBuildIdSize]) {
    if (!data || !out || size < 0x60) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    // 'N''S''O''0' little-endian
    if (!(bytes[0] == 'N' && bytes[1] == 'S' && bytes[2] == 'O' && bytes[3] == '0')) {
        return false;
    }
    std::memcpy(out, bytes + 0x40, kRecompBuildIdSize);
    return true;
}

inline std::string BuildIdToHexLower(const uint8_t id[kRecompBuildIdSize]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.resize(kRecompBuildIdSize * 2);
    for (uint32_t i = 0; i < kRecompBuildIdSize; ++i) {
        hex[i * 2] = kHex[(id[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kHex[id[i] & 0xF];
    }
    return hex;
}

} // namespace suyu::recomp
