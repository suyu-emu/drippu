// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "switch2/switch2.h"

#include <cstdio>

namespace Core::Switch2 {

const char* VersionString() {
    return "suyu Switch 2 / Ounce target: Tegra T239, Horizon OS FW 23.0.1 ABI";
}

int SelfTest() {
    // Exercise the constexpr self-check at run time as well, so a stripped
    // build still proves the constants loaded.
    if (!VerifyT239Abi()) {
        return 1;
    }
    std::printf("%s\n", VersionString());
    std::printf("  SoC: %s (%s, %s), %u cores\n", kSoCName, kCpuCore, kArmArch, kCpuCores);
    std::printf("  Horizon OS FW %s, %llu GiB RAM, %u KiB pages\n", kHorizonFirmware,
                static_cast<unsigned long long>(kPhysicalRamBytes / (1024ULL * 1024 * 1024)),
                kPageSize / 1024);
    std::printf("  GPU: %s\n", kGpuArch);
    std::printf("  Subsystems: %zu (none implemented yet)\n", kSubsystemCount);
    return 0;
}

} // namespace Core::Switch2
