// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Common::Net {

typedef struct {
    std::string name;
    std::string url;
    std::string path;
    std::string filename;
} Asset;

typedef struct Release {
    std::string title;
    std::string body;
    std::string tag;
    std::string base_download_url;
    std::string html_url;
    std::string host;

    std::vector<std::string> assets;

    u64 id;
    u64 published;
    bool prerelease;

    // Get the relevant list of assets for the current platform.
    std::vector<Asset> GetPlatformAssets() const;

    static std::optional<Release> FromJson(const nlohmann::json& json, const std::string &host, const std::string& repo, const std::string &website = {});
    static std::optional<Release> FromJson(const std::string_view& json, const std::string &host, const std::string& repo, const std::string &website = {});
    static std::vector<Release> ListFromJson(const nlohmann::json &json, const std::string &host, const std::string &repo, const std::string &website = {});
    static std::vector<Release> ListFromJson(const std::string_view &json, const std::string &host, const std::string &repo, const std::string &website = {});
} Release;

// Make a request via httplib, and return the response body if applicable.
std::optional<std::string> MakeRequest(const std::string &url, const std::string &path);

// Get all of the latest stable releases.
std::vector<Release> GetReleases();

// Get all of the latest stable releases as text.
std::optional<std::string> GetReleasesBody();

// Get the latest release of the current channel.
std::optional<Release> GetLatestRelease();

/// One place the updater can fetch releases from.
///
/// The compiled-in default points at this project's own releases (see
/// CMakeModules/GenerateSCMRev.cmake), but a source is just five strings, so a
/// fork, a mirror, or a whole new home for drippu is equally describable -
/// including hosts that are not GitHub at all, as long as they answer the same
/// two endpoints (Forgejo and Gitea both do).
///
/// Two API shapes are understood, selected per response (see Release::FromJson):
/// - GitHub-style: GET https://<api_host><api_path> returns one release object
///   with browser_download_url assets.
/// - the project's own "fake" API: the object carries a `base` key plus an
///   `assets` list of paths.
struct UpdateSource {
    /// Human label shown in the updater UI. "Built-in" for the default.
    std::string name;
    /// Bare host, no scheme: "api.github.com".
    std::string api_host;
    /// Latest-release endpoint path: "/repos/<owner>/<repo>/releases/latest".
    std::string api_path;
    /// "owner/repo", used for asset URL construction and display.
    std::string repo;
    /// Release website for the "open in browser" fallback, with scheme:
    /// "https://github.com".
    std::string website;

    bool operator==(const UpdateSource &) const = default;

    /// True when every field is empty - i.e. "not configured". Such a source is
    /// skipped rather than requested, so an all-empty build (or a cleared
    /// override) degrades to "no updates" instead of a request to "https://".
    [[nodiscard]] bool Empty() const {
        return name.empty() && api_host.empty() && api_path.empty() && repo.empty() &&
               website.empty();
    }

    /// The compiled-in default, from g_build_auto_update_*.
    static UpdateSource Default();

    /// Shorthand for a GitHub-hosted repo: fills the API host/path/website for
    /// "owner/repo". This is the common case for forks.
    static UpdateSource FromRepoSlug(const std::string &repo_slug, const std::string &name = {});

    /// "name|api_host|api_path|repo|website". '|' and newlines are not permitted
    /// in fields; Deserialize rejects entries containing them.
    [[nodiscard]] std::string Serialize() const;
    static std::optional<UpdateSource> Deserialize(const std::string &text);
};

/// Latest release from one explicit source. Empty sources are skipped and yield
/// nullopt without touching the network.
std::optional<Release> GetLatestReleaseFrom(const UpdateSource &source);

/// Effective source list: the user's override when fully specified, else the
/// compiled-in default, followed by each well-formed entry of
/// Settings::values.update_extra_sources (one serialized source per line),
/// deduplicated by (api_host, api_path, repo).
std::vector<UpdateSource> ResolveUpdateSources();

/// Parses a multi-line extra-sources setting into sources, skipping blank lines
/// and malformed entries.
std::vector<UpdateSource> ParseExtraSources(const std::string &text);

}
