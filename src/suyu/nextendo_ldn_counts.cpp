// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/nextendo_ldn_counts.h"

#include <mutex>

namespace Nextendo::LdnCounts {

namespace {
std::mutex g_mutex;
std::map<u64, std::pair<int, int>> g_stats;
} // namespace

void Set(const std::map<u64, std::pair<int, int>>& stats) {
    std::scoped_lock lock{g_mutex};
    g_stats = stats;
}

std::optional<Stats> For(u64 program_id) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_stats.find(program_id);
    if (it == g_stats.end()) {
        return std::nullopt;
    }
    return Stats{.players = it->second.first, .servers = it->second.second};
}

} // namespace Nextendo::LdnCounts
