// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"

namespace Service::SSL {

// Splatoon 2's SecureConnection.Register/ReplaceURL and MatchmakeExtension.Create/JoinMatchmakeSessionWithParam
// report the "resolved" station's address as our private LAN IP instead of the external IP
// the NAT-check already gave us (port/natf/natm ARE correct, only the address is stale).
// Rewrites the pre-TLS plaintext in place, fixing up every length field the substitution
// touches. Returns false (leave `input` untouched) for anything that doesn't cleanly match
// the expected shape.
bool TryFixupStationAddress(std::span<const u8> input, std::vector<u8>& output);

} // namespace Service::SSL
