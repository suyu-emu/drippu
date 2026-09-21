// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "web_service/mk8d_country_flags.h"

#include <array>
#include <filesystem>

#include <httplib.h>

#include "common/logging.h"

namespace WebService::Mk8dCountryFlags {

namespace {

void ApplyCaCertPath(httplib::Client& client) {
#ifdef __linux__
    static constexpr std::array<const char*, 4> candidates{
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL/CentOS
        "/etc/ssl/cert.pem",                  // Alpine
        "/etc/ssl/ca-bundle.pem",             // openSUSE
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            client.set_ca_cert_path(path);
            return;
        }
    }
#else
    (void)client;
#endif
}

} // namespace

CountryFetchResult FetchCountryFlagPatch(const std::string& country_code) {
    CountryFetchResult result{};
    result.release_page_url = MK8D_REPO_URL;

    httplib::Client client{"https://raw.githubusercontent.com"};
    client.set_connection_timeout(15);
    client.set_read_timeout(15);
    client.set_follow_location(true);
    ApplyCaCertPath(client);

    const std::string path = "/alyeri/nextendo-mk8d-country-flags/main/Consoles/Atmosphere/" +
                             country_code + "/atmosphere/exefs_patches/Nextendo%20Country%20" +
                             country_code + "/" + MK8D_BUILD_ID + ".ips";

    const auto res = client.Get(path, httplib::Headers{{"User-Agent", "citron"}});
    if (!res) {
        result.error = "network error contacting GitHub";
        LOG_ERROR(WebService, "MK8D country flag fetch ({}): failed, httplib error={}",
                  country_code, httplib::to_string(res.error()));
        return result;
    }
    if (res->status != 200) {
        result.error = "download returned HTTP " + std::to_string(res->status) +
                       " (country code may not be supported)";
        LOG_ERROR(WebService, "MK8D country flag fetch ({}): {}", country_code, result.error);
        return result;
    }

    result.success = true;
    result.data.assign(res->body.begin(), res->body.end());
    return result;
}

} // namespace WebService::Mk8dCountryFlags
