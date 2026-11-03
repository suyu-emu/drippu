// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2012 Gekko Emulator
// SPDX-FileContributor: ShizZy <shizzy247@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Copyright (C) 2005-2012 Gekko Emulator
 *
 * @file    common_types.h
 * @author  ShizZy <shizzy247@gmail.com>
 * @date    2012-02-11
 * @brief   Common types used throughout the project
 *
 * @section LICENSE
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * http://www.gnu.org/copyleft/gpl.html
 *
 * Official project repository can be found at:
 * http://code.google.com/p/gekko-gc-emu/
 */

#pragma once

#include <array>
#include <cstdint>

using u8 = std::uint8_t;   ///< 8-bit unsigned byte
using u16 = std::uint16_t; ///< 16-bit unsigned short
using u32 = std::uint32_t; ///< 32-bit unsigned word
using u64 = std::uint64_t; ///< 64-bit unsigned int

using s8 = std::int8_t;   ///< 8-bit signed byte
using s16 = std::int16_t; ///< 16-bit signed short
using s32 = std::int32_t; ///< 32-bit signed word
using s64 = std::int64_t; ///< 64-bit signed int

using f32 = float;  ///< 32-bit floating point
using f64 = double; ///< 64-bit floating point

using VAddr = u64;    ///< Represents a pointer in the userspace virtual address space.
using DAddr = u64;    ///< Represents a pointer in the device specific virtual address space.
using PAddr = u64;    ///< Represents a pointer in the ARM11 physical address space.
using GPUVAddr = u64; ///< Represents a pointer in the GPU virtual address space.

using u128 = std::array<std::uint64_t, 2>;
static_assert(sizeof(u128) == 16, "u128 must be 128 bits wide");

// -----------------------------------------------------------------------
// Switch 2 / "Ounce" target support (Tegra T239, Horizon OS FW 23.0.1).
//
// Seeded from four clean-room Switch 2 research repositories:
//   hffgzktbcc-blip/ounce-core  (Tegra T239, Horizon OS FW 23.0.1 ABI)
//   0xNikilite/oboromi          (proof-of-concept Switch 2 emulator)
//   pound-emu/pound             (Switch 1 + 2 emulator)
//   PHENGOX/Switch2Emu          (experimental C++20 Switch 2 emulator)
//
// These are architecture descriptors and ABI constants only. Nothing here
// runs a game; they exist so the rest of suyu (CPU, memory, loader, GPU)
// can grow a Switch 2 code path incrementally. The GPU is Ampere (SM 8.7),
// which is the single largest divergence from Switch 1 (Maxwell) and is
// why the existing video_core cannot be reused as-is.
// -----------------------------------------------------------------------
namespace Switch2 {
// Silicon
inline constexpr const char* kSoCName = "NVIDIA Tegra T239";
inline constexpr const char* kCpuCore = "Cortex-A78C";
inline constexpr const char* kArmArch = "ARMv8.5-A";
inline constexpr unsigned kCpuCores = 8;

// Horizon OS
inline constexpr const char* kHorizonFirmware = "23.0.1";
inline constexpr unsigned kHorizonFirmwareMajor = 23;

// Memory
inline constexpr unsigned kPageSize = 4096;
inline constexpr unsigned kPageBits = 12;
inline constexpr std::uint64_t kPhysicalRamBytes = 12ULL * 1024 * 1024 * 1024;

// GPU
inline constexpr const char* kGpuArch = "Ampere (T239, SM 8.7)";
inline constexpr unsigned kGpuSmMajor = 8;
inline constexpr unsigned kGpuSmMinor = 7;

// Containers (same family as Switch 1; loader reusable)
inline constexpr bool kContainersCompatibleWithSwitch1 = true;

// Self-check
constexpr bool VerifyT239Abi() {
    if (kPageSize != (1u << kPageBits)) return false;
    if (kCpuCores == 0) return false;
    if (kHorizonFirmwareMajor < 23) return false;
    if (kGpuSmMajor == 0) return false;
    return true;
}
static_assert(VerifyT239Abi(), "Switch 2 ABI constants are inconsistent");
} // namespace Switch2
