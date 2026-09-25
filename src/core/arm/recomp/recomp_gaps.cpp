// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm/recomp/recomp_gaps.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace Core::RecompGaps {

namespace {

std::uint64_t SatAdd(std::uint64_t a, std::uint64_t b) {
    return a > std::numeric_limits<std::uint64_t>::max() - b
               ? std::numeric_limits<std::uint64_t>::max()
               : a + b;
}

std::string Hex(std::uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(value));
    return buf;
}

bool ParseHex(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : text) {
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    out = v;
    return true;
}

// ---- A small JSON reader, enough for this schema. ----

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text; // string contents, or the number's literal
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* Get(std::string_view key) const {
        for (const auto& [k, v] : object) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(std::string_view in) : s{in} {}

    bool Document(Value& out, std::string& error) {
        if (!Parse(out, 0)) {
            error = err.empty() ? "invalid JSON" : err;
            return false;
        }
        Skip();
        if (pos != s.size()) {
            error = "trailing data after JSON";
            return false;
        }
        return true;
    }

private:
    std::string_view s;
    std::size_t pos = 0;
    std::string err;

    void Skip() {
        while (pos < s.size() &&
               (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
            ++pos;
        }
    }
    bool Fail(const char* what) {
        if (err.empty()) {
            err = std::string{what} + " at byte " + std::to_string(pos);
        }
        return false;
    }
    bool Literal(std::string_view word) {
        if (s.substr(pos, word.size()) != word) {
            return Fail("unexpected token");
        }
        pos += word.size();
        return true;
    }
    bool String(std::string& out) {
        ++pos; // opening quote
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return Fail("control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= s.size()) {
                break;
            }
            const char e = s[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint64_t cp = 0;
                if (pos + 4 > s.size() || !ParseHex(s.substr(pos, 4), cp)) {
                    return Fail("bad \\u escape");
                }
                pos += 4;
                // Nothing in this schema needs more than ASCII; keep the rest
                // recognisable rather than decoding surrogates.
                out += cp < 0x80 ? static_cast<char>(cp) : '?';
                break;
            }
            default:
                return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Number(std::string& out) {
        const std::size_t start = pos;
        if (pos < s.size() && s[pos] == '-') {
            ++pos;
        }
        while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' ||
                                  s[pos] == 'e' || s[pos] == 'E' || s[pos] == '+' ||
                                  s[pos] == '-')) {
            ++pos;
        }
        if (pos == start) {
            return Fail("unexpected character");
        }
        out = std::string{s.substr(start, pos - start)};
        return true;
    }
    bool Parse(Value& v, int depth) {
        if (depth > 16) {
            return Fail("nesting too deep");
        }
        Skip();
        if (pos >= s.size()) {
            return Fail("unexpected end");
        }
        const char c = s[pos];
        if (c == '{') {
            v.type = Value::Type::Object;
            ++pos;
            Skip();
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                Skip();
                if (pos >= s.size() || s[pos] != '"') {
                    return Fail("expected key");
                }
                std::string key;
                if (!String(key)) {
                    return false;
                }
                Skip();
                if (pos >= s.size() || s[pos] != ':') {
                    return Fail("expected ':'");
                }
                ++pos;
                Value item;
                if (!Parse(item, depth + 1)) {
                    return false;
                }
                v.object.emplace_back(std::move(key), std::move(item));
                Skip();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return true;
                }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Type::Array;
            ++pos;
            Skip();
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Value item;
                if (!Parse(item, depth + 1)) {
                    return false;
                }
                v.array.push_back(std::move(item));
                Skip();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return true;
                }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return String(v.text);
        }
        if (c == 't' || c == 'f') {
            v.type = Value::Type::Bool;
            v.boolean = c == 't';
            return Literal(v.boolean ? "true" : "false");
        }
        if (c == 'n') {
            v.type = Value::Type::Null;
            return Literal("null");
        }
        v.type = Value::Type::Number;
        return Number(v.text);
    }
};

/// A non-negative integer, saturating; nullopt for anything else.
std::optional<std::uint64_t> AsCount(const Value* v) {
    if (!v || v->type != Value::Type::Number || v->text.empty() || v->text[0] == '-') {
        return std::nullopt;
    }
    std::uint64_t out = 0;
    for (const char c : v->text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            out = std::numeric_limits<std::uint64_t>::max();
        } else {
            out = out * 10 + digit;
        }
    }
    return out;
}

std::uint64_t CountOr0(const Value* v) {
    return AsCount(v).value_or(0);
}

bool AddOffset(ModuleGaps& m, std::uint64_t offset, std::uint64_t hits, bool& truncated) {
    const auto it = m.offsets.find(offset);
    if (it != m.offsets.end()) {
        it->second = SatAdd(it->second, hits);
        return true;
    }
    if (m.offsets.size() >= kMaxOffsetsPerModule) {
        truncated = true;
        return false;
    }
    m.offsets.emplace(offset, hits);
    return true;
}

ModuleGaps* FindOrAddModule(std::map<std::string, ModuleGaps>& map, const std::string& build_id,
                            std::string_view name, bool& truncated) {
    auto it = map.find(build_id);
    if (it == map.end()) {
        if (map.size() >= kMaxModules) {
            truncated = true;
            return nullptr;
        }
        it = map.emplace(build_id, ModuleGaps{}).first;
        it->second.build_id = build_id;
    }
    if (it->second.name.empty()) {
        it->second.name = SanitizeName(name);
    }
    return &it->second;
}

void AddOpcode(GapData& d, std::uint32_t insn, std::uint64_t hits) {
    const auto it = d.unimplemented.find(insn);
    if (it != d.unimplemented.end()) {
        it->second = SatAdd(it->second, hits);
    } else if (d.unimplemented.size() >= kMaxOpcodes) {
        d.truncated = true;
    } else {
        d.unimplemented.emplace(insn, hits);
    }
}

void MergeModules(std::map<std::string, ModuleGaps>& into,
                  const std::map<std::string, ModuleGaps>& from, bool& truncated) {
    for (const auto& [id, m] : from) {
        ModuleGaps* target = FindOrAddModule(into, id, m.name, truncated);
        if (!target) {
            continue;
        }
        target->hits = SatAdd(target->hits, m.hits);
        for (const auto& [offset, hits] : m.offsets) {
            AddOffset(*target, offset, hits, truncated);
        }
    }
}

void AppendEscaped(std::string& o, std::string_view text) {
    o += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o += ' ';
        } else {
            o += c;
        }
    }
    o += '"';
}

void WriteModules(std::string& o, const char* key, const std::map<std::string, ModuleGaps>& map,
                  bool with_offsets) {
    o += "  \"";
    o += key;
    o += "\": [";
    bool first = true;
    for (const auto& [id, m] : map) {
        o += first ? "\n    {" : ",\n    {";
        first = false;
        o += "\"name\": ";
        AppendEscaped(o, SanitizeName(m.name));
        o += ", \"build_id\": \"" + id + "\", \"hits\": " + std::to_string(m.hits);
        if (with_offsets) {
            o += ", \"offsets\": [";
            bool first_offset = true;
            for (const auto& [offset, hits] : m.offsets) {
                o += first_offset ? "" : ", ";
                first_offset = false;
                o += "[\"" + Hex(offset) + "\", " + std::to_string(hits) + "]";
            }
            o += "]";
        }
        o += "}";
    }
    o += first ? "],\n" : "\n  ],\n";
}

bool ReadModules(const Value* list, std::map<std::string, ModuleGaps>& out, bool with_offsets,
                 bool& truncated, std::string& error) {
    if (!list) {
        return true;
    }
    if (list->type != Value::Type::Array) {
        error = "module list is not an array";
        return false;
    }
    for (const Value& item : list->array) {
        if (item.type != Value::Type::Object) {
            error = "module entry is not an object";
            return false;
        }
        const Value* id = item.Get("build_id");
        const std::string build_id =
            id && id->type == Value::Type::String ? NormalizeBuildId(id->text) : std::string{};
        if (build_id.empty()) {
            // Not an error: an entry nobody can match is simply of no use.
            continue;
        }
        const Value* name = item.Get("name");
        ModuleGaps* m = FindOrAddModule(
            out, build_id,
            name && name->type == Value::Type::String ? std::string_view{name->text}
                                                      : std::string_view{},
            truncated);
        if (!m) {
            continue;
        }
        m->hits = SatAdd(m->hits, CountOr0(item.Get("hits")));
        if (!with_offsets) {
            continue;
        }
        const Value* offsets = item.Get("offsets");
        if (!offsets) {
            continue;
        }
        if (offsets->type != Value::Type::Array) {
            error = "offsets is not an array";
            return false;
        }
        for (const Value& pair : offsets->array) {
            std::uint64_t offset = 0;
            if (pair.type != Value::Type::Array || pair.array.size() != 2 ||
                pair.array[0].type != Value::Type::String ||
                !ParseHex(pair.array[0].text, offset) || !AsCount(&pair.array[1])) {
                error = "bad offset entry";
                return false;
            }
            AddOffset(*m, offset, *AsCount(&pair.array[1]), truncated);
        }
    }
    return true;
}

} // namespace

std::uint64_t GapData::GapOffsets() const {
    std::uint64_t n = 0;
    for (const auto& [id, m] : modules) {
        n += m.offsets.size();
    }
    return n;
}

std::uint64_t GapData::Misses() const {
    std::uint64_t n = unattributed_misses;
    for (const auto& [id, m] : modules) {
        n = SatAdd(n, m.hits);
    }
    for (const auto& [id, m] : no_image) {
        n = SatAdd(n, m.hits);
    }
    return n;
}

bool GapData::Clean() const {
    return Misses() == 0 && modules.empty() && no_image.empty() && unimplemented.empty();
}

std::string TitleIdHex(std::uint64_t title_id) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(title_id));
    return buf;
}

std::string BuildIdHex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::size_t i = 0; i < 32; ++i) {
        const std::uint8_t b = i < size && bytes ? bytes[i] : 0;
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

std::string NormalizeBuildId(std::string_view text) {
    if (text.empty() || text.size() > 64) {
        return {};
    }
    std::string out;
    out.reserve(64);
    bool nonzero = false;
    for (const char c : text) {
        char l = c;
        if (l >= 'A' && l <= 'F') {
            l = static_cast<char>(l - 'A' + 'a');
        }
        if (!((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f'))) {
            return {};
        }
        nonzero |= l != '0';
        out += l;
    }
    if (!nonzero) {
        return {};
    }
    out.append(64 - out.size(), '0');
    return out;
}

bool BuildIdMatches(std::string_view a, std::string_view b) {
    const std::string na = NormalizeBuildId(a);
    return !na.empty() && na == NormalizeBuildId(b);
}

std::string SanitizeName(std::string_view name) {
    const std::size_t slash = name.find_last_of("/\\:");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::string out;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            out += c;
            if (out.size() == 64) {
                break;
            }
        }
    }
    return out;
}

std::string Serialize(const GapData& d) {
    std::string o = "{\n";
    o += "  \"schema\": \"" + std::string{kSchemaName} + "\",\n";
    o += "  \"schema_version\": " + std::to_string(kSchemaVersion) + ",\n";
    o += "  \"title_id\": \"" + SanitizeName(d.title_id) + "\",\n";
    o += "  \"runs\": " + std::to_string(d.runs) + ",\n";
    o += "  \"hybrid_runs\": " + std::to_string(d.hybrid_runs) + ",\n";
    o += "  \"strict_static_runs\": " + std::to_string(d.strict_runs) + ",\n";
    o += "  \"clean_runs\": " + std::to_string(d.clean_runs) + ",\n";
    o += std::string{"  \"last_run_strict_static\": "} + (d.last_run_strict ? "true" : "false") +
         ",\n";
    o += std::string{"  \"truncated\": "} + (d.truncated ? "true" : "false") + ",\n";
    o += "  \"unattributed_misses\": " + std::to_string(d.unattributed_misses) + ",\n";
    WriteModules(o, "modules", d.modules, true);
    WriteModules(o, "modules_without_image", d.no_image, false);
    o += "  \"unimplemented_opcodes\": [";
    bool first = true;
    for (const auto& [insn, hits] : d.unimplemented) {
        char enc[16];
        std::snprintf(enc, sizeof enc, "%08x", insn);
        o += first ? "" : ", ";
        first = false;
        o += "[\"" + std::string{enc} + "\", " + std::to_string(hits) + "]";
    }
    o += "]\n}\n";
    return o;
}

std::optional<GapData> Parse(std::string_view json, std::string* error) {
    std::string local;
    std::string& e = error ? *error : local;
    if (json.size() > kMaxFileBytes) {
        e = "file is too large";
        return std::nullopt;
    }
    Value root;
    if (!Reader{json}.Document(root, e)) {
        return std::nullopt;
    }
    if (root.type != Value::Type::Object) {
        e = "not a JSON object";
        return std::nullopt;
    }
    const Value* schema = root.Get("schema");
    if (!schema || schema->type != Value::Type::String || schema->text != kSchemaName) {
        e = "not a suyu coverage file";
        return std::nullopt;
    }
    const auto version = AsCount(root.Get("schema_version"));
    if (!version || *version == 0) {
        e = "missing schema_version";
        return std::nullopt;
    }
    if (*version > kSchemaVersion) {
        e = "schema_version " + std::to_string(*version) + " is newer than this suyu reads (" +
            std::to_string(kSchemaVersion) + ")";
        return std::nullopt;
    }
    GapData d;
    if (const Value* t = root.Get("title_id"); t && t->type == Value::Type::String) {
        std::uint64_t id = 0;
        if (!t->text.empty() && !ParseHex(t->text, id)) {
            e = "bad title_id";
            return std::nullopt;
        }
        d.title_id = t->text.empty() ? std::string{} : TitleIdHex(id);
    }
    d.runs = CountOr0(root.Get("runs"));
    d.hybrid_runs = CountOr0(root.Get("hybrid_runs"));
    d.strict_runs = CountOr0(root.Get("strict_static_runs"));
    d.clean_runs = CountOr0(root.Get("clean_runs"));
    d.unattributed_misses = CountOr0(root.Get("unattributed_misses"));
    if (const Value* b = root.Get("last_run_strict_static"); b && b->type == Value::Type::Bool) {
        d.last_run_strict = b->boolean;
    }
    if (const Value* b = root.Get("truncated"); b && b->type == Value::Type::Bool) {
        d.truncated = b->boolean;
    }
    if (!ReadModules(root.Get("modules"), d.modules, true, d.truncated, e) ||
        !ReadModules(root.Get("modules_without_image"), d.no_image, false, d.truncated, e)) {
        return std::nullopt;
    }
    if (const Value* ops = root.Get("unimplemented_opcodes")) {
        if (ops->type != Value::Type::Array) {
            e = "unimplemented_opcodes is not an array";
            return std::nullopt;
        }
        for (const Value& pair : ops->array) {
            std::uint64_t insn = 0;
            if (pair.type != Value::Type::Array || pair.array.size() != 2 ||
                pair.array[0].type != Value::Type::String ||
                !ParseHex(pair.array[0].text, insn) || insn > 0xFFFFFFFFull ||
                !AsCount(&pair.array[1])) {
                e = "bad opcode entry";
                return std::nullopt;
            }
            AddOpcode(d, static_cast<std::uint32_t>(insn), *AsCount(&pair.array[1]));
        }
    }
    return d;
}

void Merge(GapData& into, const GapData& from) {
    if (into.title_id.empty()) {
        into.title_id = from.title_id;
    }
    into.runs = SatAdd(into.runs, from.runs);
    into.hybrid_runs = SatAdd(into.hybrid_runs, from.hybrid_runs);
    into.strict_runs = SatAdd(into.strict_runs, from.strict_runs);
    into.clean_runs = SatAdd(into.clean_runs, from.clean_runs);
    into.unattributed_misses = SatAdd(into.unattributed_misses, from.unattributed_misses);
    if (from.runs != 0) {
        into.last_run_strict = from.last_run_strict;
    }
    into.truncated |= from.truncated;
    MergeModules(into.modules, from.modules, into.truncated);
    MergeModules(into.no_image, from.no_image, into.truncated);
    for (const auto& [insn, hits] : from.unimplemented) {
        AddOpcode(into, insn, hits);
    }
}

std::vector<std::uint64_t> RootsFor(const GapData& data, std::string_view build_id) {
    std::vector<std::uint64_t> roots;
    const std::string id = NormalizeBuildId(build_id);
    if (id.empty()) {
        return roots;
    }
    const auto it = data.modules.find(id);
    if (it == data.modules.end()) {
        return roots;
    }
    roots.reserve(it->second.offsets.size());
    for (const auto& [offset, hits] : it->second.offsets) {
        roots.push_back(offset);
    }
    return roots;
}

std::string Fingerprint(const GapData& data) {
    // FNV-1a 64: only has to notice a change, not resist one.
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::string_view bytes) {
        for (const char c : bytes) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ull;
        }
    };
    bool any = false;
    for (const auto& [id, m] : data.modules) {
        for (const auto& [offset, hits] : m.offsets) {
            mix(id);
            mix(":");
            mix(Hex(offset));
            mix(";");
            any = true;
        }
    }
    if (!any) {
        return {};
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::optional<GapData> ReadFile(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) {
            *error = "cannot read file";
        }
        return std::nullopt;
    }
    if (size > kMaxFileBytes) {
        if (error) {
            *error = "file is too large";
        }
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open file";
        }
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return Parse(text, error);
}

bool WriteFile(const std::filesystem::path& path, const GapData& data, std::string* error) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const std::string text = Serialize(data);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            if (error) {
                *error = "cannot write file";
            }
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) {
            *error = "cannot replace file";
        }
        return false;
    }
    return true;
}

// ---- SessionRecorder ----

void SessionRecorder::Begin(std::uint64_t title, bool strict_run) {
    std::scoped_lock lk{lock};
    title_id = title;
    strict = strict_run;
    dirty = true;
    loaded.clear();
    run = GapData{};
}

void SessionRecorder::NoteModule(std::uint64_t base, std::uint64_t size, std::string_view name,
                                 std::string build_id) {
    std::scoped_lock lk{lock};
    // A new module at an address replaces whatever was noted there before.
    for (auto it = loaded.begin(); it != loaded.end();) {
        const bool overlaps = it->first < base + size && base < it->first + it->second.size;
        it = overlaps ? loaded.erase(it) : std::next(it);
    }
    if (loaded.size() >= 256 || size == 0) {
        return;
    }
    loaded[base] = Module{size, SanitizeName(name), NormalizeBuildId(build_id), false};
}

void SessionRecorder::ForgetModule(std::uint64_t base) {
    std::scoped_lock lk{lock};
    loaded.erase(base);
}

void SessionRecorder::NoteImage(std::uint64_t base, std::string_view image_name) {
    std::scoped_lock lk{lock};
    const auto it = loaded.find(base);
    if (it != loaded.end()) {
        it->second.has_image = true;
        if (!image_name.empty()) {
            it->second.name = SanitizeName(image_name);
        }
    }
}

void SessionRecorder::RecordMiss(std::uint64_t pc) {
    std::scoped_lock lk{lock};
    dirty = true;
    auto it = loaded.upper_bound(pc);
    if (it == loaded.begin()) {
        run.unattributed_misses = SatAdd(run.unattributed_misses, 1);
        return;
    }
    --it;
    const Module& m = it->second;
    if (pc - it->first >= m.size || m.build_id.empty()) {
        run.unattributed_misses = SatAdd(run.unattributed_misses, 1);
        return;
    }
    ModuleGaps* target = FindOrAddModule(m.has_image ? run.modules : run.no_image, m.build_id,
                                         m.name, run.truncated);
    if (!target) {
        return;
    }
    target->hits = SatAdd(target->hits, 1);
    if (m.has_image) {
        AddOffset(*target, pc - it->first, 1, run.truncated);
    }
}

void SessionRecorder::RecordUnimplemented(std::uint32_t insn) {
    std::scoped_lock lk{lock};
    dirty = true;
    AddOpcode(run, insn, 1);
}

GapData SessionRecorder::Snapshot() const {
    std::scoped_lock lk{lock};
    GapData d = run;
    d.title_id = title_id ? TitleIdHex(title_id) : std::string{};
    d.runs = 1;
    d.hybrid_runs = strict ? 0 : 1;
    d.strict_runs = strict ? 1 : 0;
    d.last_run_strict = strict;
    d.clean_runs = run.Clean() ? 1 : 0;
    return d;
}

std::uint64_t SessionRecorder::TitleId() const {
    std::scoped_lock lk{lock};
    return title_id;
}

bool SessionRecorder::TakeDirty() {
    std::scoped_lock lk{lock};
    return std::exchange(dirty, false);
}

} // namespace Core::RecompGaps
