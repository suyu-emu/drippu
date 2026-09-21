// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include "common/nextendo_nat.h"

namespace Common::NextendoNat {

namespace {
std::mutex g_mutex;
std::optional<std::array<u8, 4>> g_external_ip;
} // Anonymous namespace

void SetObservedExternalIp(std::array<u8, 4> ip) {
    std::lock_guard lock{g_mutex};
    g_external_ip = ip;
}

std::optional<std::array<u8, 4>> GetObservedExternalIp() {
    std::lock_guard lock{g_mutex};
    return g_external_ip;
}

} // namespace Common::NextendoNat
