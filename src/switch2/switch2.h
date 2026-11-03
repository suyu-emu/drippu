// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Switch 2 / "Ounce" target support.
//
// This is the seed of a Switch 2 code path in drippu. It is not an emulator
// yet; it is the architecture descriptor, ABI constants and service surface
// that the rest of the work hangs off of, plus a self-check that the constants
// are internally consistent.
//
// Primary reference for the ABI: hffgzktbcc-blip/ounce-core ("Clean-room systems
// research runtime ... for NVIDIA Tegra T239 (Horizon OS FW 23.0.1 ABI)").
// Further reference: 0xNikilite/oboromi, pound-emu/pound, PHENGOX/Switch2Emu.
//
// Nothing here runs a game. The goal of this commit is to make the target
// expressible and buildable so the real work (CPU, memory, HLE services, GPU)
// can be added incrementally without touching the Switch 1 paths.
//
// License note: ounce-core is MIT. Its code is not copied verbatim here - only
// the public ABI constants and the documented service surface, which are
// facts rather than expression.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace Core::Switch2 {

// ---- Silicon ----
// Tegra T239 (Cortex-A78C, ARMv8.5-A). 8 cores, L2 per cluster.
inline constexpr const char* kSoCName = "NVIDIA Tegra T239";
inline constexpr const char* kCpuCore = "Cortex-A78C";
inline constexpr const char* kArmArch = "ARMv8.5-A";
inline constexpr unsigned kCpuCores = 8;

// ---- Horizon OS ----
// Firmware 23.0.1 ABI. Only the ABI version is modelled here; no firmware
// image, no keys, no proprietary data is involved.
inline constexpr const char* kHorizonFirmware = "23.0.1";
inline constexpr unsigned kHorizonFirmwareMajor = 23;
inline constexpr unsigned kHorizonFirmwareMinor = 0;
inline constexpr unsigned kHorizonFirmwarePatch = 1;

// ---- Memory ----
// 4 KB pages, matching the Switch 1 MMU granularity suyu already models, so
// the existing page-table machinery applies unchanged. DRAM timing is not
// modelled at all.
inline constexpr unsigned kPageSize = 4096;
inline constexpr unsigned kPageBits = 12;
inline constexpr std::uint64_t kAddressSpaceBits = 48;

// T239 DRAM. Total physical RAM; the usable split between CPU and GPU is
// decided by the firmware at boot and is not fixed at compile time.
inline constexpr std::uint64_t kPhysicalRamBytes = 12ULL * 1024 * 1024 * 1024;

// ---- Services ----
// Horizon-style service surface modelled so far. These are the names a title
// asks for over IPC; the implementations are stubs until the corresponding
// Switch 1 service is ported to the T239 ABI.
inline constexpr const char* kServiceSm = "sm:";          // service manager
inline constexpr const char* kServiceSetSys = "set:sys";   // system settings
inline constexpr const char* kServiceAm = "am:";           // applet manager
inline constexpr const char* kServiceVi = "vi:";           // display / vi
inline constexpr const char* kServiceHid = "hid:";          // input
inline constexpr const char* kServiceFspSrv = "fsp-srv:";   // file system proxy
inline constexpr const char* kServicePlU = "pl:u";         // placeholder font
inline constexpr const char* kServiceTimeU = "time:u";      // time (user)
inline constexpr const char* kServiceTimeS = "time:s";      // time (system)
inline constexpr const char* kServiceNvDrv = "nvdrv";       // nvidia driver
inline constexpr const char* kServiceRo1 = "ro:1";         // read-only memory

// ---- GPU ----
// T239 GPU: Ampere-derived (SM 8.7 / Orin family). Not Maxwell. The Switch 1
// Maxwell command buffer model does not apply; this is the single largest
// divergence from suyu's existing video_core.
inline constexpr const char* kGpuArch = "Ampere (T239, SM 8.7)";
inline constexpr unsigned kGpuSmMajor = 8;
inline constexpr unsigned kGpuSmMinor = 7;

// ---- Containers ----
// Same container family as Switch 1 (PFS0/NSP, HFS0/XCI, NCA3, NSO0, NRO0,
// NPDM, RomFS). The loader can be reused; only the encryption and the ABI
// header differ.
inline constexpr bool kContainersCompatibleWithSwitch1 = true;

// ---- Status ----
// What is actually implemented at this point. Keep this honest: everything
// below is "not yet".
enum class SubsystemStatus {
    NotStarted,
    Scaffolded,
    InProgress,
    Implemented,
};

struct Subsystem {
    const char* name;
    SubsystemStatus status;
    const char* note;
};

inline constexpr Subsystem kSubsystems[] = {
    {"CPU (AArch64, Cortex-A78C)", SubsystemStatus::NotStarted,
     "Interpreter + JIT from suyu's ARM backend; ARMv8.5-A only adds a few "
     "instructions, but the T239-specific behaviour is unknown."},
    {"Memory (4 KB MMU)", SubsystemStatus::Scaffolded,
     "Page table reuse from Common::PageTable; no DRAM timing model."},
    {"Kernel / SVC", SubsystemStatus::NotStarted, "Horizon OS FW 23.0.1 ABI."},
    {"IPC (CMIF/TIPC)", SubsystemStatus::NotStarted, "Same formats as Switch 1."},
    {"Services", SubsystemStatus::NotStarted, "Surface listed above; stubs only."},
    {"GPU (Ampere)", SubsystemStatus::NotStarted,
     "Largest divergence from Switch 1 (Maxwell). No Maxwell code applies."},
    {"Shader translation", SubsystemStatus::NotStarted, "SASS -> SPIR-V subset."},
    {"Audio", SubsystemStatus::NotStarted, "64-voice mixer reusable."},
    {"Input (HID)", SubsystemStatus::NotStarted, "NPad model reusable."},
    {"Containers", SubsystemStatus::Scaffolded, "Reuse Switch 1 loader."},
    {"Dynamic linking", SubsystemStatus::NotStarted, "MOD0 / PT_TLS as in ounce-core."},
    {"Crypto", SubsystemStatus::NotStarted, "AES-128 only, user-supplied keys."},
};

inline constexpr size_t kSubsystemCount = sizeof(kSubsystems) / sizeof(kSubsystems[0]);

// Self-check: the constants above must be internally consistent. Compiled in
// as a constexpr-evaluable check so a build fails rather than silently drifting.
constexpr bool VerifyT239Abi() {
    if (kPageSize != (1u << kPageBits)) return false;
    if (kCpuCores == 0) return false;
    if (kHorizonFirmwareMajor == 0) return false;
    if (kGpuSmMajor == 0) return false;
    if (kSubsystemCount == 0) return false;
    // Horizon OS FW 23.x is the first Switch 2 firmware line.
    if (kHorizonFirmwareMajor < 23) return false;
    return true;
}
static_assert(VerifyT239Abi(), "Switch 2 ABI constants are inconsistent");

} // namespace Core::Switch2
