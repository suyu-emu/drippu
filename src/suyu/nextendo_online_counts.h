// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

class QObject;

// Polls GET /api/online-counts every 5s; caches for the game list delegate to read on repaint.
namespace Nextendo::OnlineCounts {

// parent should outlive every For() call. Safe to call more than once.
void Start(QObject* parent);

int For(u64 program_id);

} // namespace Nextendo::OnlineCounts
