// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace Core::GameSettings {

enum class TitleID : std::uint64_t {
    NinjaGaidenRagebound = 0x0100781020710000ULL
};

void LoadOverrides(std::uint64_t program_id);

} // namespace Core::GameSettings
