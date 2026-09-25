// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace suyu::recomp {

inline bool ModuleNameEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Bind a live NSO to its generated image. Static bundles may omit rtld or a
// subsdk, so indexing the compact image vector by the live load index shifts
// every later module onto the wrong base. Names handle sdk and other named
// modules; the canonical index handles a main named after its game or an
// arbitrary subsdk name.
template <typename NameAt>
int FindRecompModuleForLive(size_t count, NameAt name_at, size_t live_index,
                            std::string_view live_name) {
    constexpr std::string_view order[] = {
        "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
        "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk",
    };
    const auto find = [&](std::string_view name) {
        if (name.empty()) {
            return -1;
        }
        for (size_t i = 0; i < count; ++i) {
            if (ModuleNameEquals(name_at(i), name)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    if (int found = find(live_name); found >= 0) {
        return found;
    }
    if (live_name.size() > 2 &&
        std::tolower(static_cast<unsigned char>(live_name[0])) == 'n' &&
        std::tolower(static_cast<unsigned char>(live_name[1])) == 'n') {
        if (int found = find(live_name.substr(2)); found >= 0) {
            return found;
        }
    }
    return live_index < sizeof(order) / sizeof(order[0]) ? find(order[live_index]) : -1;
}

} // namespace suyu::recomp
