// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_settings.h"

#include "common/logging.h"
#include "common/settings.h"

namespace Core::GameSettings {

void LoadOverrides(std::uint64_t program_id) {
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::NinjaGaidenRagebound:
            Settings::values.use_squashed_iterated_blend = true;
            break;
        default:
            break;
    }

    LOG_INFO(Core, "Applied game settings for title ID {:016X}", program_id);
}

} // namespace Core::GameSettings
