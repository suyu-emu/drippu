// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include "common/common_types.h"

// This console's external IPv4, as observed from the nncs NAT-check reply. Used by
// ssl/nextendo_nat_rewrite.h to fix up outgoing station URLs.
namespace Common::NextendoNat {

void SetObservedExternalIp(std::array<u8, 4> ip);
std::optional<std::array<u8, 4>> GetObservedExternalIp();

} // namespace Common::NextendoNat
