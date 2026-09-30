// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <optional>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>

#include <fmt/format.h>
#include "common/scm_rev.h"
#include "common/settings.h"
#include "net.h"

#include "common/logging.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#define QT_TR_NOOP(x) x

namespace Common::Net {

std::vector<Asset> Release::GetPlatformAssets() const {
    // TODO(crueter): Need better handling for this as a whole.
#ifdef NIGHTLY_BUILD
    std::vector<std::string> result;
    boost::algorithm::split(result, tag, boost::is_any_of("."));
    if (result.size() != 2)
        return {};
    const auto ref = result.at(1);
#else
    const auto ref = tag;
#endif

    std::vector<Asset> found_assets;

    // FIXME: This is mildly inefficient.
    // Finds assets based on a hierarchy of regex search strings.
    const auto find_asset = [&found_assets, this](const std::string& name,
                                                       const std::vector<std::string>& suffixes) {
        for (const std::string& asset : assets) {
            for (const auto& suffix : suffixes) {
                if (asset.ends_with(suffix)) {
                    const std::string_view asset_sv = asset;
                    const size_t pos = asset_sv.find_last_of('/');
                    const std::string_view filename =
                        (pos != std::string_view::npos) ? asset_sv.substr(pos + 1) : asset_sv;

                    found_assets.emplace_back(Asset{
                        .name = name,
                        .url = host,
                        .path = asset,
                        .filename = std::string{filename},
                    });
                    return;
                }
            }
        }
    };

#ifdef _WIN32
#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
    find_asset("Standard", {"amd64-msvc-standard.exe", "amd64-msvc-standard.zip"});
#else // _MSC_VER
    find_asset("Standard", {BUILD_ID "-gcc-standard.exe", BUILD_ID "-gcc-standard.zip"});
    find_asset("PGO", {BUILD_ID "-clang-pgo.exe", BUILD_ID "-clang-pgo.zip"});
#endif // _MSC_VER
#elif defined(ARCHITECTURE_arm64)
    find_asset("Standard", {"arm64-clang-standard.exe", "arm64-clang-standard.zip"});
    find_asset("PGO", {"arm64-clang-pgo.exe", "arm64-clang-pgo.zip"});
#endif // ARCHITECTURE_arm64
#elif defined(__APPLE__)
#ifdef ARCHITECTURE_arm64
    find_asset("Standard", {"standard.dmg", "standard.tar.gz", ".dmg", ".tar.gz"});
    find_asset("PGO", {"pgo.dmg", "pgo.tar.gz"});
#endif // ARCHITECTURE_arm64
#elif defined(__ANDROID__)
#ifdef ARCHITECTURE_x86_64
    find_asset("Standard", {"chromeos.apk"});
#elif defined(ARCHITECTURE_arm64)
#ifdef YUZU_LEGACY
    find_asset("Standard", {"legacy.apk"});
#elif defined(GENSHIN_SPOOF)
    find_asset("Standard", {"optimized.apk"});
#else
    find_asset("Standard", {"standard.apk"});
#endif // GENSHIN_SPOOF
#endif // ARCHITECTURE_arm64
#endif // __APPLE__
    (void)find_asset;
    return found_assets;
}

static inline u64 ParseIsoTimestamp(const std::string& iso) {
    if (iso.empty())
        return 0;

    std::string buf = iso;
    if (buf.back() == 'Z')
        buf.pop_back();

    std::tm tm{};
    std::istringstream ss(buf);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail())
        return 0;

#ifdef _WIN32
    return static_cast<u64>(_mkgmtime(&tm));
#else
    return static_cast<u64>(timegm(&tm));
#endif
}

std::optional<Release> Release::FromJson(const nlohmann::json& json, const std::string& host,
                                         const std::string& repo,
                                         const std::string& website) {
    Release rel;
    if (!json.is_object())
        return std::nullopt;

    rel.tag = json.value("tag_name", std::string{});
    if (rel.tag.empty())
        return std::nullopt;

    rel.title = json.value("name", rel.tag);
    rel.id = json.value("id", std::hash<std::string>{}(rel.title));

    rel.published = ParseIsoTimestamp(json.value("published_at", std::string{}));
    rel.prerelease = json.value("prerelease", false);

    auto body = json.value("body", rel.title);
    boost::replace_all(body, "\\r", "");
    boost::replace_all(body, "\\n", "\n");
    rel.body = body;

    rel.host = host;

    // Custom sources carry their own website (a fork's Forgejo instance, say);
    // without one the compiled-in site is the only sensible fallback.
    const std::string& site =
        website.empty() ? Common::g_build_auto_update_website : website;
    const auto release_base = fmt::format("{}/{}/releases", site, repo);
    const auto fallback_html = fmt::format("{}/tag/{}", release_base, rel.tag);
    rel.html_url = json.value("html_url", fallback_html);

    // This is our own "fake" API.
    if (json.contains("base")) {
        const auto base = json.value("base", fmt::format("https://{}", Common::g_build_auto_update_api));
        rel.base_download_url = fmt::format("{}/{}", base, rel.tag);

        // Assets are easy :)
        rel.assets = json.value("assets", std::vector<std::string>{});
    } else {
        const auto base_download_url = fmt::format("/{}/releases/download/{}", repo, rel.tag);

        rel.base_download_url = base_download_url;

        // assets are a bit more complex here. :(
        std::vector<std::string> assets;
        const nlohmann::json& arr = json["assets"];
        for (const auto &obj : arr) {
            const auto url = obj.value("browser_download_url", std::string{});
            assets.emplace_back(url);
        }

        rel.assets = assets;
    }

    return rel;
}

std::optional<Release> Release::FromJson(const std::string_view& json, const std::string& host,
                                         const std::string& repo,
                                         const std::string& website) {
    try {
        return FromJson(nlohmann::json::parse(json), host, repo, website);
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return {};
}

std::vector<Release> Release::ListFromJson(const nlohmann::json& json, const std::string& host,
                                           const std::string& repo,
                                           const std::string& website) {
    if (!json.is_array())
        return {};

    std::vector<Release> releases;
    for (const auto& obj : json) {
        auto rel = Release::FromJson(obj, host, repo, website);
        if (rel)
            releases.emplace_back(rel.value());
    }
    return releases;
}

std::vector<Release> Release::ListFromJson(const std::string_view& json, const std::string& host,
                                           const std::string& repo,
                                           const std::string& website) {
    try {
        return ListFromJson(nlohmann::json::parse(json), host, repo, website);
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return {};
}

std::optional<std::string> MakeRequest(const std::string& url, const std::string& path) {
    try {
        constexpr std::size_t timeout_seconds = 15;

        std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(url);
        client->set_connection_timeout(timeout_seconds);
        client->set_read_timeout(timeout_seconds);
        client->set_write_timeout(timeout_seconds);

#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

        if (client == nullptr) {
            LOG_ERROR(Common, "Invalid URL {}{}", url, path);
            return {};
        }

        httplib::Request request{
            .method = "GET",
            .path = path,
        };

        client->set_follow_location(true);
        httplib::Result result = client->send(request);

        if (!result) {
            LOG_ERROR(Common, "GET to {}{} returned null", url, path);
            return {};
        }

        const auto& response = result.value();
        if (response.status >= 400) {
            LOG_ERROR(Common, "GET to {}{} returned error status code: {}", url, path,
                      response.status);
            return {};
        }
        if (!response.headers.contains("content-type")) {
            LOG_ERROR(Common, "GET to {}{} returned no content", url, path);
            return {};
        }

        return response.body;
    } catch (std::exception& e) {
        LOG_ERROR(Common, "GET to {}{} failed during update check: {}", url, path, e.what());
        return std::nullopt;
    }
}

std::vector<Release> GetReleases() {
    const auto body = GetReleasesBody();

    if (!body) {
        LOG_WARNING(Common, "Failed to get stable releases");
        return {};
    }

    const std::string_view body_str = body.value();
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_stable_api);
    return Release::ListFromJson(body_str, url, Common::g_build_auto_update_stable_repo);
}

std::optional<Release> GetLatestRelease() {
    const auto releases_path =  Common::g_build_auto_update_api_path;
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_api);

    const auto body = MakeRequest(url, releases_path);
    if (!body) {
        LOG_WARNING(Common, "Failed to get latest release");
        return std::nullopt;
    }

    const std::string_view body_str = body.value();
    return Release::FromJson(body_str, url, Common::g_build_auto_update_repo);
}

std::optional<std::string> GetReleasesBody() {
    const auto releases_path =
        fmt::format("/{}/{}/releases", Common::g_build_auto_update_stable_api_path,
                    Common::g_build_auto_update_stable_repo);
    const auto url = fmt::format("https://{}", Common::g_build_auto_update_stable_api);

    return MakeRequest(url, releases_path);
}

UpdateSource UpdateSource::Default() {
    return UpdateSource{
        .name = "Built-in",
        .api_host = Common::g_build_auto_update_api,
        .api_path = Common::g_build_auto_update_api_path,
        .repo = Common::g_build_auto_update_repo,
        .website = Common::g_build_auto_update_website,
    };
}

UpdateSource UpdateSource::FromRepoSlug(const std::string& repo_slug, const std::string& name) {
    return UpdateSource{
        .name = name.empty() ? repo_slug : name,
        .api_host = "api.github.com",
        .api_path = fmt::format("/repos/{}/releases/latest", repo_slug),
        .repo = repo_slug,
        .website = "https://github.com",
    };
}

std::string UpdateSource::Serialize() const {
    return fmt::format("{}|{}|{}|{}|{}", name, api_host, api_path, repo, website);
}

std::optional<UpdateSource> UpdateSource::Deserialize(const std::string& text) {
    // No escaping: '|' separates fields and newlines separate entries, so a
    // field containing either is malformed input, not data to preserve.
    if (text.find_first_of("\r\n") != std::string::npos) {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    std::string rest = text;
    for (int i = 0; i < 4; ++i) {
        const auto pos = rest.find('|');
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        parts.emplace_back(rest.substr(0, pos));
        rest = rest.substr(pos + 1);
    }
    parts.emplace_back(std::move(rest));
    // A fifth pipe means six fields, not five with an escaped pipe.
    if (parts.size() != 5 || parts[4].find('|') != std::string::npos) {
        return std::nullopt;
    }
    UpdateSource out{
        .name = parts[0],
        .api_host = parts[1],
        .api_path = parts[2],
        .repo = parts[3],
        .website = parts[4],
    };
    // An entry that cannot be requested is not a source. Name and website may
    // be empty (they default below); the request fields may not.
    if (out.api_host.empty() || out.api_path.empty() || out.repo.empty()) {
        return std::nullopt;
    }
    if (out.name.empty()) {
        out.name = out.repo;
    }
    return out;
}

std::optional<Release> GetLatestReleaseFrom(const UpdateSource& source) {
    if (source.api_host.empty() || source.api_path.empty() || source.repo.empty()) {
        return std::nullopt;
    }
    const auto url = fmt::format("https://{}", source.api_host);
    const auto body = MakeRequest(url, source.api_path);
    if (!body) {
        LOG_WARNING(Common, "Failed to get latest release from {}", source.name);
        return std::nullopt;
    }
    return Release::FromJson(std::string_view{body.value()}, url, source.repo, source.website);
}

std::vector<UpdateSource> ParseExtraSources(const std::string& text) {
    std::vector<UpdateSource> out;
    std::string rest = text;
    while (!rest.empty()) {
        auto pos = rest.find_first_of("\r\n");
        std::string line = (pos == std::string::npos) ? rest : rest.substr(0, pos);
        rest = (pos == std::string::npos) ? std::string{} : rest.substr(pos + 1);
        // Trim surrounding whitespace; a line that is empty afterwards is a
        // separator, not an error.
        const auto not_space = [](char c) { return c != ' ' && c != '\t'; };
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), not_space));
        line.erase(std::find_if(line.rbegin(), line.rend(), not_space).base(), line.end());
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (auto parsed = UpdateSource::Deserialize(line)) {
            out.emplace_back(std::move(parsed.value()));
        } else {
            LOG_WARNING(Common, "Ignoring malformed update source entry");
        }
    }
    return out;
}

std::vector<UpdateSource> ResolveUpdateSources() {
    UpdateSource primary = UpdateSource::Default();
    const std::string override_repo = Settings::values.update_repo_override.GetValue();
    const std::string override_host = Settings::values.update_api_host.GetValue();
    const std::string override_path = Settings::values.update_api_path.GetValue();
    const std::string override_site = Settings::values.update_website.GetValue();
    // Per-field merge: setting just the repo keeps the GitHub API host/path,
    // which is the common fork case. Anything left empty falls back to the
    // compiled-in default for that field.
    if (!override_repo.empty()) {
        primary.repo = override_repo;
        primary.name = "Custom";
    }
    if (!override_host.empty()) {
        primary.api_host = override_host;
    }
    if (!override_path.empty()) {
        primary.api_path = override_path;
    }
    if (!override_site.empty()) {
        primary.website = override_site;
    }

    std::vector<UpdateSource> out;
    const auto key_of = [](const UpdateSource& s) {
        return s.api_host + '\x1F' + s.api_path + '\x1F' + s.repo;
    };
    std::vector<std::string> seen;
    const auto push_unique = [&](UpdateSource s) {
        if (s.api_host.empty() || s.api_path.empty() || s.repo.empty()) {
            return;
        }
        const auto key = key_of(s);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            return;
        }
        seen.emplace_back(key);
        out.emplace_back(std::move(s));
    };
    push_unique(std::move(primary));
    for (auto& extra :
         ParseExtraSources(Settings::values.update_extra_sources.GetValue())) {
        push_unique(std::move(extra));
    }
    return out;
}

} // namespace Common::Net
