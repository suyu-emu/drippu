// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>

#include "common/common_types.h"

class QObject;

namespace Nextendo::PopulationHistory {

struct HourBucket {
    double avg = 0.0;
    int samples = 0;
};

using DayProfile = std::array<HourBucket, 24>;

void Start(QObject* parent);
void Refresh();

std::optional<DayProfile> For(u64 program_id);
std::string LastUpdatedUtc();

} // namespace Nextendo::PopulationHistory
