// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/nextendo_population_history.h"

#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <QDateTime>
#include <QObject>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "web_service/population_scraper_client.h"

namespace Nextendo::PopulationHistory {

namespace {

std::mutex g_mutex;
std::map<std::string, DayProfile> g_profiles; // lowercase-hex title id -> local-time-rotated hours
std::string g_updated_utc;
bool g_refreshing = false;

std::filesystem::path CachePath() {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) / "nextendo_population.json";
}

int LocalUtcOffsetHours() {
    const int seconds = QDateTime::currentDateTime().offsetFromUtc();
    return static_cast<int>(std::lround(seconds / 3600.0));
}

void ApplyProfiles(const nlohmann::json& json) {
    const auto games = json.find("games");
    if (games == json.end() || !games->is_object()) {
        return;
    }
    const int offset = LocalUtcOffsetHours();

    std::map<std::string, DayProfile> profiles;
    for (const auto& [title_id, entry] : games->items()) {
        const auto hours = entry.find("hours");
        if (hours == entry.end() || !hours->is_array() || hours->size() != 24) {
            continue;
        }
        DayProfile profile{};
        for (int utc_hour = 0; utc_hour < 24; ++utc_hour) {
            const auto& bucket = (*hours)[utc_hour];
            const int local_hour = ((utc_hour + offset) % 24 + 24) % 24;
            profile[local_hour].avg = bucket.value("avg", 0.0);
            profile[local_hour].samples = bucket.value("samples", 0);
        }
        profiles.emplace(title_id, profile);
    }

    const std::string updated = json.value("updated_utc", std::string{});

    std::scoped_lock lock{g_mutex};
    g_profiles = std::move(profiles);
    if (!updated.empty()) {
        g_updated_utc = updated;
    }
}

} // Anonymous namespace

void Start(QObject*) {
    const std::string cached =
        Common::FS::ReadStringFromFile(CachePath(), Common::FS::FileType::TextFile);
    if (cached.empty()) {
        return;
    }
    try {
        ApplyProfiles(nlohmann::json::parse(cached));
    } catch (const nlohmann::json::exception&) {
    }
}

void Refresh() {
    {
        std::scoped_lock lock{g_mutex};
        if (g_refreshing) {
            return;
        }
        g_refreshing = true;
    }

    std::thread{[] {
        const auto body = WebService::FetchNextendoPopulationJson();
        if (body) {
            try {
                ApplyProfiles(nlohmann::json::parse(*body));
                void(Common::FS::CreateParentDirs(CachePath()));
                void(Common::FS::WriteStringToFile(CachePath(), Common::FS::FileType::TextFile,
                                                   *body));
            } catch (const nlohmann::json::exception& e) {
                LOG_WARNING(Frontend, "nextendo-population: unexpected response: {}", e.what());
            }
        }

        std::scoped_lock lock{g_mutex};
        g_refreshing = false;
    }}.detach();
}

std::optional<DayProfile> For(u64 program_id) {
    const std::string key = fmt::format("{:016x}", program_id);
    std::scoped_lock lock{g_mutex};
    const auto it = g_profiles.find(key);
    if (it == g_profiles.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string LastUpdatedUtc() {
    std::scoped_lock lock{g_mutex};
    return g_updated_utc;
}

} // namespace Nextendo::PopulationHistory
