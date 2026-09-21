// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace WebService::Mk8dCountryFlags {

constexpr u64 MK8D_TITLE_ID = 0x0100152000022000ULL;

constexpr bool IsMk8dTitleId(u64 program_id) {
    return program_id == MK8D_TITLE_ID;
}

// The patch is a fixed exefs IPS keyed to this exact build (game version 3.0.5); it silently
// won't apply against any other build.
constexpr const char* MK8D_REQUIRED_VERSION = "3.0.5";
constexpr const char* MK8D_BUILD_ID = "FE941ED5BA14BE5D505698DA1BBF4FE7";
constexpr const char* MK8D_REPO_URL = "https://github.com/alyeri/nextendo-mk8d-country-flags";

struct CountryInfo {
    const char* code;
    const char* name;
};

// The 110 country codes present in MK8D 3.0.5's internal country table.
constexpr std::array<CountryInfo, 110> kSupportedCountries{{
    {"AE", "United Arab Emirates"}, {"AL", "Albania"},
    {"AO", "Angola"},               {"AR", "Argentina"},
    {"AT", "Austria"},              {"AU", "Australia"},
    {"AW", "Aruba"},                {"AZ", "Azerbaijan"},
    {"BA", "Bosnia and Herzegovina"}, {"BB", "Barbados"},
    {"BE", "Belgium"},              {"BG", "Bulgaria"},
    {"BN", "Brunei"},               {"BO", "Bolivia"},
    {"BR", "Brazil"},               {"BS", "Bahamas"},
    {"BW", "Botswana"},             {"BY", "Belarus"},
    {"BZ", "Belize"},               {"CA", "Canada"},
    {"CH", "Switzerland"},          {"CL", "Chile"},
    {"CO", "Colombia"},             {"CY", "Cyprus"},
    {"CZ", "Czechia"},              {"DE", "Germany"},
    {"DK", "Denmark"},              {"DO", "Dominican Republic"},
    {"EC", "Ecuador"},              {"EE", "Estonia"},
    {"EG", "Egypt"},                {"ES", "Spain"},
    {"FI", "Finland"},              {"FR", "France"},
    {"GB", "United Kingdom"},       {"GH", "Ghana"},
    {"GR", "Greece"},               {"GT", "Guatemala"},
    {"HN", "Honduras"},             {"HR", "Croatia"},
    {"HU", "Hungary"},              {"ID", "Indonesia"},
    {"IE", "Ireland"},              {"IL", "Israel"},
    {"IN", "India"},                {"IS", "Iceland"},
    {"IT", "Italy"},                {"JM", "Jamaica"},
    {"JO", "Jordan"},               {"JP", "Japan"},
    {"KR", "South Korea"},          {"KW", "Kuwait"},
    {"KZ", "Kazakhstan"},           {"LB", "Lebanon"},
    {"LT", "Lithuania"},            {"LU", "Luxembourg"},
    {"LV", "Latvia"},               {"MA", "Morocco"},
    {"MD", "Moldova"},              {"MG", "Madagascar"},
    {"MK", "North Macedonia"},      {"ML", "Mali"},
    {"MN", "Mongolia"},             {"MR", "Mauritania"},
    {"MT", "Malta"},                {"MU", "Mauritius"},
    {"MX", "Mexico"},               {"MY", "Malaysia"},
    {"MZ", "Mozambique"},           {"NA", "Namibia"},
    {"NG", "Nigeria"},              {"NI", "Nicaragua"},
    {"NL", "Netherlands"},          {"NO", "Norway"},
    {"NZ", "New Zealand"},          {"OM", "Oman"},
    {"PA", "Panama"},               {"PE", "Peru"},
    {"PG", "Papua New Guinea"},     {"PH", "Philippines"},
    {"PK", "Pakistan"},             {"PL", "Poland"},
    {"PT", "Portugal"},             {"PY", "Paraguay"},
    {"QA", "Qatar"},                {"RO", "Romania"},
    {"RS", "Serbia"},               {"RU", "Russia"},
    {"RW", "Rwanda"},               {"SC", "Seychelles"},
    {"SE", "Sweden"},               {"SG", "Singapore"},
    {"SI", "Slovenia"},             {"SK", "Slovakia"},
    {"SR", "Suriname"},             {"SV", "El Salvador"},
    {"SZ", "Eswatini"},             {"TD", "Chad"},
    {"TH", "Thailand"},             {"TN", "Tunisia"},
    {"TR", "Turkiye"},              {"TT", "Trinidad and Tobago"},
    {"TZ", "Tanzania"},             {"UA", "Ukraine"},
    {"UG", "Uganda"},               {"US", "United States"},
    {"VE", "Venezuela"},            {"VN", "Vietnam"},
    {"ZM", "Zambia"},               {"ZW", "Zimbabwe"},
}};

struct CountryFetchResult {
    bool success = false;
    std::vector<u8> data;
    std::string error;
    std::string release_page_url; // For a manual-download link/instructions on failure.
};

// Fetches "<country_code>.ips" straight from the repo's raw content (no releases API needed --
// the patch files live directly in the repo). Never throws; failure is recorded in the result.
CountryFetchResult FetchCountryFlagPatch(const std::string& country_code);

} // namespace WebService::Mk8dCountryFlags
