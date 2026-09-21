// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <utility>

#include "common/common_types.h"

// GameList::UpdateOnlineStatus() already polls the LDN room list every 30s for the tree/grid
// "Online" column; this just mirrors that same result so other views (the carousel) can read it
// without re-polling.
namespace Nextendo::LdnCounts {

struct Stats {
    int players = 0;
    int servers = 0;
};

void Set(const std::map<u64, std::pair<int, int>>& stats);

std::optional<Stats> For(u64 program_id);

} // namespace Nextendo::LdnCounts
