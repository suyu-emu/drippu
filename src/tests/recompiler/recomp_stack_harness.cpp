// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Real-stack synthetic-homebrew integration harness: ArmRecomp (process
// ArmInterface via SetRecompLookup) + in-tree Dynarmic fallback + the kernel
// scheduler's PhysicalCore dispatch and SVC path.
//
// AOT blocks are Translate()'d from real guest encodings and compiled into a
// shared library (SUYU_HOSTED_RECOMP helpers). Matching AArch64 bytes are
// written into guest RX so Dynarmic fallback executes the same sites.
//
// Backlog #3: after the integration scenarios, the same guest fixture (same
// encodings / same PCs) is raced under hybrid AOT (lookup hit) vs JIT
// (force-miss → Dynarmic). Slice times are host RunThread durations (no GPU
// frames in this harness). Results: recomp_benchmark.json.
//
// The bench ADD chain is punctuated with STR/LDR of x0 through the opaque
// host_mem callbacks so Release (-O3) cannot strength-reduce 512 ADDs to
// `x1 << 9`. JudgeAotBenchDump: a PLT jmp is not a loop (discriminating dump:
// store+load + 1 add + PLT jmp + no shl/imul must FAIL, with count asserts).
// Named `recomp_store64@plt` is a gcc PIC accident — clang/aarch64 `bl` may
// have store=0; that is not a fold. Live pin is: not shl/imul, plus runtime
// host_mem callback counts (512 × iters).
//
// The fixture is a small custom CodeSet image, not an NSO/NRO. It is entirely
// generated in this test and contains no firmware, keys, or copyrighted data.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <regex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <nlohmann/json.hpp>

#include "common/common_types.h"
#include "common/settings.h"
#include "common/typed_address.h"
#include "core/arm/arm_interface.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/arm/recomp/recomp_image_abi.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hardware_properties.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory_types.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "core/recompiler/arm64_to_c.h"
#include "smoke_config.h"
#include "tests/recompiler/insn_correctness.h"

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

void ExpectEq(const std::string& name, u64 got, u64 want) {
    if (got != want) {
        Fail(std::string(name) + ": got=" + std::to_string(got) + " want=" + std::to_string(want));
    } else {
        Pass(std::string(name) + "=" + std::to_string(got));
    }
}

void ExpectTrue(const std::string& name, bool cond) {
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

void AppendSanitizerCmakeArgs(std::vector<std::string>& cfg) {
    const char* flags = SUYU_SMOKE_SANITIZER_FLAGS;
    if (!flags || flags[0] == '\0') {
        return;
    }
    cfg.push_back(std::string("-DCMAKE_C_FLAGS=") + flags);
    cfg.push_back(std::string("-DCMAKE_CXX_FLAGS=") + flags);
    cfg.push_back(std::string("-DCMAKE_EXE_LINKER_FLAGS=") + flags);
    cfg.push_back(std::string("-DCMAKE_SHARED_LINKER_FLAGS=") + flags);
}

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
// AOT proof: Translate MOVZ #7 / SVC #1, but guest RX is BEEF/99 (Dynarmic twin differs).
constexpr u64 kOffAotProof = 0x1400;
constexpr u64 kOffPlain = 0x1800; // icache / Translate MOVZ #7 (guest RX matches)
constexpr u64 kOffStepNoSvc = 0x1C00; // MOVZ #7 only — leftover pending_svc pin
constexpr u64 kOffCrossPage = 0x1FFC;
constexpr u64 kOffCoreDispatch = 0x2008; // TLS + safe GetCurrentProcessorNumber SVC
// Past the 8-byte STR at kOffCrossPage (0x1FFC..0x2003).
// 512 × (STR + LDR + ADD) + MOVZ + SVC = 1538 insns = 0x1810 bytes.
constexpr u64 kOffBench = 0x2400;
constexpr int kBenchAdds = 512;
constexpr u32 kBenchSvcImm = 3;
constexpr u64 kCodeBytes = 5 * Kernel::PageSize;
constexpr u64 kImageBytes = 6 * Kernel::PageSize;
constexpr u64 kOffBenchScratch = kCodeBytes; // data-segment word STR/LDR bounce
constexpr u64 kOffInsnScratch = kCodeBytes + 0x40;

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
constexpr u32 kSvcGetCurrentProcessor = 0xD4000201u; // SVC #0x10
constexpr u32 kMovzX1_55 = 0xD2800AA1u;
constexpr u32 kMovzX0_0 = 0xD2800000u;
constexpr u32 kAddX0X0X1 = 0x8B010000u; // ADD X0, X0, X1
constexpr u32 kStrX0X3 = 0xF9000060u;   // STR X0, [X3] — opaque store breaks x0 recurrence
constexpr u32 kLdrX0X3 = 0xF9400060u;   // LDR X0, [X3]
constexpr u32 kSvc3 = 0xD4000061u;      // SVC #3

u64 g_entry = 0;
u64 g_force_miss_pc = 0;
std::atomic<u64> g_lookup_calls{0};

using BlockFn = void (*)(void*);
using SetBaseFn = void (*)(u64);

BlockFn g_block_tls = nullptr;
BlockFn g_block_unhandled = nullptr;
BlockFn g_block_unhandled_park = nullptr;
BlockFn g_block_miss = nullptr;
BlockFn g_block_miss_park = nullptr;
BlockFn g_block_aot_proof = nullptr;
BlockFn g_block_plain = nullptr;
BlockFn g_block_step_no_svc = nullptr;
BlockFn g_block_core_dispatch = nullptr;
BlockFn g_block_bench = nullptr;
BlockFn g_block_insn[suyu::recomp::insn_test::kInsnBlockCount]{};
SetBaseFn g_set_base = nullptr;
void* g_so = nullptr;

struct AotCompileCosts {
    u64 translate_ns{};
    u64 bench_translate_ns{};
    u64 cmake_configure_ns{};
    u64 cmake_build_ns{};
    u64 dlopen_ns{};
    u64 so_bytes{};
    u64 bench_c_bytes{};
    std::string so_path;
    u64 disasm_add_count{};
    u64 disasm_shl9_count{};
    u64 disasm_imul512_count{};
    u64 disasm_backward_jumps{};
    u64 disasm_plt_jumps{};
    u64 disasm_store64_calls{}; // named PLT in objdump (gcc PIC; may be 0 on clang)
    u64 disasm_load64_calls{};
    bool not_single_shift{};
    std::string disasm_excerpt;
    u64 clang_disasm_add{};
    u64 clang_disasm_store64{};
    u64 clang_disasm_load64{};
    bool clang_not_reported_folded{true};
    std::string clang_dump_reason;
};
AotCompileCosts g_aot_compile;

u64* g_hm_load_calls = nullptr;
u64* g_hm_store_calls = nullptr;

u64 HmLoadCalls() {
    return g_hm_load_calls ? *g_hm_load_calls : 0;
}
u64 HmStoreCalls() {
    return g_hm_store_calls ? *g_hm_store_calls : 0;
}

u64 NsSince(std::chrono::steady_clock::time_point t0) {
    const auto raw =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    return raw > 0 ? static_cast<u64>(raw) : 0;
}

std::vector<std::pair<u64, u32>> BenchInsns() {
    std::vector<std::pair<u64, u32>> v;
    v.reserve(static_cast<size_t>(kBenchAdds) * 3 + 2);
    u64 off = kOffBench;
    v.emplace_back(off, kMovzX0_0);
    off += 4;
    for (int i = 0; i < kBenchAdds; ++i) {
        // STR/LDR of x0 through recomp_store/load (function pointers) so -O3
        // cannot treat the ADD chain as a loop-invariant `x1 << 9`.
        v.emplace_back(off, kStrX0X3);
        off += 4;
        v.emplace_back(off, kLdrX0X3);
        off += 4;
        v.emplace_back(off, kAddX0X0X1);
        off += 4;
    }
    v.emplace_back(off, kSvc3);
    return v;
}

int BenchIters() {
    int n = 32;
    if (const char* env = std::getenv("SUYU_RECOMP_BENCH_ITERS"); env && env[0] != '\0') {
        n = std::atoi(env);
    }
    return n < 4 ? 4 : n;
}

struct MemSnap {
    u64 vmrss_kb{};
    u64 vmhwm_kb{};
    u64 vmsize_kb{};
};

MemSnap ReadMem() {
    MemSnap m;
#ifndef _WIN32
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        auto take = [&](const char* key, u64& dest) {
            if (line.rfind(key, 0) != 0) {
                return;
            }
            dest = static_cast<u64>(std::strtoull(line.c_str() + std::strlen(key), nullptr, 10));
        };
        take("VmRSS:", m.vmrss_kb);
        take("VmHWM:", m.vmhwm_kb);
        take("VmSize:", m.vmsize_kb);
    }
#endif
    return m;
}

std::string HostCpu() {
#ifndef _WIN32
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find("model name");
        if (pos == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(colon + 1);
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                    name.erase(name.begin());
                }
                return name;
            }
        }
    }
#endif
    return {};
}

struct DisasmCounts {
    u64 add{};
    u64 shl9{};
    u64 imul512{};
    u64 backward_jumps{}; // in-function backedges only (not jmp to a lower PLT)
    u64 plt_jumps{};
    u64 call{};
    u64 store64{};
    u64 load64{};
};

struct BenchDumpVerdict {
    bool ok{};
    const char* reason = "";
};

// "$0x9" must not match "$0x90". Next char is end, comma, or whitespace.
bool HasImmToken(const std::string& args, std::string_view imm) {
    size_t p = 0;
    while ((p = args.find(imm.data(), p, imm.size())) != std::string::npos) {
        const size_t after = p + imm.size();
        const char c = after < args.size() ? args[after] : '\0';
        if (c == '\0' || c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            return true;
        }
        ++p;
    }
    return false;
}

DisasmCounts CountBlockBenchOps(const std::string& fn) {
    DisasmCounts c;
    std::vector<std::pair<u64, u64>> jumps;
    u64 fn_lo = ~u64{0};
    u64 fn_hi = 0;
    std::istringstream in(fn);
    std::string line;
    while (std::getline(in, line)) {
        u64 addr = std::strtoull(line.c_str(), nullptr, 16);
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto tab = line.rfind('\t');
        if (tab == std::string::npos) {
            continue;
        }
        if (addr != 0) {
            fn_lo = std::min(fn_lo, addr);
            fn_hi = std::max(fn_hi, addr);
        }
        std::string rest = line.substr(tab + 1);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
            rest.erase(rest.begin());
        }
        const auto first = rest.find(' ');
        const std::string mnem = first == std::string::npos ? rest : rest.substr(0, first);
        const std::string args = first == std::string::npos ? std::string{} : rest.substr(first);
        if (mnem == "add" || mnem == "addq") {
            if (args.find("%rsp") == std::string::npos && args.find("%rbp") == std::string::npos) {
                ++c.add;
            }
        } else if (mnem == "shl" || mnem == "shlq" || mnem == "sal" || mnem == "salq") {
            if (HasImmToken(args, "$0x9") || HasImmToken(args, "$9")) {
                ++c.shl9;
            }
        } else if (mnem == "imul" || mnem == "imulq") {
            if (HasImmToken(args, "$0x200") || HasImmToken(args, "$512")) {
                ++c.imul512;
            }
        } else if (mnem == "call" || mnem == "callq") {
            ++c.call;
            if (line.find("recomp_store64") != std::string::npos) {
                ++c.store64;
            }
            if (line.find("recomp_load64") != std::string::npos) {
                ++c.load64;
            }
        } else if (!mnem.empty() && mnem[0] == 'j') {
            size_t t = 0;
            while (t < args.size() && std::isspace(static_cast<unsigned char>(args[t]))) {
                ++t;
            }
            u64 target = 0;
            if (t < args.size()) {
                target = std::strtoull(args.c_str() + t, nullptr, 16);
            }
            if (addr != 0 && target != 0) {
                jumps.emplace_back(addr, target);
            }
        }
    }
    for (const auto& [from, to] : jumps) {
        const bool in_fn = fn_lo != ~u64{0} && to >= fn_lo && to <= fn_hi;
        if (in_fn && to < from) {
            ++c.backward_jumps;
        } else if (!in_fn && to < from) {
            ++c.plt_jumps;
        }
    }
    return c;
}

// Folded x1<<9 must lose. A PLT jmp is not an in-function loop: store+load +
// 1 add + PLT jmp + no shl/imul is FAIL. Named recomp_store64 in the dump is
// not required for PASS (clang PIC / aarch64 bl may omit it).
BenchDumpVerdict JudgeAotBenchDump(const DisasmCounts& n) {
    if (n.shl9 > 0 || n.imul512 > 0) {
        return {false, "host fold: shl $0x9 / imul $512"};
    }
    if (n.add >= static_cast<u64>(kBenchAdds)) {
        return {true, "unrolled >=512 adds (named store/load PLT not required)"};
    }
    if (n.backward_jumps >= 1 && n.add >= 1) {
        return {true, "in-function add loop"};
    }
    return {false, "too few adds and no in-function loop"};
}

void RecordBenchDump(const DisasmCounts& n, const std::string& fn) {
    g_aot_compile.disasm_add_count = n.add;
    g_aot_compile.disasm_shl9_count = n.shl9;
    g_aot_compile.disasm_imul512_count = n.imul512;
    g_aot_compile.disasm_backward_jumps = n.backward_jumps;
    g_aot_compile.disasm_plt_jumps = n.plt_jumps;
    g_aot_compile.disasm_store64_calls = n.store64;
    g_aot_compile.disasm_load64_calls = n.load64;
    std::istringstream lines(fn);
    std::string line;
    int kept = 0;
    std::ostringstream excerpt;
    while (std::getline(lines, line) && kept < 40) {
        excerpt << line << '\n';
        ++kept;
    }
    g_aot_compile.disasm_excerpt = excerpt.str();
    const auto v = JudgeAotBenchDump(n);
    g_aot_compile.not_single_shift = v.ok;
    std::cout << "AOT block_bench objdump: add=" << n.add << " shl9=" << n.shl9
              << " imul512=" << n.imul512 << " in_fn_back_jcc=" << n.backward_jumps
              << " plt_jmp=" << n.plt_jumps << " store64=" << n.store64
              << " load64=" << n.load64 << " call=" << n.call << " -> " << v.reason << "\n";
}

void ExpectDumpVerdict(const char* name, const std::string& dump, bool want_ok) {
    const DisasmCounts n = CountBlockBenchOps(dump);
    const auto v = JudgeAotBenchDump(n);
    if (v.ok != want_ok) {
        Fail(std::string(name) + ": verdict=" + (v.ok ? "PASS" : "FAIL") + " (" + v.reason +
             ") want " + (want_ok ? "PASS" : "FAIL") + " add=" + std::to_string(n.add) +
             " shl9=" + std::to_string(n.shl9) + " back=" + std::to_string(n.backward_jumps) +
             " plt_jmp=" + std::to_string(n.plt_jumps) + " store=" + std::to_string(n.store64) +
             " load=" + std::to_string(n.load64));
    } else {
        Pass(std::string(name) + " (" + v.reason + ")");
    }
}

void ExpectDumpCounts(const char* name, const DisasmCounts& n, u64 add, u64 shl9, u64 imul512,
                      u64 backward_jumps, u64 plt_jumps, u64 store64, u64 load64) {
    auto chk = [&](const char* field, u64 got, u64 want) {
        if (got != want) {
            Fail(std::string(name) + " " + field + ": got=" + std::to_string(got) +
                 " want=" + std::to_string(want));
        } else {
            Pass(std::string(name) + " " + field + "=" + std::to_string(got));
        }
    };
    chk("add", n.add, add);
    chk("shl9", n.shl9, shl9);
    chk("imul512", n.imul512, imul512);
    chk("backward_jumps", n.backward_jumps, backward_jumps);
    chk("plt_jumps", n.plt_jumps, plt_jumps);
    chk("store64", n.store64, store64);
    chk("load64", n.load64, load64);
}

void ScenarioFoldPinSelfCheck() {
    const int before = g_fails;
    // Tabs match GNU objdump. PLT is at a lower address than block_bench, so a
    // naive "target < addr" count treats SVC jmp as a loop. Must still FAIL.
    const std::string folded_shl_svc_jmp =
        "0000000000001410 <block_bench>:\n"
        "    1410:\t48 8b 77 08         \tmov    0x8(%rdi),%rsi\n"
        "    1414:\t48 c1 e6 09         \tshl    $0x9,%rsi\n"
        "    1418:\t48 01 37            \tadd    %rsi,(%rdi)\n"
        "    141b:\te9 a0 fc ff ff      \tjmp    10c0 <recomp_svc@plt>\n";
    ExpectDumpVerdict("folded shl $0x9 + SVC jmp is rejected", folded_shl_svc_jmp, false);

    // Store/load calls must not rescue a shift-fold (shl9 is fatal).
    const std::string folded_shl_named_plt =
        "0000000000001410 <block_bench>:\n"
        "    1410:\t48 c1 e6 09         \tshl    $0x9,%rsi\n"
        "    1414:\te8 87 fc ff ff      \tcall   10a0 <recomp_store64@plt>\n"
        "    1419:\te8 92 fc ff ff      \tcall   10b0 <recomp_load64@plt>\n"
        "    141e:\te9 9d fc ff ff      \tjmp    10c0 <recomp_svc@plt>\n";
    ExpectDumpVerdict("folded shl $0x9 still rejected with store/load calls",
                      folded_shl_named_plt, false);

    const std::string folded_imul =
        "0000000000001410 <block_bench>:\n"
        "    1410:\t48 69 f6 00 02 00 00\timul   $0x200,%rsi,%rsi\n"
        "    1417:\t48 01 37            \tadd    %rsi,(%rdi)\n"
        "    141a:\te8 a1 fc ff ff      \tcall   10c0 <recomp_svc@plt>\n";
    ExpectDumpVerdict("folded imul $512 + SVC call is rejected", folded_imul, false);

    const std::string add_only =
        "0000000000001410 <block_bench>:\n"
        "    1410:\t48 03 47 08         \tadd    0x8(%rdi),%rax\n"
        "    1414:\t48 03 47 08         \tadd    0x8(%rdi),%rax\n"
        "    1418:\te9 a3 fc ff ff      \tjmp    10c0 <recomp_svc@plt>\n";
    ExpectDumpVerdict("add-only dump (no store/load) is rejected", add_only, false);

    // Discriminating dump: store+load + 1 add + PLT jmp + no shl/imul.
    // If fn_lo/fn_hi is reverted, plt_jmp is counted as backward_jumps and
    // this dump wrongly PASSes as an in-function loop.
    const std::string plt_not_loop =
        "0000000000001410 <block_bench>:\n"
        "    1410:\te8 8b fc ff ff      \tcall   10a0 <recomp_store64@plt>\n"
        "    1415:\te8 96 fc ff ff      \tcall   10b0 <recomp_load64@plt>\n"
        "    141a:\t48 03 43 08         \tadd    0x8(%rbx),%rax\n"
        "    141e:\te9 9d fc ff ff      \tjmp    10c0 <recomp_svc@plt>\n";
    {
        const DisasmCounts n = CountBlockBenchOps(plt_not_loop);
        ExpectDumpCounts("discriminating PLT dump", n, /*add=*/1, /*shl9=*/0, /*imul512=*/0,
                         /*backward_jumps=*/0, /*plt_jumps=*/1, /*store64=*/1, /*load64=*/1);
        ExpectDumpVerdict("discriminating store+load+1 add+PLT jmp (no shl) is rejected",
                          plt_not_loop, false);
    }

    const std::string honest_loop =
        "0000000000001410 <block_bench>:\n"
        "    1410:\t48 c7 07 00 00 00 00\tmovq   $0x0,(%rdi)\n"
        "    1417:\te8 84 fc ff ff      \tcall   10a0 <recomp_store64@plt>\n"
        "    141c:\te8 8f fc ff ff      \tcall   10b0 <recomp_load64@plt>\n"
        "    1421:\t48 03 43 08         \tadd    0x8(%rbx),%rax\n"
        "    1425:\t75 f0               \tjne    1417 <block_bench+0x7>\n"
        "    1427:\tc3                  \tret\n";
    ExpectDumpVerdict("in-function add+store+load loop is accepted", honest_loop, true);

    // clang 18 PIC: 512 adds, many calls, no named recomp_store64/load64.
    // Must PASS (not reported as folded). AArch64 `bl` looks the same to this pin.
    std::ostringstream clang_like;
    clang_like << "0000000000001410 <block_bench>:\n";
    u64 addr = 0x1410;
    for (int i = 0; i < kBenchAdds; ++i) {
        clang_like << "    " << std::hex << addr
                   << ":\t48 03 43 08          \tadd    0x8(%rbx),%rax\n";
        addr += 4;
    }
    clang_like << "    " << std::hex << addr << ":\te9 00 fc ff ff      \tjmp    10c0 <recomp_svc@plt>\n";
    {
        const std::string dump = clang_like.str();
        const DisasmCounts n = CountBlockBenchOps(dump);
        ExpectDumpCounts("clang-like unnamed dump", n, static_cast<u64>(kBenchAdds), 0, 0, 0, 1, 0,
                         0);
        ExpectDumpVerdict("clang-like 512 adds with no named store/load is not folded", dump, true);
    }

    ScenarioPass("objdump fold pin rejects shl $0x9 + SVC jmp; PLT is not a loop", before);
}

#ifndef _WIN32
std::string PopenDump(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        return {};
    }
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) {
        out.append(buf);
    }
    pclose(p);
    return out;
}

std::string ExtractObjdumpFn(const std::string& dump, const char* name) {
    const std::string tag = std::string("<") + name + ">:";
    const auto tag_at = dump.find(tag);
    if (tag_at == std::string::npos) {
        return {};
    }
    const auto line_start = dump.rfind('\n', tag_at);
    const auto from = line_start == std::string::npos ? 0 : line_start + 1;
    static const std::regex next_fn("\n[0-9A-Fa-f]+ <[^>]+>:");
    std::smatch m;
    const std::string rest = dump.substr(tag_at + tag.size());
    if (std::regex_search(rest, m, next_fn)) {
        return dump.substr(from, (tag_at + tag.size() + static_cast<size_t>(m.position())) - from);
    }
    return dump.substr(from);
}

void PinAotBenchNotFolded(const fs::path& so) {
    const std::string cmd = "objdump -d " + Quote(so.string()) + " 2>/dev/null";
    const std::string dump = PopenDump(cmd);
    if (dump.empty()) {
        Fail("objdump -d of AOT .so produced no output (objdump required to pin no x1<<9 fold)");
        return;
    }
    const std::string fn = ExtractObjdumpFn(dump, "block_bench");
    if (fn.empty()) {
        Fail("objdump missing <block_bench> in " + so.string());
        return;
    }
    const DisasmCounts n = CountBlockBenchOps(fn);
    RecordBenchDump(n, fn);
    if (n.shl9 > 0 || n.imul512 > 0) {
        Fail("AOT block_bench was host-folded (x1<<9 / imul 512), not 512 adds; dump:\n" + fn);
        return;
    }
    if (!g_aot_compile.not_single_shift) {
        Fail("AOT block_bench dump is not 512 adds and not an in-function loop; dump:\n" + fn);
        return;
    }
    ExpectTrue("AOT block_bench is not a single x1<<9", g_aot_compile.not_single_shift);
    // Named recomp_store64@plt is gcc PIC, not the pin. clang / aarch64 bl may
    // have store64=0; that must not be reported as a fold. Runtime host_mem
    // callback counts (512 × iters) pin that the memory ops actually ran.
}

void PinClangDumpNotFalseFolded(const fs::path& blocks_c) {
    const char* clang = "/usr/bin/clang-18";
    if (!fs::exists(clang)) {
        clang = "/usr/bin/clang";
    }
    if (!fs::exists(clang)) {
        std::cout << "clang not found; host_mem callback counts still pin store/load\n";
        g_aot_compile.clang_dump_reason = "clang not found";
        return;
    }
    const fs::path out = blocks_c.parent_path() / "libstack_aot_clang.so";
    const int rc = RunArgs({clang, "-O3", "-fPIC", "-shared", "-std=c11", "-o", out.string(),
                            blocks_c.string()});
    if (rc != 0 || !fs::exists(out)) {
        Fail(std::string("clang -O3 AOT .so failed rc=") + std::to_string(rc));
        g_aot_compile.clang_dump_reason = "clang compile failed";
        return;
    }
    const std::string dump = PopenDump("objdump -d " + Quote(out.string()) + " 2>/dev/null");
    const std::string fn = ExtractObjdumpFn(dump, "block_bench");
    if (fn.empty()) {
        Fail("clang objdump missing <block_bench>");
        return;
    }
    const DisasmCounts n = CountBlockBenchOps(fn);
    g_aot_compile.clang_disasm_add = n.add;
    g_aot_compile.clang_disasm_store64 = n.store64;
    g_aot_compile.clang_disasm_load64 = n.load64;
    const auto v = JudgeAotBenchDump(n);
    g_aot_compile.clang_not_reported_folded = v.ok && n.shl9 == 0 && n.imul512 == 0;
    g_aot_compile.clang_dump_reason = v.reason;
    std::cout << "clang -O3 block_bench objdump: add=" << n.add << " shl9=" << n.shl9
              << " imul512=" << n.imul512 << " store64=" << n.store64 << " load64=" << n.load64
              << " call=" << n.call << " in_fn_back=" << n.backward_jumps
              << " plt_jmp=" << n.plt_jumps << " -> " << v.reason << "\n";
    if (n.shl9 > 0 || n.imul512 > 0) {
        Fail("clang -O3 folded bench to shl/imul (unexpected with STR/LDR fixture)");
        return;
    }
    ExpectTrue("clang -O3 dump is not reported as folded", g_aot_compile.clang_not_reported_folded);
    if (n.store64 == 0 && n.load64 == 0) {
        Pass("clang -O3 has no named recomp_store64/load64 (PIC accident, not a fold)");
    }
}
#endif

Core::ArmRecomp* AsRecomp(Core::ArmInterface* iface) {
    if (!iface || !iface->IsRecompBackend()) {
        return nullptr;
    }
    // Safe after IsRecompBackend(): builds are -fno-rtti.
    return static_cast<Core::ArmRecomp*>(iface);
}

Core::RecompBlockFn Lookup(u64 pc) {
    g_lookup_calls.fetch_add(1, std::memory_order_relaxed);
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
    if (pc == g_entry + kOffAotProof) {
        return g_block_aot_proof;
    }
    if (pc == g_entry + kOffPlain) {
        return g_block_plain;
    }
    if (pc == g_entry + kOffStepNoSvc) {
        return g_block_step_no_svc;
    }
    if (pc == g_entry + kOffCoreDispatch) {
        return g_block_core_dispatch;
    }
    if (pc == g_entry + kOffBench) {
        return g_block_bench;
    }
    if (pc >= g_entry + suyu::recomp::insn_test::kOffInsn) {
        const u64 rel = pc - (g_entry + suyu::recomp::insn_test::kOffInsn);
        const u64 idx = rel / suyu::recomp::insn_test::kInsnStride;
        if (idx < static_cast<u64>(suyu::recomp::insn_test::kInsnBlockCount) &&
            rel % suyu::recomp::insn_test::kInsnStride == 0) {
            return g_block_insn[idx];
        }
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
    // AOT proof site: Translate #7/SVC1 while guest RX holds BEEF/99.
    const std::string t_proof_mov = TranslateInsn(kMovzX0_7, kOffAotProof);
    const std::string t_proof_svc = TranslateInsn(kSvc1, kOffAotProof + 4);
    const std::string t_mov7 = TranslateInsn(kMovzX0_7, kOffPlain);
    const std::string t_svc1 = TranslateInsn(kSvc1, kOffPlain + 4);
    const std::string t_step_mov7 = TranslateInsn(kMovzX0_7, kOffStepNoSvc);
    const std::string t_core_mov = TranslateInsn(kMovzX0_1234, kOffCoreDispatch + 0);
    const std::string t_core_tls = TranslateInsn(kMrsX3Tpidrro, kOffCoreDispatch + 4);
    const std::string t_core_marker = TranslateInsn(kMovzX1_55, kOffCoreDispatch + 8);
    const std::string t_core_svc =
        TranslateInsn(kSvcGetCurrentProcessor, kOffCoreDispatch + 12);

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
uint64_t g_recomp_hm_load_calls = 0;
uint64_t g_recomp_hm_store_calls = 0;

void recomp_set_module_base(uint64_t b) { g_module_base = b; }

void recomp_svc(GuestContext* c, unsigned imm) { (void)c; (void)imm; }
void recomp_unhandled(GuestContext* c, uint32_t insn, uint64_t pc) {
    (void)insn;
    c->pc = pc;
    c->halted = RECOMP_HALT_UNHANDLED;
}
uint64_t recomp_load64(GuestContext* c, uint64_t a) {
    ++g_recomp_hm_load_calls;
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->load) return hm->load(hm->user, a, 8);
    return 0;
}
void recomp_store64(GuestContext* c, uint64_t a, uint64_t v) {
    ++g_recomp_hm_store_calls;
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->store) hm->store(hm->user, a, 8, v);
}
uint64_t recomp_load8(GuestContext* c, uint64_t a) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->load) return hm->load(hm->user, a, 1);
    return 0;
}
uint64_t recomp_load16(GuestContext* c, uint64_t a) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->load) return hm->load(hm->user, a, 2);
    return 0;
}
uint64_t recomp_load32(GuestContext* c, uint64_t a) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->load) return hm->load(hm->user, a, 4);
    return 0;
}
void recomp_store8(GuestContext* c, uint64_t a, uint64_t v) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->store) hm->store(hm->user, a, 1, v);
}
void recomp_store16(GuestContext* c, uint64_t a, uint64_t v) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->store) hm->store(hm->user, a, 2, v);
}
void recomp_store32(GuestContext* c, uint64_t a, uint64_t v) {
    const RecompHostMem* hm = (const RecompHostMem*)c->host_mem;
    if (hm && hm->store) hm->store(hm->user, a, 4, v);
}
void recomp_set_flags(GuestContext* c,int is_sub,uint64_t a,uint64_t b,uint64_t r,int is64){
    uint64_t m=is64?~0ULL:0xFFFFFFFFULL; r&=m;a&=m;b&=m;
    uint64_t s=is64?0x8000000000000000ULL:0x80000000ULL;
    c->z=(r==0); c->n=(r&s)?1:0;
    if(is_sub){ c->c=(a>=b); c->v=(((a^b)&(a^r))&s)?1:0; }
    else { c->c=(r<a); c->v=((~(a^b)&(a^r))&s)?1:0; }
}
uint64_t recomp_umulh(uint64_t a,uint64_t b){
    uint64_t al=a&0xFFFFFFFFULL, ah=a>>32, bl=b&0xFFFFFFFFULL, bh=b>>32;
    uint64_t ll=al*bl, lh=al*bh, hl=ah*bl, hh=ah*bh;
    uint64_t mid=(ll>>32)+(lh&0xFFFFFFFFULL)+(hl&0xFFFFFFFFULL);
    return hh+(lh>>32)+(hl>>32)+(mid>>32);
}
uint64_t recomp_smulh(uint64_t a,uint64_t b){
    uint64_t hi=recomp_umulh(a,b);
    if((int64_t)a<0) hi-=b;
    if((int64_t)b<0) hi-=a;
    return hi;
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

void block_aot_proof(GuestContext* c) {
)C";
    src << t_proof_mov << t_proof_svc;
    src << R"C(}

void block_plain(GuestContext* c) {
)C";
    src << t_mov7 << t_svc1;
    src << R"C(}

void block_step_no_svc(GuestContext* c) {
)C";
    src << t_step_mov7;
    src << "    c->pc = g_module_base + 0x" << std::hex << (kOffStepNoSvc + 4) << std::dec
        << "ULL;\n";
    src << R"C(}

void block_core_dispatch(GuestContext* c) {
)C";
    src << t_core_mov << t_core_tls << t_core_marker << t_core_svc;
    src << R"C(}

void block_bench(GuestContext* c) {
)C";
    const auto bench_insns = BenchInsns();
    const auto t_bench = std::chrono::steady_clock::now();
    u64 bench_c = 0;
    for (const auto& [off, enc] : bench_insns) {
        bool unhandled = false;
        std::string body;
        suyu::recomp::Translate(enc, off, body, &unhandled);
        if (unhandled) {
            Fail("bench Translate unhandled encoding 0x" +
                 [&] {
                     std::ostringstream h;
                     h << std::hex << enc;
                     return h.str();
                 }());
        }
        src << body;
        bench_c += body.size();
    }
    g_aot_compile.bench_translate_ns = NsSince(t_bench);
    g_aot_compile.bench_c_bytes = bench_c;
    src << R"C(}
)C";

    const auto insn_blocks = suyu::recomp::insn_test::ReferenceBlocks();
    for (size_t bi = 0; bi < insn_blocks.size(); ++bi) {
        src << "void block_insn_" << bi << "(GuestContext* c) {\n";
        u64 pc = insn_blocks[bi].offset;
        for (u32 enc : insn_blocks[bi].insns) {
            bool unhandled = false;
            std::string body;
            suyu::recomp::Translate(enc, pc, body, &unhandled);
            if (unhandled) {
                Fail(std::string("insn block ") + insn_blocks[bi].name +
                     " Translate unhandled");
            }
            src << body;
            pc += 4;
        }
        src << "}\n";
    }
    return src.str();
}

bool BuildAndLoadAot(const fs::path& root) {
#ifdef _WIN32
    Fail("AOT shared library build requires POSIX dlopen in this harness");
    return false;
#else
    const fs::path src_dir = root / "aot_blocks";
    fs::create_directories(src_dir);
    const auto t_translate = std::chrono::steady_clock::now();
    const std::string aot_src = BuildAotSource();
    g_aot_compile.translate_ns = NsSince(t_translate);
    {
        const auto count = [&](std::string_view needle) {
            u64 n = 0;
            for (size_t p = 0; (p = aot_src.find(needle, p)) != std::string::npos;
                 p += needle.size()) {
                ++n;
            }
            return n;
        };
        ExpectTrue("bench C emits 512 recomp_store64 (not add-only)",
                   count("recomp_store64") >= static_cast<u64>(kBenchAdds));
        ExpectTrue("bench C emits 512 recomp_load64 (x0 reload)",
                   count("recomp_load64") >= static_cast<u64>(kBenchAdds));
    }
    if (!WriteFile(src_dir / "blocks.c", aot_src)) {
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
                   "  C_VISIBILITY_PRESET default)\n"
                   "if (CMAKE_C_COMPILER_ID MATCHES \"GNU|Clang\")\n"
                   "  target_compile_options(stack_aot PRIVATE\n"
                   "    $<$<CONFIG:Release>:-O3>)\n"
                   "endif()\n")) {
        return false;
    }

    const fs::path build = src_dir / "build";
    std::vector<std::string> cfg{SUYU_SMOKE_CMAKE, "-S", src_dir.string(), "-B", build.string(),
                                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_STANDARD=11"};
    AppendSanitizerCmakeArgs(cfg);
    const std::string gen = SUYU_SMOKE_GENERATOR;
    if (!gen.empty()) {
        cfg.push_back("-G");
        cfg.push_back(gen);
    }
    const std::string cc = SUYU_SMOKE_C_COMPILER;
    if (!cc.empty()) {
        cfg.push_back(std::string("-DCMAKE_C_COMPILER=") + cc);
    }
    const auto t_cfg = std::chrono::steady_clock::now();
    if (RunArgs(cfg) != 0) {
        Fail("cmake configure aot blocks");
        return false;
    }
    g_aot_compile.cmake_configure_ns = NsSince(t_cfg);
    const auto t_build = std::chrono::steady_clock::now();
    if (RunArgs({SUYU_SMOKE_CMAKE, "--build", build.string(), "--config", "Release", "--target",
                 "stack_aot"}) != 0) {
        Fail("cmake build aot blocks");
        return false;
    }
    g_aot_compile.cmake_build_ns = NsSince(t_build);

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
    std::error_code ec;
    const auto sz = fs::file_size(so, ec);
    if (ec) {
        Fail("file_size " + so.string() + ": " + ec.message());
        return false;
    }
    g_aot_compile.so_bytes = static_cast<u64>(sz);
    g_aot_compile.so_path = so.string();
    PinAotBenchNotFolded(so);
    PinClangDumpNotFalseFolded(src_dir / "blocks.c");

    const auto t_dl = std::chrono::steady_clock::now();
    g_so = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    g_aot_compile.dlopen_ns = NsSince(t_dl);
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
    g_block_aot_proof = reinterpret_cast<BlockFn>(dlsym(g_so, "block_aot_proof"));
    g_block_plain = reinterpret_cast<BlockFn>(dlsym(g_so, "block_plain"));
    g_block_step_no_svc = reinterpret_cast<BlockFn>(dlsym(g_so, "block_step_no_svc"));
    g_block_core_dispatch = reinterpret_cast<BlockFn>(dlsym(g_so, "block_core_dispatch"));
    g_block_bench = reinterpret_cast<BlockFn>(dlsym(g_so, "block_bench"));
    for (int i = 0; i < suyu::recomp::insn_test::kInsnBlockCount; ++i) {
        const std::string sym = "block_insn_" + std::to_string(i);
        g_block_insn[i] = reinterpret_cast<BlockFn>(dlsym(g_so, sym.c_str()));
        ExpectTrue(("dlsym " + sym).c_str(), g_block_insn[i] != nullptr);
    }
    g_hm_load_calls = static_cast<u64*>(dlsym(g_so, "g_recomp_hm_load_calls"));
    g_hm_store_calls = static_cast<u64*>(dlsym(g_so, "g_recomp_hm_store_calls"));
    ExpectTrue("dlsym recomp_set_module_base", g_set_base != nullptr);
    ExpectTrue("dlsym block_tls_svc", g_block_tls != nullptr);
    ExpectTrue("dlsym block_unhandled", g_block_unhandled != nullptr);
    ExpectTrue("dlsym block_miss", g_block_miss != nullptr);
    ExpectTrue("dlsym block_aot_proof", g_block_aot_proof != nullptr);
    ExpectTrue("dlsym block_plain", g_block_plain != nullptr);
    ExpectTrue("dlsym block_step_no_svc", g_block_step_no_svc != nullptr);
    ExpectTrue("dlsym block_core_dispatch", g_block_core_dispatch != nullptr);
    ExpectTrue("dlsym block_bench", g_block_bench != nullptr);
    ExpectTrue("dlsym g_recomp_hm_load_calls", g_hm_load_calls != nullptr);
    ExpectTrue("dlsym g_recomp_hm_store_calls", g_hm_store_calls != nullptr);
    return g_set_base && g_block_tls && g_block_unhandled && g_block_miss && g_block_aot_proof &&
           g_block_plain && g_block_step_no_svc && g_block_core_dispatch && g_block_bench &&
           g_hm_load_calls &&
           g_hm_store_calls && g_block_insn[0] &&
           g_block_insn[suyu::recomp::insn_test::kInsnBlockCount - 1];
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
    // AOT proof: guest RX is Dynarmic twin (BEEF/99); AOT Translate is #7/SVC1.
    put(kOffAotProof, kMovzX0Beef);
    put(kOffAotProof + 4, kSvc99);
    // Plain / icache site (guest RX matches Translate).
    put(kOffPlain, kMovzX0_7);
    put(kOffPlain + 4, kSvc1);
    put(kOffStepNoSvc, kMovzX0_7);
    put(kOffCoreDispatch + 0, kMovzX0_1234);
    put(kOffCoreDispatch + 4, kMrsX3Tpidrro);
    put(kOffCoreDispatch + 8, kMovzX1_55);
    put(kOffCoreDispatch + 12, kSvcGetCurrentProcessor);
    for (const auto& [off, enc] : BenchInsns()) {
        put(off, enc);
    }
    for (const auto& blk : suyu::recomp::insn_test::ReferenceBlocks()) {
        u64 off = blk.offset;
        for (u32 enc : blk.insns) {
            put(off, enc);
            off += 4;
        }
    }
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
        ExpectTrue("process ArmInterface is ArmRecomp", arm->IsRecompBackend());
        ExpectTrue("SetRecompLookup still set", Core::GetRecompLookup() == &Lookup);
        auto* recomp = AsRecomp(arm);
        ExpectTrue("AsRecomp after IsRecompBackend", recomp != nullptr);

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

        // LoadModule → SetProcessMemoryPermission → InvalidateCacheRange must
        // NOT permanently reject AOT (that is ClearInstructionCache only).
        ExpectTrue("AllowsAot survived LoadModule InvalidateCacheRange", recomp->AllowsAot());
        // Re-hit the same path explicitly so the fix is uniquely pinned even if
        // LoadModule's invalidate were ever skipped.
        for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; ++i) {
            if (auto* iface = process->GetArmInterface(i)) {
                ExpectTrue("core is ArmRecomp", iface->IsRecompBackend());
                iface->InvalidateCacheRange(g_entry, kCodeBytes);
                ExpectTrue("AllowsAot after explicit InvalidateCacheRange",
                           AsRecomp(iface)->AllowsAot());
            }
        }

        // Guest RX must hold the Translate'd encodings (not zeros) at twin sites;
        // AOT proof site deliberately diverges (BEEF/99 vs Translate #7/SVC1).
        ExpectEq("guest RX tls movz", system.ApplicationMemory().Read32(g_entry + kOffTlsSvc),
                 kMovzX0_1234);
        ExpectEq("guest RX tls svc", system.ApplicationMemory().Read32(g_entry + kOffTlsSvc + 24),
                 kSvc42);
        ExpectEq("guest RX miss", system.ApplicationMemory().Read32(g_entry + kOffMiss),
                 kMovzX0Beef);
        ExpectEq("guest RX aot-proof Dynarmic twin",
                 system.ApplicationMemory().Read32(g_entry + kOffAotProof), kMovzX0Beef);

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

void ScenarioAotLiveProof(StackFixture& f) {
    const int before = g_fails;
    auto* recomp = AsRecomp(f.arm);
    ExpectTrue("AOT-proof ArmRecomp", recomp != nullptr);
    ExpectTrue("AOT-proof AllowsAot", recomp && recomp->AllowsAot());

    // Guest RX at proof site is BEEF/99 — Dynarmic twin would yield those.
    ExpectEq("AOT-proof guest RX != Translate twin",
             f.system.ApplicationMemory().Read32(g_entry + kOffAotProof), kMovzX0Beef);
    ExpectTrue("AOT-proof guest svc twin is 99 encoding",
               f.system.ApplicationMemory().Read32(g_entry + kOffAotProof + 4) == kSvc99);

    const u64 lookups_before = g_lookup_calls.load(std::memory_order_relaxed);
    auto& ctx = f.thread->GetContext();
    ctx = {};
    ctx.pc = g_entry + kOffAotProof;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

    const auto hr = f.arm->RunThread(f.thread);
    const u64 lookups_after = g_lookup_calls.load(std::memory_order_relaxed);
    ExpectTrue("Lookup consulted during RunThread", lookups_after > lookups_before);
    ExpectTrue("AOT-proof svc HaltReason", True(hr & Core::HaltReason::SupervisorCall));
    // Translate AOT outcome (#7 / svc 1), not Dynarmic twin (BEEF / 99).
    ExpectEq("AOT-proof svc (not Dynarmic twin 99)", f.arm->GetSvcNumber(), 1);
    Kernel::Svc::ThreadContext out{};
    f.arm->GetContext(out);
    ExpectEq("AOT-proof x0 (not Dynarmic twin 0xBEEF)", out.r[0], 7);
    ScenarioPass("ArmRecomp Translate AOT live (Lookup + outcome != guest RX twin)", before);
}

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

void ScenarioPhysicalCoreDispatch(StackFixture& f) {
    const int before = g_fails;
    auto& kernel = f.system.Kernel();

    // Feed both fixture threads through the real priority queue before this
    // host thread is registered as an emulated core. Keeping the outer lock
    // held lets the harness inspect both deterministic selection boundaries
    // without updating scheduler execution state or yielding to a CpuManager
    // guest fiber that it does not own.
    f.thread->SetPriority(20);
    f.thread_b->SetPriority(10);
    {
        Kernel::KScopedSchedulerLock lock(kernel);
        f.thread->SetState(kernel, Kernel::ThreadState::Runnable);
        ExpectTrue("scheduler selected initial fixture thread on core 0",
                   kernel.GlobalSchedulerContext().GetScheduledFront(0) == f.thread);

        f.thread_b->SetState(kernel, Kernel::ThreadState::Runnable);
        ExpectTrue("scheduler selected higher-priority fixture thread on core 0",
                   kernel.GlobalSchedulerContext().GetScheduledFront(0) == f.thread_b);

        f.thread->SetState(kernel, Kernel::ThreadState::Initialized);
        f.thread_b->SetState(kernel, Kernel::ThreadState::Initialized);
    }

    // Run on a registered emulated core so PhysicalCore::RunThread's Svc::Call
    // observes the same current core/process context as CpuManager's guest
    // loop. SVC #0x10 is a side-effect-free kernel service and therefore makes
    // a redistributable, deterministic real-dispatch fixture.
    f.system.RegisterCoreThread(0);
    const auto run = [&](Kernel::KThread* t, u64 expected_tls, const char* label) {
        Kernel::SetCurrentThread(kernel, t);
        auto& ctx = t->GetContext();
        ctx = {};
        ctx.pc = g_entry + kOffCoreDispatch;
        kernel.PhysicalCore(0).LoadContext(t);
        kernel.PhysicalCore(0).RunThread(kernel, t);

        f.arm = t->GetOwnerProcess()->GetArmInterface(0);
        ExpectEq((std::string(label) + " SVC number").c_str(), f.arm->GetSvcNumber(), 0x10);
        Kernel::Svc::ThreadContext out{};
        f.arm->GetContext(out);
        ExpectEq((std::string(label) + " SVC result core").c_str(), out.r[0], 0);
        ExpectEq((std::string(label) + " TLS").c_str(), out.r[3], expected_tls);
        ExpectEq((std::string(label) + " marker").c_str(), out.r[1], 0x55);
    };

    run(f.thread, GetInteger(f.thread->GetTlsAddress()), "scheduler thread A");
    run(f.thread_b, GetInteger(f.thread_b->GetTlsAddress()), "scheduler thread B");
    ExpectTrue("scheduler thread TLS values distinct",
               GetInteger(f.thread->GetTlsAddress()) != GetInteger(f.thread_b->GetTlsAddress()));
    Kernel::SetCurrentThread(kernel, f.thread);
    ScenarioPass("PhysicalCore scheduler dispatch -> ArmRecomp -> Svc::Call (A/B TLS)", before);
}

void ScenarioLeftoverSvcStep(StackFixture& f) {
    const int before = g_fails;
    // ScenarioSvcTlsCrossPage left pending_svc=42. LoadContext does not clear it.
    ExpectEq("leftover svc still parked", f.arm->GetSvcNumber(), 42);

    auto& ctx = f.thread->GetContext();
    ctx = {};
    ctx.pc = g_entry + kOffStepNoSvc;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

    const auto hr = f.arm->StepThread(f.thread);
    ExpectTrue("leftover-svc step is StepThread, not SupervisorCall",
               True(hr & Core::HaltReason::StepThread));
    ExpectTrue("leftover-svc step is not SupervisorCall",
               !True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("leftover svc cleared (not this step)", f.arm->GetSvcNumber(),
             static_cast<u32>(~0u));
    Kernel::Svc::ThreadContext out{};
    f.arm->GetContext(out);
    ExpectEq("leftover-svc step x0", out.r[0], 7);
    ExpectEq("leftover-svc step pc", out.pc, g_entry + kOffStepNoSvc + 4);
    ScenarioPass("StepThread clears leftover pending_svc before a non-SVC AOT block", before);
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
            ExpectTrue("inv Clear target is ArmRecomp", iface->IsRecompBackend());
            ExpectTrue("AllowsAot before Clear", AsRecomp(iface)->AllowsAot());
            iface->ClearInstructionCache();
            ExpectTrue("AllowsAot false after Clear only", !AsRecomp(iface)->AllowsAot());
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

void FillThreadRegs(Kernel::Svc::ThreadContext& ctx, const u64 x[32]) {
    ctx = {};
    for (int i = 0; i < 29; ++i) {
        ctx.r[static_cast<size_t>(i)] = x[i];
    }
    ctx.fp = x[29];
    ctx.lr = x[30];
    ctx.sp = x[31];
    ctx.pstate = (1u << 29) | (1u << 28); // C=V=1; ANDS must clear them (A64/Dynarmic)
}

u32 Nzcv(const Kernel::Svc::ThreadContext& ctx) {
    return ctx.pstate & 0xF0000000u;
}

u64 Gpr(const Kernel::Svc::ThreadContext& ctx, int i) {
    if (i < 29) {
        return ctx.r[static_cast<size_t>(i)];
    }
    if (i == 29) {
        return ctx.fp;
    }
    if (i == 30) {
        return ctx.lr;
    }
    return ctx.sp;
}

bool SameGprsNzcv(const Kernel::Svc::ThreadContext& a, const Kernel::Svc::ThreadContext& b,
                  const char* tag) {
    bool ok = true;
    for (int i = 0; i < 32; ++i) {
        if (Gpr(a, i) != Gpr(b, i)) {
            Fail(std::string(tag) + " x" + std::to_string(i) + " aot=" +
                 std::to_string(Gpr(a, i)) + " dyn=" + std::to_string(Gpr(b, i)));
            ok = false;
        }
    }
    if (a.pc != b.pc) {
        Fail(std::string(tag) + " pc aot=" + std::to_string(a.pc) + " dyn=" + std::to_string(b.pc));
        ok = false;
    }
    if (Nzcv(a) != Nzcv(b)) {
        Fail(std::string(tag) + " nzcv aot=" + std::to_string(Nzcv(a)) +
             " dyn=" + std::to_string(Nzcv(b)));
        ok = false;
    }
    return ok;
}

struct InsnSnap {
    Kernel::Svc::ThreadContext ctx{};
    u32 svc{};
    bool halt_svc{};
    u64 mem0{};
    u8 mem8{};
};

InsnSnap RunInsnBackend(StackFixture& f, u64 pc, bool aot, const Kernel::Svc::ThreadContext& init) {
    g_force_miss_pc = aot ? 0 : pc;
    auto& tctx = f.thread->GetContext();
    tctx = init;
    tctx.pc = pc;
    f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);
    const auto hr = f.arm->RunThread(f.thread);
    g_force_miss_pc = 0;
    InsnSnap s;
    f.arm->GetContext(s.ctx);
    s.svc = f.arm->GetSvcNumber();
    s.halt_svc = True(hr & Core::HaltReason::SupervisorCall);
    s.mem0 = f.system.ApplicationMemory().Read64(g_entry + kOffInsnScratch);
    s.mem8 = static_cast<u8>(f.system.ApplicationMemory().Read8(g_entry + kOffInsnScratch + 8));
    return s;
}

void SeedInsnScratch(StackFixture& f, u64 word, u8 b) {
    f.system.ApplicationMemory().Write64(g_entry + kOffInsnScratch, word);
    f.system.ApplicationMemory().Write8(g_entry + kOffInsnScratch + 8, b);
}

void ScenarioInsnCorrectness(StackFixture& f) {
    const int before = g_fails;
    const auto blocks = suyu::recomp::insn_test::ReferenceBlocks();
    ExpectEq("insn block count", static_cast<u64>(blocks.size()),
             static_cast<u64>(suyu::recomp::insn_test::kInsnBlockCount));

    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const u64 pc = g_entry + blocks[bi].offset;
        ExpectEq(("guest RX insn " + std::string(blocks[bi].name)).c_str(),
                 f.system.ApplicationMemory().Read32(pc), blocks[bi].insns.front());
        ExpectTrue(("AOT registered " + std::string(blocks[bi].name)).c_str(),
                   Lookup(pc) == g_block_insn[bi] && g_block_insn[bi] != nullptr);
        const std::string_view name = blocks[bi].name;
        if (name == "b_cbz" || name == "b_cbnz" || name == "b_tbz" || name == "b_tbnz" ||
            name == "b_beq") {
            ExpectEq((std::string(name) + " stride-sized AOT layout").c_str(),
                     static_cast<u64>(blocks[bi].insns.size()),
                     suyu::recomp::insn_test::kInsnStride / 4);
            ExpectTrue((std::string(name) + " AOT fallthrough registered").c_str(),
                       Lookup(pc + suyu::recomp::insn_test::kInsnStride) != nullptr);
            ExpectTrue((std::string(name) + " AOT taken target registered").c_str(),
                       Lookup(g_entry + suyu::recomp::insn_test::kOffInsn +
                                  18 * suyu::recomp::insn_test::kInsnStride) != nullptr);
        }
    }

    suyu::recomp::insn_test::XorShift64 rng(suyu::recomp::insn_test::RandomSeed());
    const int trials = suyu::recomp::insn_test::RandomTrials();

    auto one = [&](const suyu::recomp::insn_test::RefBlock& blk, const u64 x[32], const char* tag) {
        Kernel::Svc::ThreadContext init{};
        FillThreadRegs(init, x);
        init.pc = g_entry + blk.offset;
        SeedInsnScratch(f, 0x1111222233334444ULL, 0x80);
        const auto aot_metrics_before = Core::GetRecompExecutionMetrics();
        const InsnSnap aot = RunInsnBackend(f, g_entry + blk.offset, true, init);
        const auto aot_metrics_after = Core::GetRecompExecutionMetrics();
        SeedInsnScratch(f, 0x1111222233334444ULL, 0x80);
        const InsnSnap dyn = RunInsnBackend(f, g_entry + blk.offset, false, init);
        if (!aot.halt_svc || !dyn.halt_svc) {
            Fail(std::string(tag) + " missing SupervisorCall aot=" +
                 std::to_string(aot.halt_svc) + " dyn=" + std::to_string(dyn.halt_svc));
        }
        if (aot.svc != suyu::recomp::insn_test::kInsnSvcImm ||
            dyn.svc != suyu::recomp::insn_test::kInsnSvcImm) {
            Fail(std::string(tag) + " svc aot=" + std::to_string(aot.svc) +
                 " dyn=" + std::to_string(dyn.svc));
        }
        SameGprsNzcv(aot.ctx, dyn.ctx, tag);
        if (std::string_view(blk.name) == "logic_flags" ||
            std::string_view(blk.name) == "p0_logic_flags") {
            // FillThreadRegs presets C=V=1. A64/Dynarmic ANDS/BICS write C=V=0.
            // This fails if AOT left those bits stale even when N/Z match.
            if ((Nzcv(aot.ctx) & 0x30000000u) != 0) {
                Fail(std::string(tag) + " AOT ANDS left C/V stale nzcv=" +
                     std::to_string(Nzcv(aot.ctx)));
            }
            if ((Nzcv(dyn.ctx) & 0x30000000u) != 0) {
                Fail(std::string(tag) + " Dynarmic ANDS C/V not 0 nzcv=" +
                     std::to_string(Nzcv(dyn.ctx)));
            }
        }
        if (aot.mem0 != dyn.mem0 || aot.mem8 != dyn.mem8) {
            Fail(std::string(tag) + " mem mismatch");
        }
        const std::string_view name = blk.name;
        if (name == "b_cbz" || name == "b_cbnz" || name == "b_tbz" || name == "b_tbnz" ||
            name == "b_beq") {
            bool taken = false;
            u64 fallthrough_marker = 0;
            if (name == "b_cbz") {
                taken = x[0] == 0;
                fallthrough_marker = 0xC1;
            } else if (name == "b_cbnz") {
                taken = x[0] != 0;
                fallthrough_marker = 0xC2;
            } else if (name == "b_tbz") {
                taken = (x[0] & (1ULL << 3)) == 0;
                fallthrough_marker = 0xC3;
            } else if (name == "b_tbnz") {
                taken = (x[0] & (1ULL << 3)) != 0;
                fallthrough_marker = 0xC4;
            } else {
                taken = x[0] == x[1];
                fallthrough_marker = 0xC5;
            }
            const u64 marker = Gpr(aot.ctx, taken ? 11 : 10);
            ExpectEq((std::string(tag) + " branch marker").c_str(), marker,
                     taken ? 0xB7 : fallthrough_marker);
            ExpectEq((std::string(tag) + " AOT branch fallback lookup delta").c_str(),
                     aot_metrics_after.fallback_lookup_miss -
                         aot_metrics_before.fallback_lookup_miss,
                     0);
        }
    };

    for (const auto& blk : blocks) {
        u64 edge[32]{};
        if (std::string_view(blk.name) == "edge_sdiv") {
            edge[1] = 0x8000000000000000ULL; // INT64_MIN
            edge[2] = ~0ULL;                 // -1
            edge[4] = 0xFFFFFFFF80000000ULL; // INT32_MIN in W4
            edge[5] = ~0ULL;
        } else if (std::string_view(blk.name) == "mem") {
            edge[1] = g_entry + kOffInsnScratch;
            edge[2] = 0xA1B2C3D4E5F60718ULL;
            edge[3] = 0x9E; // STRB
        } else {
            edge[1] = ~0ULL;
            edge[2] = 1;
            edge[3] = 3;
            edge[4] = 0xFFFFFFFFULL;
            edge[5] = 1;
            edge[13] = static_cast<u64>(static_cast<int32_t>(-5)); // BIC ASR#31
        }
        if (std::string_view(blk.name) == "shift_div") {
            edge[1] = 0x8000000000000000ULL;
            edge[2] = 1;
        }
        if (std::string_view(blk.name) == "b_cbz" || std::string_view(blk.name) == "b_tbz") {
            edge[0] = 0; // taken: CBZ X0==0, TBZ bit 3 clear
        } else if (std::string_view(blk.name) == "b_cbnz") {
            edge[0] = 1; // taken: CBNZ X0!=0
        } else if (std::string_view(blk.name) == "b_tbnz") {
            edge[0] = 8; // taken: TBNZ bit 3 set
        } else if (std::string_view(blk.name) == "b_beq") {
            edge[0] = 0x1234;
            edge[1] = 0x1234; // taken: X0==X1
        }
        one(blk, edge, (std::string(blk.name) + " edge").c_str());

        u64 wrap[32]{};
        wrap[1] = 0xFFFFFFFFULL;
        wrap[2] = 1;
        wrap[3] = 7;
        wrap[4] = 0xFFFFFFFFULL;
        wrap[5] = 1;
        if (std::string_view(blk.name) == "mem") {
            wrap[1] = g_entry + kOffInsnScratch;
            wrap[2] = ~0ULL;
            wrap[3] = 0xFF;
        }
        if (std::string_view(blk.name) == "b_cbz") {
            wrap[0] = 1; // not-taken: CBZ X0!=0
        } else if (std::string_view(blk.name) == "b_cbnz" ||
                   std::string_view(blk.name) == "b_tbnz") {
            wrap[0] = 0; // not-taken: CBNZ X0==0, TBNZ bit 3 clear
        } else if (std::string_view(blk.name) == "b_tbz") {
            wrap[0] = 8; // not-taken: TBZ bit 3 set
        } else if (std::string_view(blk.name) == "b_beq") {
            wrap[0] = 1;
            wrap[1] = 2; // not-taken: X0!=X1
        }
        one(blk, wrap, (std::string(blk.name) + " wrap").c_str());

        for (int t = 0; t < trials; ++t) {
            u64 rnd[32]{};
            for (int r = 0; r < 16; ++r) {
                rnd[r] = rng.next();
            }
            if (std::string_view(blk.name) == "mem") {
                rnd[1] = g_entry + kOffInsnScratch;
            }
            if (std::string_view(blk.name) == "shift_div" ||
                std::string_view(blk.name) == "edge_sdiv") {
                // Keep divisors from exploding the comparison: still random, but
                // include 0 often enough via the wrap/edge cases.
                if (rnd[2] == 0) {
                    rnd[2] = 3;
                }
                if ((rnd[5] & 0xFFFFFFFFULL) == 0) {
                    rnd[5] = 5;
                }
            }
            one(blk, rnd, (std::string(blk.name) + " rand" + std::to_string(t)).c_str());
            if (g_fails != before) {
                break;
            }
        }
        if (g_fails != before) {
            break;
        }
    }

    ScenarioPass("Translate AOT vs Dynarmic instruction correctness (edge + random)", before);
    Pass("ANDS C/V: AOT writes 0 (A64/Dynarmic); full NZCV vs Dynarmic; stale C/V fails");
}

void ExportExecutionJson(const fs::path& path) {
    const int before = g_fails;
    if (!Core::WriteRecompExecutionJson(path)) {
        Fail("WriteRecompExecutionJson " + path.string());
        return;
    }
    Pass("wrote " + path.string());
    ExpectTrue("JSON overwrite via tmp+rename", Core::WriteRecompExecutionJson(path));
    const fs::path def = Core::DefaultRecompExecutionJsonPath();
    if (def != path) {
        ExpectTrue("also wrote default LogDir JSON", Core::WriteRecompExecutionJson({}));
    }

    std::ifstream in(path);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ExpectTrue("JSON file not empty", !json.empty());
    ExpectTrue("JSON schema_version 1", json.find("\"schema_version\": 1") != std::string::npos);
    ExpectTrue("JSON kind recomp_execution", json.find("\"kind\": \"recomp_execution\"") != std::string::npos);
    ExpectTrue("JSON clock steady_clock", json.find("\"clock\": \"steady_clock\"") != std::string::npos);
    ExpectTrue("JSON backends.aot", json.find("\"aot\"") != std::string::npos);
    ExpectTrue("JSON backends.dynarmic", json.find("\"dynarmic\"") != std::string::npos);
    ExpectTrue("JSON fallback_reasons", json.find("\"fallback_reasons\"") != std::string::npos);
    ExpectTrue("JSON icache", json.find("\"icache\"") != std::string::npos);

    const auto m = Core::GetRecompExecutionMetrics();
    ExpectEq("metrics schema", static_cast<u64>(Core::RecompExecutionMetrics::kSchemaVersion), 1);
    ExpectTrue("AOT block_executions > 0", m.aot_block_executions > 0);
    ExpectTrue("AOT time_ns > 0", m.aot_time_ns > 0);
    ExpectTrue("Dynarmic run+step slices > 0",
               (m.dynarmic_run_slices + m.dynarmic_step_slices) > 0);
    ExpectTrue("Dynarmic time_ns > 0", m.dynarmic_time_ns > 0);
    ExpectTrue("aot_to_dynarmic > 0", m.aot_to_dynarmic > 0);
    ExpectTrue("fallback lookup_miss > 0", m.fallback_lookup_miss > 0);
    ExpectTrue("fallback unhandled_opcode > 0", m.fallback_unhandled_opcode > 0);
    ExpectTrue("fallback icache_rejected > 0", m.fallback_icache_rejected > 0);
    ExpectTrue("ClearInstructionCache recorded", m.clear_instruction_cache_calls > 0);
    ExpectTrue("InvalidateCacheRange recorded (not a permanent reject)",
               m.invalidate_cache_range_calls > 0);
    ExpectTrue("permanent AOT reject recorded", m.permanent_aot_reject_events > 0);

    std::cout << "recomp_execution.json path: " << path << "\n";
    std::cout << "also: " << Core::DefaultRecompExecutionJsonPath() << "\n";
    std::cout << "=== recomp_execution.json ===\n" << json << std::endl;
    ScenarioPass("AOT/JIT execution JSON from live ArmRecomp stack", before);
}

nlohmann::json MetricsToJson(const Core::RecompExecutionMetrics& m) {
    return {
        {"backends",
         {{"aot", {{"block_executions", m.aot_block_executions}, {"time_ns", m.aot_time_ns}}},
          {"dynarmic",
           {{"run_slices", m.dynarmic_run_slices},
            {"step_slices", m.dynarmic_step_slices},
            {"time_ns", m.dynarmic_time_ns}}}}},
        {"transitions",
         {{"aot_to_dynarmic", m.aot_to_dynarmic}, {"dynarmic_to_aot", m.dynarmic_to_aot}}},
        {"fallback_reasons",
         {{"lookup_miss", m.fallback_lookup_miss},
          {"unhandled_opcode", m.fallback_unhandled_opcode},
          {"icache_rejected", m.fallback_icache_rejected},
          {"no_fallback_available", m.fallback_no_backend}}},
        {"svc_calls", m.svc_calls},
    };
}

Core::RecompExecutionMetrics MetricsDelta(const Core::RecompExecutionMetrics& after,
                                         const Core::RecompExecutionMetrics& before) {
    Core::RecompExecutionMetrics d;
    const auto sub = [](u64 a, u64 b) { return a >= b ? a - b : 0; };
    d.aot_block_executions = sub(after.aot_block_executions, before.aot_block_executions);
    d.aot_time_ns = sub(after.aot_time_ns, before.aot_time_ns);
    d.dynarmic_run_slices = sub(after.dynarmic_run_slices, before.dynarmic_run_slices);
    d.dynarmic_step_slices = sub(after.dynarmic_step_slices, before.dynarmic_step_slices);
    d.dynarmic_time_ns = sub(after.dynarmic_time_ns, before.dynarmic_time_ns);
    d.aot_to_dynarmic = sub(after.aot_to_dynarmic, before.aot_to_dynarmic);
    d.dynarmic_to_aot = sub(after.dynarmic_to_aot, before.dynarmic_to_aot);
    d.fallback_lookup_miss = sub(after.fallback_lookup_miss, before.fallback_lookup_miss);
    d.fallback_unhandled_opcode =
        sub(after.fallback_unhandled_opcode, before.fallback_unhandled_opcode);
    d.fallback_icache_rejected =
        sub(after.fallback_icache_rejected, before.fallback_icache_rejected);
    d.fallback_no_backend = sub(after.fallback_no_backend, before.fallback_no_backend);
    d.svc_calls = sub(after.svc_calls, before.svc_calls);
    return d;
}

struct SliceStats {
    u64 count{};
    u64 first_ns{};
    u64 min_ns{};
    u64 max_ns{};
    u64 sum_ns{};
    u64 mean_ns{};
    u64 median_ns{};
    u64 p95_ns{};
};

SliceStats SummarizeSlices(std::vector<u64> samples) {
    SliceStats s;
    if (samples.empty()) {
        return s;
    }
    s.count = samples.size();
    s.first_ns = samples.front();
    s.sum_ns = std::accumulate(samples.begin(), samples.end(), u64{0});
    s.mean_ns = s.sum_ns / s.count;
    s.min_ns = *std::min_element(samples.begin(), samples.end());
    s.max_ns = *std::max_element(samples.begin(), samples.end());
    std::sort(samples.begin(), samples.end());
    s.median_ns = samples[samples.size() / 2];
    const size_t p95_i = (samples.size() * 95) / 100;
    s.p95_ns = samples[std::min(p95_i, samples.size() - 1)];
    return s;
}

nlohmann::json SliceStatsToJson(const SliceStats& s) {
    return {
        {"count", s.count},
        {"first_ns", s.first_ns},
        {"min_ns", s.min_ns},
        {"median_ns", s.median_ns},
        {"mean_ns", s.mean_ns},
        {"p95_ns", s.p95_ns},
        {"max_ns", s.max_ns},
        {"sum_ns", s.sum_ns},
        {"unit", "host_steady_clock_ns_per_RunThread"},
        {"note", "No GPU frames in this harness; one RunThread until SVC is one slice/tick."},
    };
}

struct ModeResult {
    const char* id = "";
    const char* backend = "";
    const char* description = "";
    SliceStats slices{};
    u64 startup_ns{};
    u64 compile_ns{};
    MemSnap mem_before{};
    MemSnap mem_after_first{};
    MemSnap mem_after{};
    u64 x0{};
    u32 svc{};
    bool halt_svc{false};
    Core::RecompExecutionMetrics exec{};
    u64 hm_load_calls{};
    u64 hm_store_calls{};
    u64 expected_x0{};
    u32 expected_svc{};
    u64 frame_events{};
    u64 frame_event_time_ns{};
    double perf_stats_frametime_seconds{};
};

ModeResult RunIdenticalWorkload(StackFixture& f, bool hybrid_aot, int iters) {
    ModeResult r;
    r.id = hybrid_aot ? "hybrid_aot" : "jit";
    r.backend = hybrid_aot ? "hybrid_aot" : "jit";
    r.expected_x0 = kBenchAdds;
    r.expected_svc = kBenchSvcImm;
    r.description = hybrid_aot
                        ? "ArmRecomp Translate AOT lookup hit; Dynarmic fallback must not run"
                        : "ArmRecomp force-miss of the same PC; Dynarmic executes guest RX";
    g_force_miss_pc = hybrid_aot ? 0 : (g_entry + kOffBench);

    const u64 hm_load0 = HmLoadCalls();
    const u64 hm_store0 = HmStoreCalls();
    const auto before = Core::GetRecompExecutionMetrics();
    r.mem_before = ReadMem();

    std::vector<u64> times;
    times.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        auto& ctx = f.thread->GetContext();
        ctx = {};
        ctx.pc = g_entry + kOffBench;
        ctx.r[1] = 1;
        ctx.r[3] = g_entry + kOffBenchScratch;
        f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);

        const auto frame_t0 = std::chrono::steady_clock::now();
        f.system.GetPerfStats().BeginSystemFrame();
        const auto slice_t0 = std::chrono::steady_clock::now();
        const auto hr = f.arm->RunThread(f.thread);
        const u64 ns = NsSince(slice_t0);
        f.system.GetPerfStats().EndSystemFrame();
        ++r.frame_events;
        r.frame_event_time_ns += NsSince(frame_t0);
        times.push_back(ns);
        if (i == 0) {
            r.mem_after_first = ReadMem();
        }

        const bool svc_halt = True(hr & Core::HaltReason::SupervisorCall);
        r.halt_svc = r.halt_svc || svc_halt;
        r.svc = f.arm->GetSvcNumber();
        Kernel::Svc::ThreadContext out{};
        f.arm->GetContext(out);
        r.x0 = out.r[0];
        if (!svc_halt) {
            Fail(std::string(r.id) + " slice " + std::to_string(i) + " not SupervisorCall");
        }
        if (r.svc != kBenchSvcImm) {
            Fail(std::string(r.id) + " svc got=" + std::to_string(r.svc) +
                 " want=" + std::to_string(kBenchSvcImm));
        }
        if (r.x0 != static_cast<u64>(kBenchAdds)) {
            Fail(std::string(r.id) + " x0 got=" + std::to_string(r.x0) +
                 " want=" + std::to_string(kBenchAdds));
        }
    }
    g_force_miss_pc = 0;
    r.mem_after = ReadMem();
    r.slices = SummarizeSlices(std::move(times));
    r.startup_ns = r.slices.first_ns;
    r.exec = MetricsDelta(Core::GetRecompExecutionMetrics(), before);
    r.hm_load_calls = HmLoadCalls() - hm_load0;
    r.hm_store_calls = HmStoreCalls() - hm_store0;
    if (hybrid_aot) {
        r.compile_ns = g_aot_compile.translate_ns + g_aot_compile.cmake_configure_ns +
                       g_aot_compile.cmake_build_ns + g_aot_compile.dlopen_ns;
    } else {
        const u64 steady = r.slices.median_ns;
        r.compile_ns = r.slices.first_ns > steady ? r.slices.first_ns - steady : 0;
    }
    return r;
}

nlohmann::json ModeToJson(const ModeResult& r) {
    const std::int64_t rss_delta =
        static_cast<std::int64_t>(r.mem_after.vmrss_kb) - static_cast<std::int64_t>(r.mem_before.vmrss_kb);
    const std::int64_t rss_first_delta = static_cast<std::int64_t>(r.mem_after_first.vmrss_kb) -
                                static_cast<std::int64_t>(r.mem_before.vmrss_kb);
    nlohmann::json generated;
    if (std::string_view(r.backend) == "hybrid_aot") {
        generated = {
            {"aot_so_bytes", g_aot_compile.so_bytes},
            {"aot_so_path", g_aot_compile.so_path},
            {"bench_generated_c_bytes", g_aot_compile.bench_c_bytes},
            {"note", "so includes integration blocks as well as the bench block"},
        };
    } else {
        generated = {
            {"jit_rss_delta_after_first_slice_bytes", rss_first_delta * 1024},
            {"jit_rss_delta_after_all_slices_bytes", rss_delta * 1024},
            {"note", "RSS delta around first Dynarmic slice (code-cache commit + this block). "
                     "Dynarmic reserves up to 128MiB code cache; RSS is committed pages."},
        };
    }
    return {
        {"backend", r.backend},
        {"execution_backend", r.backend},
        {"description", r.description},
        {"slices", SliceStatsToJson(r.slices)},
        {"startup_ns", r.startup_ns},
        {"compile_ns", std::string_view(r.backend) == "hybrid_aot"
                           ? nlohmann::json(nullptr)
                           : nlohmann::json(r.compile_ns)},
        {"compile_ns_meaning",
         std::string_view(r.backend) == "hybrid_aot"
             ? "unavailable per workload; see top-level aot_compile shared aggregate"
             : "approx first-JIT compile: first_slice_ns - median_ns"},
        {"compile_ns_scope", std::string_view(r.backend) == "hybrid_aot"
                                 ? "shared_aggregate"
                                 : "workload"},
        {"memory",
         {{"sampler", "/proc/self/status"},
          {"rss_kb_before", r.mem_before.vmrss_kb},
          {"rss_kb_after_first_slice", r.mem_after_first.vmrss_kb},
          {"rss_kb_after", r.mem_after.vmrss_kb},
          {"rss_kb_delta", rss_delta},
          {"hwm_kb_after", r.mem_after.vmhwm_kb},
          {"vmsize_kb_after", r.mem_after.vmsize_kb}}},
        {"generated_binary", std::move(generated)},
        {"correctness",
         {{"x0", r.x0},
          {"svc", r.svc},
          {"halt_supervisor_call", r.halt_svc},
          {"expected_x0", r.expected_x0},
          {"expected_svc", r.expected_svc}}},
        {"execution_metrics_delta", MetricsToJson(r.exec)},
        {"frame_events",
         {{"count", r.frame_events},
          {"wall_time_ns", r.frame_event_time_ns},
          {"source", "Core::PerfStats BeginSystemFrame/EndSystemFrame event boundary"},
          {"note", "Standalone harness has no renderer/display; these are explicit emulated frame-event boundaries, not renamed RunThread slices."}}},
        {"host_mem_callbacks",
         {{"load", r.hm_load_calls},
          {"store", r.hm_store_calls},
          {"note", "recomp_load64/store64 increments around host_mem function-pointer "
                   "calls; AOT bench must be 512 x iters; JIT must be 0"}}},
    };
}

void AddRepresentativeWorkloads(nlohmann::json& doc, StackFixture& f, int iters);

void ExportBenchmarkJson(const fs::path& path, const ModeResult& aot, const ModeResult& jit,
                         int iters, StackFixture& f) {
    const int before = g_fails;
    std::ostringstream entry_pc;
    entry_pc << "0x" << std::hex << (g_entry + kOffBench);
    nlohmann::json doc{
        {"schema_version", 1},
        {"kind", "recomp_benchmark"},
        {"clock", "steady_clock"},
        {"related",
         {{"recomp_execution", "schema_version 1 kind=recomp_execution from ArmRecomp (#2)"},
          {"note", "Per-mode execution_metrics_delta is a GetRecompExecutionMetrics() window "
                   "around that mode's slices, not the mixed-scenario recomp_execution.json."}}},
        {"host", {{"cpu", HostCpu()}, {"rss_sampler", "/proc/self/status VmRSS/VmHWM/VmSize (kB)"}}},
        {"workload",
         {{"name", "alu_add_reload_svc"},
          {"identical_across_backends", true},
          {"anti_fold",
           "each ADD is preceded by STR/LDR of x0 through host_mem function pointers "
           "so -O3 cannot reduce 512 ADDs to x1<<9; JudgeAotBenchDump FAILs a "
           "discriminating store+load+1 add+PLT jmp dump (PLT is not a loop); "
           "named recomp_store64@plt is not the live pin; host_mem callbacks "
           "must be 512 x iters"},
          {"guest_pc_offset", "0x2400"},
          {"entry_pc", entry_pc.str()},
          {"guest_instruction_count", kBenchAdds * 3 + 2},
          {"encodings",
           nlohmann::json::array({
               {{"asm", "movz x0, #0"}, {"encoding", "0xD2800000"}},
               {{"asm", "str x0, [x3]"}, {"encoding", "0xF9000060"}, {"repeat", kBenchAdds}},
               {{"asm", "ldr x0, [x3]"}, {"encoding", "0xF9400060"}, {"repeat", kBenchAdds}},
               {{"asm", "add x0, x0, x1"},
                {"encoding", "0x8B010000"},
                {"repeat", kBenchAdds},
                {"x1", 1}},
               {{"asm", "svc #3"}, {"encoding", "0xD4000061"}},
           })},
          {"expected_x0", kBenchAdds},
          {"expected_svc", kBenchSvcImm},
          {"iters", iters}}},
        {"aot_compile",
         {{"scope", "shared_aot_image"},
          {"per_workload", "unavailable"},
          {"translate_ns", g_aot_compile.translate_ns},
          {"bench_translate_ns", g_aot_compile.bench_translate_ns},
          {"cmake_configure_ns", g_aot_compile.cmake_configure_ns},
          {"cmake_build_ns", g_aot_compile.cmake_build_ns},
          {"dlopen_ns", g_aot_compile.dlopen_ns},
          {"so_bytes", g_aot_compile.so_bytes},
          {"so_path", g_aot_compile.so_path},
          {"bench_generated_c_bytes", g_aot_compile.bench_c_bytes},
          {"host_opt_check",
           {{"method", "objdump -d <block_bench> + JudgeAotBenchDump + host_mem callback counts"},
            {"pin",
             "FAIL dump if shl $0x9 / imul $512, or if too few adds and no in-function "
             "loop. A jmp to a lower recomp_svc@plt is plt_jumps not a loop "
             "(discriminating dump: store+load + 1 add + PLT jmp + no shl/imul). "
             "Named recomp_store64@plt is gcc PIC, not required for PASS. Live "
             "store/load pin is host_mem_callbacks == 512 * iters."},
            {"add_mnemonics", g_aot_compile.disasm_add_count},
            {"shift_by_9", g_aot_compile.disasm_shl9_count},
            {"imul_512", g_aot_compile.disasm_imul512_count},
            {"backward_jumps", g_aot_compile.disasm_backward_jumps},
            {"plt_jumps", g_aot_compile.disasm_plt_jumps},
            {"named_store64_plt", g_aot_compile.disasm_store64_calls},
            {"named_load64_plt", g_aot_compile.disasm_load64_calls},
            {"not_single_shift", g_aot_compile.not_single_shift},
            {"clang_o3",
             {{"add_mnemonics", g_aot_compile.clang_disasm_add},
              {"named_store64_plt", g_aot_compile.clang_disasm_store64},
              {"named_load64_plt", g_aot_compile.clang_disasm_load64},
              {"not_reported_folded", g_aot_compile.clang_not_reported_folded},
              {"reason", g_aot_compile.clang_dump_reason}}},
            {"excerpt", g_aot_compile.disasm_excerpt}}}}},
        {"modes", {{"hybrid_aot", ModeToJson(aot)}, {"jit", ModeToJson(jit)}}},
    };
    AddRepresentativeWorkloads(doc, f, std::max(4, std::min(iters, 16)));

    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    {
        std::ofstream out(path);
        if (!out) {
            Fail("write " + path.string());
            return;
        }
        out << doc.dump(2) << '\n';
        if (!out) {
            Fail("flush " + path.string());
            return;
        }
    }
    Pass("wrote " + path.string());

    std::ifstream in(path);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ExpectTrue("bench JSON kind recomp_benchmark",
               json.find("\"kind\": \"recomp_benchmark\"") != std::string::npos);
    ExpectTrue("bench JSON schema_version 1",
               json.find("\"schema_version\": 1") != std::string::npos);
    ExpectTrue("bench JSON hybrid_aot mode", json.find("\"hybrid_aot\"") != std::string::npos);
    ExpectTrue("bench JSON jit mode", json.find("\"jit\"") != std::string::npos);
    ExpectTrue("bench JSON not_single_shift",
               json.find("\"not_single_shift\": true") != std::string::npos);
    ExpectTrue("bench JSON host_mem_callbacks",
               json.find("\"host_mem_callbacks\"") != std::string::npos);
    std::cout << "recomp_benchmark.json path: " << path << "\n";
    std::cout << "=== recomp_benchmark.json ===\n" << json << std::endl;
    ScenarioPass("JIT vs hybrid AOT benchmark JSON from identical guest fixture", before);
}

// Keep a small suite of workloads alongside the anti-folding ALU/memory loop.
// These are deliberately self-contained guest blocks already used by the
// integration scenarios: a TLS/SVC transition and a minimal compute/SVC
// workload exercise different transition and state-marshalling paths while
// remaining deterministic on every host.
ModeResult RunRepresentativeWorkload(StackFixture& f, const char* id, const char* description,
                                     u64 pc, u64 expected_x0, u32 expected_svc, int iters,
                                     bool tls, bool force_miss) {
    ModeResult r;
    r.id = id;
    r.backend = force_miss ? "jit" : "hybrid_aot";
    r.description = description;
    r.expected_x0 = expected_x0;
    r.expected_svc = expected_svc;
    g_force_miss_pc = force_miss ? pc : 0;
    const auto before = Core::GetRecompExecutionMetrics();
    r.mem_before = ReadMem();
    std::vector<u64> times;
    times.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        auto& ctx = f.thread->GetContext();
        ctx = {};
        ctx.pc = pc;
        if (tls) {
            ctx.r[4] = g_entry + kOffCrossPage;
        }
        f.system.Kernel().PhysicalCore(0).LoadContext(f.thread);
        if (tls) {
            // LoadContext publishes the thread's TPIDRRO. Set the test value
            // afterwards so the generated MRS observes it, exactly as the
            // production context-switch contract requires.
            f.arm->SetTpidrroEl0(0xC0FFEE);
        }
        const auto frame_t0 = std::chrono::steady_clock::now();
        f.system.GetPerfStats().BeginSystemFrame();
        const auto slice_t0 = std::chrono::steady_clock::now();
        const auto hr = f.arm->RunThread(f.thread);
        const u64 ns = NsSince(slice_t0);
        f.system.GetPerfStats().EndSystemFrame();
        times.push_back(ns);
        ++r.frame_events;
        r.frame_event_time_ns += NsSince(frame_t0);
        r.halt_svc = r.halt_svc || True(hr & Core::HaltReason::SupervisorCall);
        r.svc = f.arm->GetSvcNumber();
        Kernel::Svc::ThreadContext out{};
        f.arm->GetContext(out);
        r.x0 = out.r[0];
        if (tls) {
            if (out.r[1] != expected_x0 || out.r[3] != 0xC0FFEE ||
                f.system.ApplicationMemory().Read64(g_entry + kOffCrossPage) != 0xABCD) {
                Fail(std::string(id) + " TLS/cross-page effects mismatch");
            }
        }
        if (i == 0) {
            r.mem_after_first = ReadMem();
        }
        if (!True(hr & Core::HaltReason::SupervisorCall) || r.svc != expected_svc ||
            r.x0 != expected_x0) {
            Fail(std::string(id) + " result mismatch");
        }
    }
    g_force_miss_pc = 0;
    r.perf_stats_frametime_seconds =
        f.system.GetPerfStats().GetAndResetStats(f.system.CoreTiming().GetGlobalTimeUs()).frametime;
    r.mem_after = ReadMem();
    r.slices = SummarizeSlices(std::move(times));
    r.startup_ns = r.slices.first_ns;
    r.compile_ns = r.slices.first_ns > r.slices.median_ns
                       ? r.slices.first_ns - r.slices.median_ns
                       : 0;
    r.exec = MetricsDelta(Core::GetRecompExecutionMetrics(), before);
    return r;
}

void AddRepresentativeWorkloads(nlohmann::json& doc, StackFixture& f, int iters) {
    nlohmann::json suite = nlohmann::json::object();
    for (const auto& spec : std::array{
             std::tuple{"tls_svc", "TLS register and cross-page store followed by SVC", kOffTlsSvc,
                        u64{0x1234}, u32{42}, true},
             std::tuple{"plain_svc", "minimal MOVZ plus SVC transition", kOffPlain, u64{7},
                        u32{1}, false},
         }) {
        const auto [id, description, off, expected_x0, expected_svc, tls] = spec;
        // AOT first, then force the identical PC through Dynarmic. Both modes
        // therefore report real RunThread timings for the same guest bytes.
        auto aot = RunRepresentativeWorkload(f, id, description, g_entry + off, expected_x0,
                                             expected_svc, iters, tls, false);
        const std::string jit_id = std::string(id) + "_jit";
        auto jit = RunRepresentativeWorkload(f, jit_id.c_str(), description, g_entry + off,
                                             expected_x0, expected_svc, iters, tls, true);
        ExpectTrue(std::string(id) + " AOT blocks", aot.exec.aot_block_executions >=
                                                     static_cast<u64>(iters));
        ExpectEq(std::string(id) + " AOT no JIT", aot.exec.dynarmic_run_slices, 0);
        ExpectTrue(std::string(id) + " JIT slices", jit.exec.dynarmic_run_slices >=
                                                     static_cast<u64>(iters));
        ExpectEq(std::string(id) + " JIT no AOT", jit.exec.aot_block_executions, 0);
        suite[id] = {{"description", description},
                     {"iters", iters},
                     {"aot", ModeToJson(aot)},
                     {"jit", ModeToJson(jit)}};
    }
    doc["representative_workloads"] = std::move(suite);
}

void ScenarioBenchmark(StackFixture& f, const fs::path& json_path) {
    const int before = g_fails;
    // Fresh application process: AllowsAot, lookup still set, Dynarmic not yet
    // constructed. Hybrid AOT runs first so JIT compile is isolated to the jit mode.
    if (!f.BootstrapSecondProcess()) {
        return;
    }
    auto* recomp = AsRecomp(f.arm);
    ExpectTrue("bench ArmRecomp", recomp != nullptr);
    ExpectTrue("bench AllowsAot", recomp && recomp->AllowsAot());
    ExpectTrue("bench lookup registered",
               Lookup(g_entry + kOffBench) == g_block_bench && g_block_bench != nullptr);
    ExpectEq("bench guest RX movz", f.system.ApplicationMemory().Read32(g_entry + kOffBench),
             kMovzX0_0);
    ExpectEq("bench guest RX str", f.system.ApplicationMemory().Read32(g_entry + kOffBench + 4),
             kStrX0X3);
    ExpectEq("bench guest RX ldr", f.system.ApplicationMemory().Read32(g_entry + kOffBench + 8),
             kLdrX0X3);
    ExpectEq("bench guest RX add", f.system.ApplicationMemory().Read32(g_entry + kOffBench + 12),
             kAddX0X0X1);
    ExpectEq("bench guest RX svc",
             f.system.ApplicationMemory().Read32(g_entry + kOffBench + 4ull +
                                                 12ull * static_cast<u64>(kBenchAdds)),
             kSvc3);
    ExpectTrue("AOT .so not_single_shift (objdump pin)", g_aot_compile.not_single_shift);

    const int iters = BenchIters();
    std::cout << "bench iters=" << iters << " adds=" << kBenchAdds << " pc=" << std::hex
              << (g_entry + kOffBench) << std::dec << "\n";

    const ModeResult aot = RunIdenticalWorkload(f, true, iters);
    ExpectTrue("hybrid AOT stayed on AOT", aot.exec.aot_block_executions >= static_cast<u64>(iters));
    ExpectTrue("hybrid AOT time_ns > 0", aot.exec.aot_time_ns > 0);
    ExpectEq("hybrid AOT no Dynarmic fallback", aot.exec.dynarmic_run_slices, 0);
    ExpectEq("hybrid AOT no lookup miss", aot.exec.fallback_lookup_miss, 0);
    ExpectTrue("hybrid AOT slice times real", aot.slices.median_ns > 0 && aot.slices.first_ns > 0);
    ExpectTrue("AOT so_bytes > 0", g_aot_compile.so_bytes > 0);
    ExpectTrue("AOT compile_ns > 0", aot.compile_ns > 0);
    ExpectEq("AOT host_mem store callbacks", aot.hm_store_calls,
             static_cast<u64>(kBenchAdds) * static_cast<u64>(iters));
    ExpectEq("AOT host_mem load callbacks", aot.hm_load_calls,
             static_cast<u64>(kBenchAdds) * static_cast<u64>(iters));

    const ModeResult jit = RunIdenticalWorkload(f, false, iters);
    ExpectTrue("JIT Dynarmic slices", jit.exec.dynarmic_run_slices >= static_cast<u64>(iters));
    ExpectTrue("JIT Dynarmic time_ns > 0", jit.exec.dynarmic_time_ns > 0);
    ExpectTrue("JIT lookup miss recorded", jit.exec.fallback_lookup_miss > 0);
    ExpectEq("JIT did not execute AOT blocks", jit.exec.aot_block_executions, 0);
    ExpectTrue("JIT slice times real", jit.slices.median_ns > 0 && jit.slices.first_ns > 0);
    ExpectEq("JIT host_mem store callbacks (AOT helpers unused)", jit.hm_store_calls, 0);
    ExpectEq("JIT host_mem load callbacks (AOT helpers unused)", jit.hm_load_calls, 0);
    ExpectEq("both modes x0", aot.x0, jit.x0);
    ExpectEq("both modes svc", static_cast<u64>(aot.svc), static_cast<u64>(jit.svc));

    ExportBenchmarkJson(json_path, aot, jit, iters, f);
    ScenarioPass("identical JIT vs hybrid AOT workload (slice/startup/compile/memory/size)",
                 before);
}

void PrintGaps() {
    std::cout
        << "GAPS (honest / out of scope):\n"
        << "  - Multi-core KScheduler fiber world / CpuManager guest loop (the fixture uses a\n"
           "    registered core and real PhysicalCore::RunThread/Svc::Call dispatch)\n"
        << "  - Real NSO/NRO homebrew load (this uses a self-contained synthetic CodeSet)\n"
        << "  - gdbstub StepThread against a live title\n"
        << "  - GPU frame times (this harness has no renderer; slice = RunThread until SVC)\n"
        << "  - Isolated JIT code-cache byte size (Dynarmic does not expose used bytes; RSS delta)\n"
        << "  - AOT .so size includes non-bench integration blocks\n"
        << "  - Bench slice is ADD+STR+LDR (anti-fold), not a pure ALU stream\n"
        << "  - #5 compatibility, #6 opts, #7 release-gate\n"
        << "Pinned here: SetRecompLookup ArmRecomp, AllowsAot after Invalidate,\n"
        << "  Translate AOT != guest RX twin, Lookup consulted, LoadContext TLS,\n"
        << "  registered-PC force-miss, ClearInstructionCache, restart, StepThread,\n"
        << "  leftover pending_svc cleared on StepThread, live AOT/Dynarmic timers +\n"
        << "  fallback reasons + icache JSON export (path-safe tmp+rename),\n"
        << "  identical-PC JIT vs hybrid AOT race (recomp_benchmark.json),\n"
        << "  JudgeAotBenchDump FAILs discriminating PLT dump (counts lock PLT vs loop);\n"
        << "  live gcc -O3 PASS; named store/load PLT not required; host_mem callbacks "
           "512 x iters.\n"
        << "  #4 Translate AOT vs Dynarmic instruction correctness (edge + random inputs).\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "recomp_stack_harness: SetRecompLookup ArmRecomp + Translate AOT + Dynarmic\n";
    std::cout << "  (+ identical-PC JIT vs hybrid AOT benchmark + insn correctness)\n";

    ScenarioFoldPinSelfCheck();

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

    ScenarioAotLiveProof(*fix); // before Clear: proves Lookup + AOT != Dynarmic twin
    ScenarioSvcTlsCrossPage(*fix);
    ScenarioLeftoverSvcStep(*fix);
    // ScenarioLeftoverSvcStep consumes the pending SVC produced above. Keep
    // any scenario that may leave a pending SVC after that consumer.
    ScenarioPhysicalCoreDispatch(*fix);
    ScenarioLoadContextTls(*fix);
    // Force-miss + unhandled need AllowsAot (registered Translate blocks).
    ScenarioForceMissRegistered(*fix);
    ScenarioUnhandledFallback(*fix);
    ScenarioInsnCorrectness(*fix);
    // ClearInstructionCache permanently refuses AOT; run after the above.
    ScenarioInvalidation(*fix);
    ScenarioRestart(*fix);
    ScenarioStepMiss(*fix);

    const fs::path json_path = [](const fs::path& work) {
        if (const char* env = std::getenv("SUYU_RECOMP_EXECUTION_JSON"); env && env[0] != '\0') {
            return fs::path(env);
        }
        return work / "recomp_execution.json";
    }(root);
    ExportExecutionJson(json_path);

    const fs::path bench_path = [](const fs::path& work) {
        if (const char* env = std::getenv("SUYU_RECOMP_BENCHMARK_JSON"); env && env[0] != '\0') {
            return fs::path(env);
        }
        return work / "recomp_benchmark.json";
    }(root);
    ScenarioBenchmark(*fix, bench_path);
    PrintGaps();

    if (g_fails == 0) {
        std::cout << "ALL PASSED\n";
        std::quick_exit(0);
    }
    std::cerr << g_fails << " FAILURE(S)\n";
    std::quick_exit(1);
}
