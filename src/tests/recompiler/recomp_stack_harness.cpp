// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Real-stack homebrew integration harness: ArmRecomp (process ArmInterface via
// SetRecompLookup) + in-tree Dynarmic fallback + PhysicalCore::LoadContext TLS.
//
// AOT blocks are Translate()'d from real guest encodings and compiled into a
// shared library (SUYU_HOSTED_RECOMP helpers). Matching AArch64 bytes are
// written into guest RX so Dynarmic fallback executes the same sites.
//
// Does not call Svc::Call / PhysicalCore::RunThread (fixture SVC imms are not
// safe live HLE). Does not load copyrighted titles or keys.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "common/common_types.h"
#include "common/settings.h"
#include "common/typed_address.h"
#include "core/arm/arm_interface.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/arm/recomp/recomp_image_abi.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/hardware_properties.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory_types.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "core/recompiler/arm64_to_c.h"
#include "smoke_config.h"

namespace fs = std::filesystem;
using suyu::recomp::u32;
using suyu::recomp::u64;

namespace {

int g_fails = 0;

void Fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << std::endl;
    ++g_fails;
}

void Pass(const std::string& msg) {
    std::cout << "PASS: " << msg << std::endl;
}

void ExpectEq(const char* name, u64 got, u64 want) {
    if (got != want) {
        Fail(std::string(name) + ": got=" + std::to_string(got) + " want=" + std::to_string(want));
    } else {
        Pass(std::string(name) + "=" + std::to_string(got));
    }
}

void ExpectTrue(const char* name, bool cond) {
    if (!cond) {
        Fail(std::string(name) + " was false");
    } else {
        Pass(name);
    }
}

void ScenarioPass(const char* name, int fails_before) {
    if (g_fails == fails_before) {
        Pass(name);
    }
}

#ifndef _WIN32
std::string Quote(const std::string& s) {
    return "'" + s + "'";
}
#endif

int RunArgs(const std::vector<std::string>& args) {
    if (args.empty()) {
        Fail("empty command");
        return 1;
    }
#ifndef _WIN32
    std::ostringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) {
            cmd << ' ';
        }
        cmd << Quote(args[i]);
    }
    std::cout << "+ " << cmd.str() << std::endl;
    return std::system(cmd.str().c_str());
#else
    Fail("Windows nested AOT build not wired in this harness");
    return 1;
#endif
}

bool WriteFile(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        Fail("write " + path.string());
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string TranslateInsn(u32 insn, u64 pc) {
    std::string body;
    bool unhandled = false;
    suyu::recomp::Translate(insn, pc, body, &unhandled);
    return body;
}

// Guest layout (module-relative). Entry is 0x80000000 with aslr_offset=0.
constexpr u64 kExpectedEntry = 0x80000000ULL;
constexpr u64 kOffTlsSvc = 0x0000;
constexpr u64 kOffUnhandled = 0x0800;
constexpr u64 kOffUnhandledPark = 0x0808; // clears in_fallback after Dynarmic SVC
constexpr u64 kOffMiss = 0x1000;          // registered AOT; force-miss target
constexpr u64 kOffMissPark = 0x1008;
constexpr u64 kOffPlain = 0x1800; // icache / Translate MOVZ #7
constexpr u64 kOffCrossPage = 0x1FFC;
constexpr u64 kCodeBytes = 3 * Kernel::PageSize;
constexpr u64 kImageBytes = 4 * Kernel::PageSize;

// AArch64 encodings (also written into guest RX).
constexpr u32 kMovzX0_1234 = 0xD2824680u;
constexpr u32 kMsrTpidrX0 = 0xD51BD040u;
constexpr u32 kMrsX1Tpidr = 0xD53BD041u;
constexpr u32 kMrsX3Tpidrro = 0xD53BD063u;
constexpr u32 kMovzX2_ABCD = 0xD29579A2u;
constexpr u32 kStrX2X4 = 0xF9000082u; // STR X2,[X4]
constexpr u32 kSvc42 = 0xD4000541u;
constexpr u32 kBrk0 = 0xD4200000u;
constexpr u32 kMovzX0Beef = 0xD2800000u | (0xBEEFu << 5);
constexpr u32 kSvc99 = 0xD4000C61u;
constexpr u32 kMovzX0Cafe = 0xD2800000u | (0xCAFEu << 5);
constexpr u32 kSvc77 = 0xD40009A1u;
constexpr u32 kMovzX0_7 = 0xD28000E0u;
constexpr u32 kSvc1 = 0xD4000021u;

u64 g_entry = 0;
u64 g_force_miss_pc = 0;

using BlockFn = void (*)(void*);
using SetBaseFn = void (*)(u64);

BlockFn g_block_tls = nullptr;
BlockFn g_block_unhandled = nullptr;
BlockFn g_block_unhandled_park = nullptr;
BlockFn g_block_miss = nullptr;
BlockFn g_block_miss_park = nullptr;
BlockFn g_block_plain = nullptr;
SetBaseFn g_set_base = nullptr;
void* g_so = nullptr;

Core::RecompBlockFn Lookup(u64 pc) {
    if (g_force_miss_pc != 0 && pc == g_force_miss_pc) {
        return nullptr;
    }
    if (pc == g_entry + kOffTlsSvc) {
        return g_block_tls;
    }
    if (pc == g_entry + kOffUnhandled) {
        return g_block_unhandled;
    }
    if (pc == g_entry + kOffUnhandledPark) {
        return g_block_unhandled_park;
    }
    if (pc == g_entry + kOffMiss) {
        return g_block_miss;
    }
    if (pc == g_entry + kOffMissPark) {
        return g_block_miss_park;
    }
    if (pc == g_entry + kOffPlain) {
        return g_block_plain;
    }
    return nullptr;
}

std::string BuildAotSource() {
    // Translate at module-relative PCs; g_module_base is set to entry at runtime.
    const std::string t_movz_tp = TranslateInsn(kMovzX0_1234, kOffTlsSvc + 0);
    const std::string t_msr_tp = TranslateInsn(kMsrTpidrX0, kOffTlsSvc + 4);
    const std::string t_mrs_tp = TranslateInsn(kMrsX1Tpidr, kOffTlsSvc + 8);
    const std::string t_mrs_ro = TranslateInsn(kMrsX3Tpidrro, kOffTlsSvc + 12);
    const std::string t_movz_val2 = TranslateInsn(kMovzX2_ABCD, kOffTlsSvc + 16);
    const std::string t_str2 = TranslateInsn(kStrX2X4, kOffTlsSvc + 20);
    const std::string t_svc42 = TranslateInsn(kSvc42, kOffTlsSvc + 24);
    const std::string t_brk = TranslateInsn(kBrk0, kOffUnhandled);
    const std::string t_park_u = TranslateInsn(kSvc1, kOffUnhandledPark);
    const std::string t_mov_beef = TranslateInsn(kMovzX0Beef, kOffMiss);
    const std::string t_svc99 = TranslateInsn(kSvc99, kOffMiss + 4);
    const std::string t_park_m = TranslateInsn(kSvc1, kOffMissPark);
    const std::string t_mov7 = TranslateInsn(kMovzX0_7, kOffPlain);
    const std::string t_svc1 = TranslateInsn(kSvc1, kOffPlain + 4);

    std::ostringstream src;
    src << R"C(#include <stdint.h>
#include <string.h>

#define RECOMP_HALT_UNHANDLED 2

typedef struct GuestContext {
    uint64_t x[32];
    uint64_t pc;
    uint8_t n, z, c, v;
    uint8_t* mem;
    uint64_t mem_size;
    uint64_t mem_base_vaddr;
    int halted;
    uint64_t pending_svc;
    uint64_t vreg[32][2];
    uint64_t tpidr_el0;
    const void* host_mem;
    uint64_t tpidrro_el0;
    uint64_t fpcr;
    uint64_t fpsr;
    int chain_budget;
} GuestContext;

typedef struct RecompHostMem {
    void* user;
    uint64_t (*load)(void* user, uint64_t va, uint32_t size);
    void (*store)(void* user, uint64_t va, uint32_t size, uint64_t value);
} RecompHostMem;

uint64_t g_module_base = 0;

void recomp_set_module_base(uint64_t b) { g_module_base = b; }

void recomp_svc(GuestContext* c, unsigned imm) { (void)c; (void)imm; }
void recomp_unhandled(GuestContext* c, uint32_t insn, uint64_t pc) {
    (void)insn;
    c->pc = pc;
    c->halted = RECOMP_HALT_UNHANDLED;
}
uint64_t recomp_load64(GuestContext* c, uint64_t a) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->load) return hm->load(hm->user, a, 8);
    return 0;
}
void recomp_store64(GuestContext* c, uint64_t a, uint64_t v) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->store) hm->store(hm->user, a, 8, v);
}

void block_tls_svc(GuestContext* c) {
)C";
    src << t_movz_tp << t_msr_tp << t_mrs_tp << t_mrs_ro << t_movz_val2 << t_str2 << t_svc42;
    src << R"C(}

void block_unhandled(GuestContext* c) {
)C";
    src << t_brk;
    src << R"C(}

void block_unhandled_park(GuestContext* c) {
)C";
    src << t_park_u;
    src << R"C(}

void block_miss(GuestContext* c) {
)C";
    src << t_mov_beef << t_svc99;
    src << R"C(}

void block_miss_park(GuestContext* c) {
)C";
    src << t_park_m;
    src << R"C(}

void block_plain(GuestContext* c) {
)C";
    src << t_mov7 << t_svc1;
    src << R"C(}
)C";
    return src.str();
}

bool BuildAndLoadAot(const fs::path& root) {
#ifdef _WIN32
    Fail("AOT shared library build requires POSIX dlopen in this harness");
    return false;
#else
    const fs::path src_dir = root / "aot_blocks";
    fs::create_directories(src_dir);
    if (!WriteFile(src_dir / "blocks.c", BuildAotSource())) {
        return false;
    }
    if (!WriteFile(src_dir / "CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.13)\n"
                   "project(suyu_stack_aot C)\n"
                   "set(CMAKE_C_STANDARD 11)\n"
                   "set(CMAKE_C_EXTENSIONS OFF)\n"
                   "add_library(stack_aot SHARED blocks.c)\n"
                   "set_target_properties(stack_aot PROPERTIES\n"
                   "  POSITION_INDEPENDENT_CODE ON\n"
                   "  C_VISIBILITY_PRESET default)\n")) {
        return false;
    }

    const fs::path build = src_dir / "build";
    std::vector<std::string> cfg{SUYU_SMOKE_CMAKE, "-S", src_dir.string(), "-B", build.string(),
                                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_STANDARD=11"};
    const std::string gen = SUYU_SMOKE_GENERATOR;
    if (!gen.empty()) {
        cfg.push_back("-G");
        cfg.push_back(gen);
    }
    const std::string cc = SUYU_SMOKE_C_COMPILER;
    if (!cc.empty()) {
        cfg.push_back(std::string("-DCMAKE_C_COMPILER=") + cc);
    }
    if (RunArgs(cfg) != 0) {
        Fail("cmake configure aot blocks");
        return false;
    }
    if (RunArgs({SUYU_SMOKE_CMAKE, "--build", build.string(), "--config", "Release", "--target",
                 "stack_aot"}) != 0) {
        Fail("cmake build aot blocks");
        return false;
    }

    fs::path so;
    for (const auto& p :
         {build / "libstack_aot.so", build / "stack_aot.so", build / "Release" / "libstack_aot.so"}) {
        if (fs::exists(p)) {
            so = p;
            break;
        }
    }
    if (so.empty()) {
        Fail("libstack_aot.so not found");
        return false;
    }

    g_so = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_so) {
        Fail(std::string("dlopen: ") + dlerror());
        return false;
    }
    g_set_base = reinterpret_cast<SetBaseFn>(dlsym(g_so, "recomp_set_module_base"));
    g_block_tls = reinterpret_cast<BlockFn>(dlsym(g_so, "block_tls_svc"));
    g_block_unhandled = reinterpret_cast<BlockFn>(dlsym(g_so, "block_unhandled"));
    g_block_unhandled_park = reinterpret_cast<BlockFn>(dlsym(g_so, "block_unhandled_park"));
    g_block_miss = reinterpret_cast<BlockFn>(dlsym(g_so, "block_miss"));
    g_block_miss_park = reinterpret_cast<BlockFn>(dlsym(g_so, "block_miss_park"));
    g_block_plain = reinterpret_cast<BlockFn>(dlsym(g_so, "block_plain"));
    ExpectTrue("dlsym recomp_set_module_base", g_set_base != nullptr);
    ExpectTrue("dlsym block_tls_svc", g_block_tls != nullptr);
    ExpectTrue("dlsym block_unhandled", g_block_unhandled != nullptr);
    ExpectTrue("dlsym block_miss", g_block_miss != nullptr);
    ExpectTrue("dlsym block_plain", g_block_plain != nullptr);
    return g_set_base && g_block_tls && g_block_unhandled && g_block_miss && g_block_plain;
#endif
}

void WriteGuestImage(std::vector<u8>& image) {
    auto put = [&](u64 off, u32 insn) {
        std::memcpy(image.data() + off, &insn, sizeof(insn));
    };
    // TLS + SVC fixture (same encodings Translate consumed).
    put(kOffTlsSvc + 0, kMovzX0_1234);
    put(kOffTlsSvc + 4, kMsrTpidrX0);
    put(kOffTlsSvc + 8, kMrsX1Tpidr);
    put(kOffTlsSvc + 12, kMrsX3Tpidrro);
    put(kOffTlsSvc + 16, kMovzX2_ABCD);
    put(kOffTlsSvc + 20, kStrX2X4);
    put(kOffTlsSvc + 24, kSvc42);
    // Unhandled AOT is BRK; Dynarmic path after halt uses MOVZ/SVC under the BRK site.
    put(kOffUnhandled, kMovzX0Cafe);
    put(kOffUnhandled + 4, kSvc77);
    // Registered miss target (also Dynarmic bytes when force-missed).
    put(kOffMiss, kMovzX0Beef);
    put(kOffMiss + 4, kSvc99);
    put(kOffMissPark, kSvc1);
    put(kOffUnhandledPark, kSvc1);
    // Plain / icache site.
    put(kOffPlain, kMovzX0_7);
    put(kOffPlain + 4, kSvc1);
}

struct StackFixture {
    Core::System system;
    Kernel::KProcess* process = nullptr;
    Kernel::KThread* thread = nullptr;
    Kernel::KThread* thread_b = nullptr;
    Core::ArmInterface* arm = nullptr;
    fs::path workdir;

    bool Bootstrap(const fs::path& root) {
        workdir = root;
        if (!BuildAndLoadAot(root)) {
            return false;
        }

        // Register lookup BEFORE any process InitializeInterfaces.
        Core::SetRecompLookup(&Lookup);

        system.Initialize();
        system.Kernel().Initialize();
        system.GetCpuManager().Initialize();

        auto& kernel = system.Kernel();
        process = Kernel::KProcess::Create(kernel);
        if (!process) {
            Fail("KProcess::Create");
            return false;
        }
        Kernel::KProcess::Register(kernel, process);

        const auto meta = FileSys::ProgramMetadata::GetDefault();
        if (process->LoadFromMetadata(kernel, meta, kImageBytes, 0, 0).IsError()) {
            Fail("LoadFromMetadata");
            return false;
        }

        kernel.AppendNewProcess(process);
        kernel.MakeApplicationProcess(process);
        process->Open(kernel);

        g_entry = GetInteger(process->GetEntryPoint());
        ExpectEq("entry", g_entry, kExpectedEntry);
        g_set_base(g_entry);

        arm = process->GetArmInterface(0);
        ExpectTrue("ArmInterface installed", arm != nullptr);
        ExpectTrue("SetRecompLookup still set", Core::GetRecompLookup() == &Lookup);

        Kernel::CodeSet codeset;
        codeset.memory.assign(kImageBytes, 0);
        WriteGuestImage(codeset.memory);
        codeset.CodeSegment().offset = 0;
        codeset.CodeSegment().addr = 0;
        codeset.CodeSegment().size = static_cast<u32>(kCodeBytes);
        codeset.RODataSegment().offset = 0;
        codeset.RODataSegment().addr = 0;
        codeset.RODataSegment().size = 0;
        codeset.DataSegment().offset = kCodeBytes;
        codeset.DataSegment().addr = kCodeBytes;
        codeset.DataSegment().size = static_cast<u32>(kImageBytes - kCodeBytes);
        process->LoadModule(kernel, std::move(codeset), process->GetEntryPoint());

        // Guest RX must hold the Translate'd encodings (not zeros).
        ExpectEq("guest RX tls movz", system.ApplicationMemory().Read32(g_entry + kOffTlsSvc),
                 kMovzX0_1234);
        ExpectEq("guest RX tls svc", system.ApplicationMemory().Read32(g_entry + kOffTlsSvc + 24),
                 kSvc42);
        ExpectEq("guest RX miss", system.ApplicationMemory().Read32(g_entry + kOffMiss),
                 kMovzX0Beef);

        Kernel::KProcessAddress stack_bottom{};
        if (process->GetPageTable()
                .MapPages(std::addressof(stack_bottom), 1, Kernel::KMemoryState::Stack,
                          Kernel::KMemoryPermission::UserReadWrite)
                .IsError()) {
            Fail("MapPages stack A");
            return false;
        }
        const Kernel::KProcessAddress stack_top = stack_bottom + Kernel::PageSize;

        thread = Kernel::KThread::Create(kernel);
        if (!thread) {
            Fail("KThread::Create A");
            return false;
        }
        if (Kernel::KThread::InitializeUserThread(system, thread, process->GetEntryPoint(), 0,
                                                  stack_top, meta.GetMainThreadPriority(), 0,
                                                  process)
                .IsError()) {
            Fail("InitializeUserThread A");
            return false;
        }
        Kernel::KThread::Register(kernel, thread);
        ExpectTrue("thread A TLS", GetInteger(thread->GetTlsAddress()) != 0);

        Kernel::KProcessAddress stack_b{};
        if (process->GetPageTable()
                .MapPages(std::addressof(stack_b), 1, Kernel::KMemoryState::Stack,
                          Kernel::KMemoryPermission::UserReadWrite)
                .IsError()) {
            Fail("MapPages stack B");
            return false;
        }
        thread_b = Kernel::KThread::Create(kernel);
        if (!thread_b) {
            Fail("KThread::Create B");
            return false;
        }
        if (Kernel::KThread::InitializeUserThread(system, thread_b, process->GetEntryPoint(), 0,
                                                  stack_b + Kernel::PageSize,
                                                  meta.GetMainThreadPriority(), 0, process)
                .IsError()) {
            Fail("InitializeUserThread B");
            return false;
        }
        Kernel::KThread::Register(kernel, thread_b);
        ExpectTrue("thread B TLS distinct",
                   GetInteger(thread_b->GetTlsAddress()) != GetInteger(thread->GetTlsAddress()));

        return g_fails == 0;
    }

    bool BootstrapSecondProcess() {
        auto& kernel = system.Kernel();
        auto* p2 = Kernel::KProcess::Create(kernel);
        if (!p2) {
            Fail("KProcess::Create restart");
            return false;
        }
        Kernel::KProcess::Register(kernel, p2);
        const auto meta = FileSys::ProgramMetadata::GetDefault();
        if (p2->LoadFromMetadata(kernel, meta, kImageBytes, 0, 0).IsError()) {
            Fail("LoadFromMetadata restart");
            return false;
        }
        kernel.AppendNewProcess(p2);
        kernel.MakeApplicationProcess(p2);
        p2->Open(kernel);

        ExpectEq("restart entry", GetInteger(p2->GetEntryPoint()), kExpectedEntry);
        g_set_base(g_entry);

        Kernel::CodeSet codeset;
        codeset.memory.assign(kImageBytes, 0);
        WriteGuestImage(codeset.memory);
        codeset.CodeSegment().offset = 0;
        codeset.CodeSegment().addr = 0;
        codeset.CodeSegment().size = static_cast<u32>(kCodeBytes);
        codeset.DataSegment().offset = kCodeBytes;
        codeset.DataSegment().addr = kCodeBytes;
        codeset.DataSegment().size = static_cast<u32>(kImageBytes - kCodeBytes);
        p2->LoadModule(kernel, std::move(codeset), p2->GetEntryPoint());

        Kernel::KProcessAddress stack_bottom{};
        if (p2->GetPageTable()
                .MapPages(std::addressof(stack_bottom), 1, Kernel::KMemoryState::Stack,
                          Kernel::KMemoryPermission::UserReadWrite)
                .IsError()) {
            Fail("MapPages restart stack");
            return false;
        }
        auto* t2 = Kernel::KThread::Create(kernel);
        if (Kernel::KThread::InitializeUserThread(system, t2, p2->GetEntryPoint(), 0,
                                                  stack_bottom + Kernel::PageSize,
                                                  meta.GetMainThreadPriority(), 0, p2)
                .IsError()) {
            Fail("InitializeUserThread restart");
            return false;
        }
        Kernel::KThread::Register(kernel, t2);

        process = p2;
        thread = t2;
        thread_b = nullptr;
        arm = p2->GetArmInterface(0);
        ExpectTrue("restart ArmInterface", arm != nullptr);
        return arm != nullptr;
    }

    void PrepThreadForTlsSvc(Kernel::KThread* t) {
        auto& ctx = t->GetContext();
        ctx = {};
        ctx.pc = g_entry + kOffTlsSvc;
        ctx.sp = GetInteger(t->GetTlsAddress()) ? GetInteger(t->GetTlsAddress()) : (g_entry + 0x3F00);
        ctx.r[4] = g_entry + kOffCrossPage;
    }
};

void ScenarioSvcTlsCrossPage(StackFixture& f) {
    const int before = g_fails;
    f.PrepThreadForTlsSvc(f.thread);
    // Publish TPIDRRO via LoadContext only (also covers context load).
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);
    // Overwrite tpidrro for this scenario's fixed expected value AFTER proving
    // LoadContext in ScenarioLoadContextTls — here we only need a known RO value
    // for the Translate'd MRS. Use SetTpidrroEl0 solely as the test input for
    // the MRS path, not as a substitute for LoadContext.
    f.arm->SetTpidrroEl0(0xC0FFEE);

    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("svc HaltReason", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("svc number", f.arm->GetSvcNumber(), 42);

    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("tpidr_el0 via GetContext", ctx.tpidr, 0x1234);
    ExpectEq("x1 tpidr readback", ctx.r[1], 0x1234);
    ExpectEq("x3 tpidrro", ctx.r[3], 0xC0FFEE);
    ExpectEq("cross-page store", f.system.ApplicationMemory().Read64(g_entry + kOffCrossPage),
             0xABCD);
    ScenarioPass("Translate AOT SVC/TLS/cross-page via ApplicationMemory", before);
}

void ScenarioLoadContextTls(StackFixture& f) {
    const int before = g_fails;
    auto& core0 = f.system.Kernel().PhysicalCore(0);
    const u64 tls_a = GetInteger(f.thread->GetTlsAddress());
    const u64 tls_b = GetInteger(f.thread_b->GetTlsAddress());

    // Poison TPIDRRO, then LoadContext alone must republish thread TLS.
    f.PrepThreadForTlsSvc(f.thread);
    f.arm->SetTpidrroEl0(0xDEAD);
    core0.LoadContext(f.thread);
    // Intentionally no SetTpidrroEl0 after LoadContext.
    const auto hr_a = f.arm->RunThread(f.thread);
    ExpectTrue("LoadContext A svc", True(hr_a & Core::HaltReason::SupervisorCall));
    Kernel::Svc::ThreadContext a{};
    f.arm->GetContext(a);
    ExpectEq("LoadContext A tpidrro==TLS", a.r[3], tls_a);

    f.PrepThreadForTlsSvc(f.thread_b);
    f.arm->SetTpidrroEl0(0xBEEF);
    core0.LoadContext(f.thread_b);
    const auto hr_b = f.arm->RunThread(f.thread_b);
    ExpectTrue("LoadContext B svc", True(hr_b & Core::HaltReason::SupervisorCall));
    Kernel::Svc::ThreadContext b{};
    f.arm->GetContext(b);
    ExpectEq("LoadContext B tpidrro==TLS", b.r[3], tls_b);
    ExpectTrue("TLS A != TLS B", tls_a != tls_b);
    ScenarioPass("PhysicalCore::LoadContext alone publishes TPIDRRO", before);
}

void ScenarioForceMissRegistered(StackFixture& f) {
    const int before = g_fails;
    // Registered AOT at kOffMiss; force Lookup null so Dynarmic runs guest RX.
    g_force_miss_pc = g_entry + kOffMiss;
    auto& ctx = f.thread->GetContext();
    ctx = {};
    ctx.pc = g_entry + kOffMiss;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

    const auto hr = f.arm->RunThread(f.thread);
    g_force_miss_pc = 0;

    ExpectTrue("registered-miss SupervisorCall", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("registered-miss svc", f.arm->GetSvcNumber(), 99);
    Kernel::Svc::ThreadContext out{};
    f.arm->GetContext(out);
    ExpectEq("registered-miss x0", out.r[0], 0xBEEF);
    ScenarioPass("force-miss of registered AOT PC entered Dynarmic", before);
}

void ScenarioUnhandledFallback(StackFixture& f) {
    const int before = g_fails;
    auto& ctx = f.thread->GetContext();
    ctx = {};
    ctx.pc = g_entry + kOffUnhandled;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("unhandled SupervisorCall", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("unhandled svc", f.arm->GetSvcNumber(), 77);
    Kernel::Svc::ThreadContext out{};
    f.arm->GetContext(out);
    ExpectEq("unhandled x0", out.r[0], 0xCAFE);
    ScenarioPass("Translate recomp_unhandled -> Dynarmic on guest RX", before);
}

void ScenarioInvalidation(StackFixture& f) {
    const int before = g_fails;
    auto& ctx = f.thread->GetContext();
    ctx = {};
    ctx.pc = g_entry + kOffPlain;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

    const auto hr1 = f.arm->RunThread(f.thread);
    ExpectTrue("pre-inv AOT svc", True(hr1 & Core::HaltReason::SupervisorCall));
    ExpectEq("pre-inv svc", f.arm->GetSvcNumber(), 1);
    Kernel::Svc::ThreadContext pre{};
    f.arm->GetContext(pre);
    ExpectEq("pre-inv x0 from Translate MOVZ #7", pre.r[0], 7);

    // ClearInstructionCache permanently refuses Translate AOT; also flush any
    // Dynarmic fallback that may already exist on each core.
    for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; ++i) {
        if (auto* iface = f.process->GetArmInterface(i)) {
            iface->ClearInstructionCache();
        }
    }

    // Rewrite guest RX so Dynarmic result differs from Translate AOT (x0=7/svc=1).
    f.system.ApplicationMemory().Write32(g_entry + kOffPlain, kMovzX0Beef);
    f.system.ApplicationMemory().Write32(g_entry + kOffPlain + 4, kSvc99);
    ExpectEq("post-rewrite guest", f.system.ApplicationMemory().Read32(g_entry + kOffPlain),
             kMovzX0Beef);
    ExpectTrue("plain AOT still registered after Clear",
               Lookup(g_entry + kOffPlain) == g_block_plain && g_block_plain != nullptr);

    ctx = {};
    ctx.pc = g_entry + kOffPlain;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);
    f.arm = f.process->GetArmInterface(0);
    const auto hr2 = f.arm->RunThread(f.thread);
    ExpectTrue("post-inv Dynarmic svc", True(hr2 & Core::HaltReason::SupervisorCall));
    ExpectEq("post-inv svc", f.arm->GetSvcNumber(), 99);
    Kernel::Svc::ThreadContext post{};
    f.arm->GetContext(post);
    ExpectEq("post-inv x0", post.r[0], 0xBEEF);
    ScenarioPass("ClearInstructionCache refuses Translate AOT on real RX", before);
}

void ScenarioRestart(StackFixture& f) {
    const int before = g_fails;
    if (!f.BootstrapSecondProcess()) {
        return;
    }
    f.PrepThreadForTlsSvc(f.thread);
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);
    f.arm->SetTpidrroEl0(0xABCD1234ULL);
    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("restart svc", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("restart svc num", f.arm->GetSvcNumber(), 42);
    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("restart tpidrro", ctx.r[3], 0xABCD1234ULL);
    ScenarioPass("new process ArmRecomp (fresh icache) re-runs Translate AOT", before);
}

void ScenarioStepMiss(StackFixture& f) {
    const int before = g_fails;
    // On the restarted process: force-miss registered AOT at kOffMiss.
    // Restore guest encodings at kOffPlain may have been rewritten; miss site untouched.
    g_force_miss_pc = g_entry + kOffMiss;
    auto& ctx = f.thread->GetContext();
    ctx = {};
    ctx.pc = g_entry + kOffMiss;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

    const auto hr = f.arm->StepThread(f.thread);
    g_force_miss_pc = 0;

    Kernel::Svc::ThreadContext out{};
    f.arm->GetContext(out);
    ExpectEq("step-miss x0", out.r[0], 0xBEEF);
    ExpectEq("step-miss pc", out.pc, g_entry + kOffMiss + 4);
    ExpectTrue("step-miss HaltReason::StepThread", True(hr & Core::HaltReason::StepThread));
    ScenarioPass("StepThread force-miss of registered AOT uses Dynarmic step", before);
}

void PrintGaps() {
    std::cout
        << "GAPS (honest / out of scope):\n"
        << "  - Full PhysicalCore::RunThread -> Svc::Call HLE (needs safe SVC + services)\n"
        << "  - Multi-core KScheduler fiber world / CpuManager guest loop\n"
        << "  - Real NSO/NRO homebrew load (keys/firmware/dumps)\n"
        << "  - gdbstub StepThread against a live title\n"
        << "Pinned here: SetRecompLookup process ArmRecomp, Translate AOT + guest RX,\n"
        << "  LoadContext TLS publish, registered-PC force-miss, icache, restart, StepThread.\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "recomp_stack_harness: SetRecompLookup ArmRecomp + Translate AOT + Dynarmic\n";

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("suyu-recomp-stack-" + std::to_string(stamp));
    fs::create_directories(root);

    // Leak fixture: minimal bootstrap has no safe System shutdown.
    auto* fix = new StackFixture;
    if (!fix->Bootstrap(root)) {
        std::cerr << "bootstrap failed\n";
        std::quick_exit(1);
    }
    Pass("bootstrap SetRecompLookup + application KProcess ArmRecomp");

    ScenarioSvcTlsCrossPage(*fix);
    ScenarioLoadContextTls(*fix);
    // Force-miss + unhandled need AllowsAot (registered Translate blocks).
    ScenarioForceMissRegistered(*fix);
    ScenarioUnhandledFallback(*fix);
    // ClearInstructionCache permanently refuses AOT; run after the above.
    ScenarioInvalidation(*fix);
    ScenarioRestart(*fix);
    ScenarioStepMiss(*fix);
    PrintGaps();

    if (g_fails == 0) {
        std::cout << "ALL PASSED\n";
        std::quick_exit(0);
    }
    std::cerr << g_fails << " FAILURE(S)\n";
    std::quick_exit(1);
}
