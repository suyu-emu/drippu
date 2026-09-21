// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

// The server never tells a client about its own outgoing friend requests (GetFriends() only
// returns incoming ones), so this is purely a local record of friend codes the account has sent
// a request to, kept so the UI can show "request sent" instead of going silent after Add Friend.
namespace Common::NextendoOutgoingRequests {

struct Entry {
    std::string friend_code;
    s64 sent_unix_time = 0;
};

std::vector<Entry> Get();
void Add(const std::string& friend_code);
void Remove(const std::string& friend_code);

// Drops any entry whose friend_code is now in the accepted friends list -- the request resolved,
// it's not pending anymore. Called after every friend-list refresh.
void PruneAccepted(const std::vector<std::string>& current_friend_codes);

} // namespace Common::NextendoOutgoingRequests
