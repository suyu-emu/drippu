// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <string>
#include <vector>
#include "common/net/net.h"

namespace UpdateChecker {

std::optional<Common::Net::Release> GetUpdate();

/// A release together with the source it came from, so the UI can say *where*
/// an update was found - which matters once forks and mirrors are in play.
struct SourcedRelease {
    Common::Net::Release release;
    std::string source_name;
};

/// Newest available release across all resolved update sources that is not
/// what this build already is. Nullopt when everything is up to date or when
/// no source answered.
std::optional<SourcedRelease> GetBestUpdate();

/// Latest release per source, including sources that report what we already
/// run (useful for the updater UI's per-source status list).
std::vector<SourcedRelease> GetLatestPerSource();

/// True when a release tag names something other than this build (nightly
///-aware: compares the channel-qualified tag, exactly as GetUpdate always has).
bool IsDifferentFromBuild(const std::string& release_tag);

} // namespace UpdateChecker
