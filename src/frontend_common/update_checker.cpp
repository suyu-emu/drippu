// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef NIGHTLY_BUILD
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#endif

#include <fmt/format.h>
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "update_checker.h"

#include "common/logging.h"

namespace {

// The tag identifying "what we already run", in the same terms a release tag
// uses. Split out so the multi-source selection below compares every source by
// the same rule the single-source check always used.
std::optional<std::string> CurrentReleaseTag(const std::string& release_tag) {
#ifdef NIGHTLY_BUILD
    std::vector<std::string> result;

    boost::split(result, release_tag, boost::is_any_of("."));
    if (result.size() != 2)
        return std::nullopt;

    const std::string tag = result[1];

    boost::split(result, std::string{Common::g_build_version}, boost::is_any_of("-"));
    if (result.empty())
        return std::nullopt;

    const std::string build = result[0];
#else
    const std::string tag = release_tag;
    const std::string build = Common::g_build_version;
#endif
    if (tag != build) {
        return tag;
    }
    return std::nullopt;
}

} // namespace

bool UpdateChecker::IsDifferentFromBuild(const std::string& release_tag) {
    return CurrentReleaseTag(release_tag).has_value();
}

std::vector<UpdateChecker::SourcedRelease> UpdateChecker::GetLatestPerSource() {
    std::vector<SourcedRelease> out;
    for (const auto& source : Common::Net::ResolveUpdateSources()) {
        auto latest = Common::Net::GetLatestReleaseFrom(source);
        if (!latest) {
            continue;
        }
        LOG_INFO(Frontend, "Received update {} from {}", latest->title, source.name);
        out.push_back(SourcedRelease{
            .release = std::move(latest.value()),
            .source_name = source.name,
        });
    }
    return out;
}

std::optional<UpdateChecker::SourcedRelease> UpdateChecker::GetBestUpdate() {
    std::optional<SourcedRelease> best;
    for (auto& candidate : GetLatestPerSource()) {
        if (!CurrentReleaseTag(candidate.release.tag)) {
            continue;
        }
        if (!best || candidate.release.published > best->release.published) {
            best = std::move(candidate);
        }
    }
    return best;
}

std::optional<Common::Net::Release> UpdateChecker::GetUpdate() {
    // Same rule as before, now over every configured source with one fetch per
    // source: the primary source is first in the resolved list and keeps its
    // historical priority on ties (strict greater-than below).
    if (auto best = GetBestUpdate()) {
        return std::move(best->release);
    }
    return std::nullopt;
}
