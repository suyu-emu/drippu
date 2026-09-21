// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <ctime>
#include <mutex>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/nextendo_outgoing_requests.h"
#include "common/string_util.h"

namespace Common::NextendoOutgoingRequests {

namespace {

std::mutex g_mutex;
bool g_loaded = false;
std::vector<Entry> g_entries;

std::filesystem::path FilePath() {
    return FS::GetCitronPath(FS::CitronPath::ConfigDir) / "nextendo_outgoing_requests.txt";
}

// Caller holds g_mutex.
void WriteFile() {
    void(FS::CreateParentDirs(FilePath()));
    std::string contents;
    for (const auto& entry : g_entries) {
        contents += fmt::format("{}|{}\n", entry.friend_code, entry.sent_unix_time);
    }
    void(FS::WriteStringToFile(FilePath(), FS::FileType::TextFile, contents));
}

// Caller holds g_mutex.
void EnsureLoaded() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;

    const std::string contents = FS::ReadStringFromFile(FilePath(), FS::FileType::TextFile);
    std::vector<std::string> lines;
    Common::SplitString(contents, '\n', lines);

    for (const auto& line : lines) {
        const auto bar = line.find('|');
        if (bar == std::string::npos || bar == 0) {
            continue;
        }
        Entry entry;
        entry.friend_code = Common::StripSpaces(line.substr(0, bar));
        try {
            entry.sent_unix_time = std::stoll(line.substr(bar + 1));
        } catch (...) {
            entry.sent_unix_time = 0;
        }
        if (!entry.friend_code.empty()) {
            g_entries.push_back(std::move(entry));
        }
    }
}

} // Anonymous namespace

std::vector<Entry> Get() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_entries;
}

void Add(const std::string& friend_code) {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    const auto it = std::find_if(g_entries.begin(), g_entries.end(),
                                 [&](const Entry& e) { return e.friend_code == friend_code; });
    if (it != g_entries.end()) {
        it->sent_unix_time = static_cast<s64>(std::time(nullptr));
    } else {
        g_entries.push_back(Entry{friend_code, static_cast<s64>(std::time(nullptr))});
    }
    WriteFile();
}

void Remove(const std::string& friend_code) {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    std::erase_if(g_entries, [&](const Entry& e) { return e.friend_code == friend_code; });
    WriteFile();
}

void PruneAccepted(const std::vector<std::string>& current_friend_codes) {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    const std::size_t before = g_entries.size();
    std::erase_if(g_entries, [&](const Entry& e) {
        return std::find(current_friend_codes.begin(), current_friend_codes.end(),
                         e.friend_code) != current_friend_codes.end();
    });
    if (g_entries.size() != before) {
        WriteFile();
    }
}

} // namespace Common::NextendoOutgoingRequests
