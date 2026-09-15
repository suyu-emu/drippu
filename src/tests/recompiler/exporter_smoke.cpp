// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Portable acceptance check for the active AArch64 exporter (arm64_to_c.h):
//   1. EmitProject on a tiny supported sequence, then CMake-compile the project
//   2. RET Rn / BLR X30 probes compiled and executed as permanent regressions
//
// Deliberately does not invoke tools/static_recompiler.

#include "core/recompiler/arm64_to_c.h"
#include "core/arm/recomp/recomp_aot_cache.h"
#include "core/arm/recomp/recomp_icache.h"
#include "core/arm/recomp/recomp_image_abi.h"
#include "core/arm/recomp/recomp_session.h"
#include "core/arm/recomp/unresolved_import.h"
#include "smoke_config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;
using suyu::recomp::u32;
using suyu::recomp::u64;

namespace {

int g_fails = 0;

void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << std::endl;
    ++g_fails;
}

void pass(const std::string& msg) {
    std::cout << "PASS: " << msg << std::endl;
}

#ifndef _WIN32
std::string Quote(const std::string& s) {
    return "'" + s + "'";
}
#endif

std::string QuoteWinArg(std::string_view arg) {
    // CommandLineToArgvW rules: quote if empty or if space/tab/quote present;
    // double backslashes that precede a quote; double trailing backslashes
    // before the closing quote.
    const bool need_quote =
        arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string_view::npos;
    if (!need_quote) {
        return std::string(arg);
    }
    std::string out;
    out.push_back('"');
    size_t slashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
            out.push_back('"');
            slashes = 0;
            continue;
        }
        out.append(slashes, '\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string JoinWindowsCommandLine(const std::vector<std::string>& args) {
    std::string line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) {
            line.push_back(' ');
        }
        line += QuoteWinArg(args[i]);
    }
    return line;
}

int RunArgs(const std::vector<std::string>& args) {
    if (args.empty()) {
        fail("empty command");
        return 1;
    }
#ifdef _WIN32
    // _spawnv concatenates argv with spaces and does not quote, so
    // "C:/Program Files/CMake/..." and "Visual Studio 18 2026" split. Build a
    // CommandLineToArgvW-compatible line and CreateProcess it.
    const std::string cmdline = JoinWindowsCommandLine(args);
    std::cout << "+ " << cmdline << std::endl;
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &si, &pi)) {
        fail("CreateProcess " + args[0] + ": error " + std::to_string(GetLastError()));
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::cout << '+';
    for (const auto& a : args) {
        std::cout << ' ' << Quote(a);
    }
    std::cout << std::endl;
    std::ostringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) {
            cmd << ' ';
        }
        cmd << Quote(args[i]);
    }
    return std::system(cmd.str().c_str());
#endif
}

bool WriteFile(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fail("write " + path.string());
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// AArch64 encodings used by the review's reproductions.
constexpr u32 kMovzX0_5 = 0xD28000A0u;
constexpr u32 kMovzX1_7 = 0xD28000E1u;
constexpr u32 kMovX0Zero = 0xD2800000u;
constexpr u32 kAddX2X0X1 = 0x8B010002u;
constexpr u32 kSvc0 = 0xD4000001u;
constexpr u32 kRetX5 = 0xD65F00A0u;
constexpr u32 kRetX30 = 0xD65F03C0u;
constexpr u32 kBlrX30 = 0xD63F03C0u;
constexpr u32 kBPlus8 = 0x14000002u;
constexpr u32 kMsrFpcrX0 = 0xD51B4400u;
constexpr u32 kMrsX0Fpcr = 0xD53B4400u;
constexpr u32 kMrsX1Fpsr = 0xD53B4421u;
constexpr u32 kFaddD2D0D1 = 0x1E612802u;
constexpr u32 kFmulD2D0D1 = 0x1E610802u;
constexpr u32 kFdivD2D0D1 = 0x1E611802u;
constexpr u32 kFsqrtD0D1 = 0x1E61C020u;
constexpr u32 kFcvtS0D1 = 0x1E624020u;
constexpr u32 kScvtfD0X1 = 0x9E620020u;
constexpr u32 kFaddpD0V1 = 0x7E70D820u;
constexpr u32 kFaddV0V1V2_2d = 0x4E62D420u;
constexpr u32 kFmlaV0V1V2_2d = 0x4E62CC20u;
constexpr u32 kFabsD0D1 = 0x1E60C020u;

bool AesHelpersAtFileScope(const std::string& runtime_c) {
    const auto save = runtime_c.find("int recomp_save_write(");
    const auto sbox = runtime_c.find("recomp_aes_sbox");
    if (save == std::string::npos) {
        fail("generated runtime missing recomp_save_write");
        return false;
    }
    if (sbox == std::string::npos) {
        fail("generated runtime missing recomp_aes_sbox");
        return false;
    }
    const auto brace = runtime_c.find('{', save);
    if (brace == std::string::npos) {
        fail("recomp_save_write has no body");
        return false;
    }
    int depth = 0;
    size_t end = std::string::npos;
    for (size_t i = brace; i < runtime_c.size(); ++i) {
        if (runtime_c[i] == '{') {
            ++depth;
        } else if (runtime_c[i] == '}') {
            --depth;
            if (depth == 0) {
                end = i;
                break;
            }
        }
    }
    if (end == std::string::npos) {
        fail("unterminated recomp_save_write");
        return false;
    }
    if (sbox > brace && sbox < end) {
        fail("recomp_aes_sbox is nested inside recomp_save_write");
        return false;
    }
    const auto gmul = runtime_c.find("recomp_gmul");
    if (gmul != std::string::npos && gmul > brace && gmul < end) {
        fail("recomp_gmul is nested inside recomp_save_write");
        return false;
    }
    pass("AES helpers are at file scope");
    return true;
}

int CmakeBuild(const fs::path& src, const fs::path& build, const char* target,
               bool iso_c11) {
    std::vector<std::string> cfg{SUYU_SMOKE_CMAKE, "-S", src.string(), "-B",
                                 build.string(), "-DCMAKE_BUILD_TYPE=Release"};
    const std::string gen = SUYU_SMOKE_GENERATOR;
    if (!gen.empty()) {
        cfg.push_back("-G");
        cfg.push_back(gen);
    }
    const std::string plat = SUYU_SMOKE_GENERATOR_PLATFORM;
    if (!plat.empty()) {
        cfg.push_back("-A");
        cfg.push_back(plat);
    }
    if (iso_c11) {
        // Probe sources are ISO C11. The generated runtime uses POSIX
        // nanosleep, so it is compiled with the host compiler defaults.
        cfg.emplace_back("-DCMAKE_C_STANDARD=11");
        cfg.emplace_back("-DCMAKE_C_EXTENSIONS=OFF");
    }
    const std::string cc = SUYU_SMOKE_C_COMPILER;
    // The Visual Studio generator selects cl.exe itself; passing a
    // CMAKE_C_COMPILER path is unnecessary and can confuse the cache.
    if (!cc.empty() && gen.rfind("Visual Studio", 0) != 0) {
        cfg.push_back(std::string("-DCMAKE_C_COMPILER=") + cc);
    }
    if (RunArgs(cfg) != 0) {
        fail("cmake configure " + src.string());
        return 1;
    }
    const std::vector<std::string> bld{SUYU_SMOKE_CMAKE, "--build", build.string(),
                                       "--config", "Release", "--target", target};
    if (RunArgs(bld) != 0) {
        fail("cmake build " + std::string(target) + " in " + build.string());
        return 1;
    }
    return 0;
}

void TestEmitProjectCompile(const fs::path& root) {
    const fs::path out = root / "emit_project";
    fs::create_directories(out);

    u32 text[4] = {kMovzX0_5, kMovzX1_7, kAddX2X0X1, kSvc0};
    suyu::recomp::EmitProject("smoke", reinterpret_cast<const suyu::recomp::u8*>(text),
                              sizeof(text), 0x1000, out.string(), true);

    const fs::path runtime = out / "recomp_runtime.c";
    if (!fs::exists(runtime)) {
        fail("EmitProject did not write recomp_runtime.c");
        return;
    }
    if (!AesHelpersAtFileScope(ReadFile(runtime))) {
        return;
    }

    if (CmakeBuild(out, out / "build", "recompiled", false) == 0) {
        pass("EmitProject generated project compiled");
    }
}

std::string TranslateInsn(u32 insn, u64 pc) {
    std::string body;
    suyu::recomp::Translate(insn, pc, body);
    return body;
}

bool BodyUnhandled(const std::string& body) {
    return body.find("recomp_unhandled") != std::string::npos;
}

bool BodyReadsFpcr(const std::string& body) {
    return body.find("c->fpcr") != std::string::npos;
}

void ExpectFpControlledOrUnhandled(const char* name, u32 insn) {
    const std::string body = TranslateInsn(insn, 0x1000);
    if (BodyUnhandled(body)) {
        pass(std::string(name) + " routed to accurate backend");
        return;
    }
    if (BodyReadsFpcr(body)) {
        pass(std::string(name) + " translated C reads c->fpcr");
        return;
    }
    fail(std::string(name) + " uses host FP without guest FPCR: " + body);
}

void TestTranslatedShape() {
    const std::string ret5 = TranslateInsn(kRetX5, 0x1000);
    if (ret5.find("c->x[5]") == std::string::npos) {
        fail("RET X5 does not read X5: " + ret5);
    } else if (ret5.find("c->x[30]") != std::string::npos) {
        fail("RET X5 still mentions X30: " + ret5);
    } else {
        pass("RET X5 translated C reads X5");
    }

    const std::string blr = TranslateInsn(kBlrX30, 0x1000);
    const auto read = blr.find("c->x[30]");
    const auto write = blr.find("c->x[30]=");
    if (read == std::string::npos || write == std::string::npos) {
        fail("BLR X30 missing X30 read or link write: " + blr);
    } else if (read >= write) {
        fail("BLR X30 writes X30 before reading the target: " + blr);
    } else {
        pass("BLR X30 translated C reads the target before writing LR");
    }
}

void TestBranchProbes(const fs::path& root) {
    const std::string ret5 = TranslateInsn(kRetX5, 0x1000);
    const std::string ret30 = TranslateInsn(kRetX30, 0x1000);
    const std::string blr = TranslateInsn(kBlrX30, 0x1000);

    std::ostringstream src;
    src << "#include <stdint.h>\n#include <stdio.h>\n"
           "typedef struct { uint64_t x[32]; uint64_t pc; } GuestContext;\n"
           "uint64_t g_module_base = 0;\n"
           "static void ret_x5(GuestContext* c) {\n"
        << ret5
        << "}\nstatic void ret_x30(GuestContext* c) {\n"
        << ret30
        << "}\nstatic void blr_x30(GuestContext* c) {\n"
        << blr
        << "}\nint main(void) {\n"
           "  int fail = 0;\n"
           "  GuestContext c;\n"
           "  int i;\n"
           "  for (i = 0; i < 32; i++) c.x[i] = 0;\n"
           "  c.x[5] = 0x9000; c.x[30] = 0x8000; c.pc = 0x1000;\n"
           "  ret_x5(&c);\n"
           "  printf(\"RET X5: pc=%llx expected=9000\\n\", (unsigned long long)c.pc);\n"
           "  if (c.pc != 0x9000) fail = 1;\n"
           "  for (i = 0; i < 32; i++) c.x[i] = 0;\n"
           "  c.x[30] = 0x8000; c.pc = 0x1000;\n"
           "  ret_x30(&c);\n"
           "  printf(\"RET X30: pc=%llx expected=8000\\n\", (unsigned long long)c.pc);\n"
           "  if (c.pc != 0x8000) fail = 1;\n"
           "  for (i = 0; i < 32; i++) c.x[i] = 0;\n"
           "  c.x[30] = 0x8000; c.pc = 0x1000;\n"
           "  blr_x30(&c);\n"
           "  printf(\"BLR X30: pc=%llx expected=8000 lr=%llx expected=1004\\n\",\n"
           "         (unsigned long long)c.pc, (unsigned long long)c.x[30]);\n"
           "  if (c.pc != 0x8000 || c.x[30] != 0x1004) fail = 1;\n"
           "  return fail;\n"
           "}\n";

    const fs::path probe_src = root / "branch_probe";
    fs::create_directories(probe_src);
    if (!WriteFile(probe_src / "probe.c", src.str())) {
        return;
    }
    if (!WriteFile(probe_src / "CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.13)\n"
                   "project(suyu_branch_probe C)\n"
                   "set(CMAKE_C_STANDARD 11)\n"
                   "set(CMAKE_C_EXTENSIONS OFF)\n"
                   "add_executable(branch_probe probe.c)\n")) {
        return;
    }

    const fs::path probe_build = probe_src / "build";
    if (CmakeBuild(probe_src, probe_build, "branch_probe", true) != 0) {
        return;
    }

    fs::path exe = probe_build / "branch_probe";
#ifdef _WIN32
    if (!fs::exists(exe)) {
        exe = probe_build / "Release" / "branch_probe.exe";
    }
    if (!fs::exists(exe)) {
        exe = probe_build / "Debug" / "branch_probe.exe";
    }
#else
    if (!fs::exists(exe)) {
        exe = probe_build / "Release" / "branch_probe";
    }
#endif
    if (!fs::exists(exe)) {
        fail("branch_probe executable not found under " + probe_build.string());
        return;
    }
    if (RunArgs({exe.string()}) != 0) {
        fail("branch_probe execution");
        return;
    }
    pass("RET X5 / RET X30 / BLR X30 executed");
}

void TestFpControl(const fs::path& root) {
    const std::string msr = TranslateInsn(kMsrFpcrX0, 0x1000);
    const std::string mrs_fpcr = TranslateInsn(kMrsX0Fpcr, 0x1004);
    const std::string mrs_fpsr = TranslateInsn(kMrsX1Fpsr, 0x1008);
    const std::string fadd = TranslateInsn(kFaddD2D0D1, 0x100C);

    if (msr.find("c->fpcr") == std::string::npos) {
        fail("MSR FPCR does not store c->fpcr: " + msr);
        return;
    }
    if (mrs_fpcr.find("c->fpcr") == std::string::npos) {
        fail("MRS FPCR does not read c->fpcr: " + mrs_fpcr);
        return;
    }
    pass("MSR/MRS FPCR translated C stores and loads the field");

    const std::string fabsd = TranslateInsn(kFabsD0D1, 0x1000);
    if (BodyUnhandled(fabsd) || fabsd.find("fabs") == std::string::npos) {
        fail("FABS Dd should stay translated: " + fabsd);
    } else {
        pass("FABS Dd stays bitwise translated");
    }

    ExpectFpControlledOrUnhandled("FADD Dd", kFaddD2D0D1);
    ExpectFpControlledOrUnhandled("FMUL Dd", kFmulD2D0D1);
    ExpectFpControlledOrUnhandled("FDIV Dd", kFdivD2D0D1);
    ExpectFpControlledOrUnhandled("FSQRT Dd", kFsqrtD0D1);
    ExpectFpControlledOrUnhandled("FCVT Sd,Dd", kFcvtS0D1);
    ExpectFpControlledOrUnhandled("SCVTF Dd,Xn", kScvtfD0X1);
    ExpectFpControlledOrUnhandled("FADDP Dd", kFaddpD0V1);
    ExpectFpControlledOrUnhandled("FADD Vd.2D", kFaddV0V1V2_2d);
    ExpectFpControlledOrUnhandled("FMLA Vd.2D", kFmlaV0V1V2_2d);

    if (BodyUnhandled(fadd) || BodyReadsFpcr(fadd)) {
        return;
    }

    std::ostringstream src;
    src << "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
           "typedef struct {\n"
           "  uint64_t x[32];\n"
           "  uint64_t pc;\n"
           "  uint64_t vreg[32][2];\n"
           "  uint64_t fpcr;\n"
           "  uint64_t fpsr;\n"
           "} GuestContext;\n"
           "uint64_t g_module_base = 0;\n"
           "static void msr_fpcr(GuestContext* c) {\n"
        << msr
        << "}\nstatic void mrs_fpcr(GuestContext* c) {\n"
        << mrs_fpcr
        << "}\nstatic void mrs_fpsr(GuestContext* c) {\n"
        << mrs_fpsr
        << "}\nstatic void fadd_d2(GuestContext* c) {\n"
        << fadd
        << "}\nstatic uint64_t run_fadd(uint64_t fpcr) {\n"
           "  GuestContext c;\n"
           "  memset(&c, 0, sizeof c);\n"
           "  c.x[0] = fpcr;\n"
           "  msr_fpcr(&c);\n"
           "  mrs_fpcr(&c);\n"
           "  c.vreg[0][0] = 0x3FF0000000000000ULL;\n"
           "  c.vreg[1][0] = 0x3CA0000000000000ULL;\n"
           "  fadd_d2(&c);\n"
           "  mrs_fpsr(&c);\n"
           "  printf(\"FPCR wrote=%llx read=%llx sum=%llx fpsr=%llx\\n\",\n"
           "         (unsigned long long)fpcr,\n"
           "         (unsigned long long)c.x[0],\n"
           "         (unsigned long long)c.vreg[2][0],\n"
           "         (unsigned long long)c.x[1]);\n"
           "  if (c.x[0] != fpcr) return 0;\n"
           "  return c.vreg[2][0];\n"
           "}\nint main(void) {\n"
           "  const uint64_t rp = run_fadd(0x400000ULL);\n"
           "  const uint64_t rm = run_fadd(0x800000ULL);\n"
           "  printf(\"FADD 1+2^-53 RP=%llx RM=%llx\\n\",\n"
           "         (unsigned long long)rp, (unsigned long long)rm);\n"
           "  if (rp == 0 || rm == 0) return 1;\n"
           "  if (rp == rm) {\n"
           "    printf(\"FPCR rounding not applied\\n\");\n"
           "    return 1;\n"
           "  }\n"
           "  if (rp != 0x3FF0000000000001ULL) {\n"
           "    printf(\"RP sum is not 1.0+ulp\\n\");\n"
           "    return 1;\n"
           "  }\n"
           "  if (rm != 0x3FF0000000000000ULL) {\n"
           "    printf(\"RM sum is not 1.0\\n\");\n"
           "    return 1;\n"
           "  }\n"
           "  return 0;\n"
           "}\n";

    const fs::path probe_src = root / "fp_probe";
    fs::create_directories(probe_src);
    if (!WriteFile(probe_src / "probe.c", src.str())) {
        return;
    }
    if (!WriteFile(probe_src / "CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.13)\n"
                   "project(suyu_fp_probe C)\n"
                   "set(CMAKE_C_STANDARD 11)\n"
                   "set(CMAKE_C_EXTENSIONS OFF)\n"
                   "add_executable(fp_probe probe.c)\n")) {
        return;
    }

    const fs::path probe_build = probe_src / "build";
    if (CmakeBuild(probe_src, probe_build, "fp_probe", true) != 0) {
        return;
    }

    fs::path exe = probe_build / "fp_probe";
#ifdef _WIN32
    if (!fs::exists(exe)) {
        exe = probe_build / "Release" / "fp_probe.exe";
    }
    if (!fs::exists(exe)) {
        exe = probe_build / "Debug" / "fp_probe.exe";
    }
#else
    if (!fs::exists(exe)) {
        exe = probe_build / "Release" / "fp_probe";
    }
#endif
    if (!fs::exists(exe)) {
        fail("fp_probe executable not found under " + probe_build.string());
        return;
    }
    if (RunArgs({exe.string()}) != 0) {
        fail("FADD under guest FPCR RP vs RM");
        return;
    }
    pass("FADD honors guest FPCR rounding");
}

template <typename Read32>
u64 FindGuestReturnStub(u64 mod_base, Read32&& read32, u64 scan_limit = 0x100000) {
    u64 bare_ret = 0;
    for (u64 off = 0; off < scan_limit; off += 4) {
        const u32 insn = read32(mod_base + off);
        if (insn == kRetX30) {
            if (!bare_ret) {
                bare_ret = mod_base + off;
            }
        } else if (insn == kMovX0Zero && read32(mod_base + off + 4) == kRetX30) {
            return mod_base + off;
        }
    }
    return bare_ret;
}

void TestUnresolvedImportPolicy() {
    using suyu::recomp::FormatUnresolvedImportDiagnostic;
    using suyu::recomp::IsUnresolvedImportTrap;
    using suyu::recomp::kUnresolvedImportTrap;
    using suyu::recomp::TakeUnresolvedImportTrap;
    using suyu::recomp::UnresolvedImport;
    using suyu::recomp::UnresolvedReloc;
    using suyu::recomp::UnresolvedSlotTarget;
    using suyu::recomp::UnresolvedTrapAction;

    const u64 base = 0x7100000000ULL;
    std::vector<u32> text(16, 0xD503201Fu);
    text[4] = kRetX30;
    auto read32 = [&](u64 va) -> u32 {
        const u64 i = (va - base) / 4;
        return i < text.size() ? text[i] : 0;
    };
    const u64 bare = FindGuestReturnStub(base, read32, 64);
    if (bare != base + 16) {
        fail("FindGuestReturnStub missed the bare RET");
        return;
    }
    if (UnresolvedSlotTarget() == bare) {
        fail("unresolved JUMP_SLOT still targets a guest RET stub");
    } else {
        pass("unresolved JUMP_SLOT uses the halt sentinel");
    }

    text[8] = kMovX0Zero;
    text[9] = kRetX30;
    const u64 zero_ret = FindGuestReturnStub(base, read32, 64);
    if (zero_ret != base + 32) {
        fail("FindGuestReturnStub missed mov x0,#0; ret");
        return;
    }
    if (UnresolvedSlotTarget() == zero_ret) {
        fail("unsupported IRELATIVE still targets mov x0,#0; ret");
    } else {
        pass("unsupported IRELATIVE uses the halt sentinel");
    }
    if (UnresolvedSlotTarget() != kUnresolvedImportTrap) {
        fail("UnresolvedSlotTarget is not the halt sentinel");
    } else {
        pass("UnresolvedSlotTarget is the halt sentinel");
    }

    const std::vector<UnresolvedImport> recorded{
        {"nn::fs::MountSdCard", base, 0x2000, UnresolvedReloc::JumpSlot},
        {"", base, 0x2010, UnresolvedReloc::Irelative},
    };
    const auto hit = TakeUnresolvedImportTrap(0xDEADBEEFCAFEBABEULL, 0x7100001000ULL, recorded);
    if (hit.action != UnresolvedTrapAction::Halt) {
        fail("unresolved import trap still fakes a function return");
    } else if (hit.x0 != 0xDEADBEEFCAFEBABEULL) {
        fail("unresolved import trap changed X0");
    } else if (hit.pc == 0x7100001000ULL) {
        fail("unresolved import trap returned to LR");
    } else {
        pass("unresolved import trap halts");
    }
    if (!IsUnresolvedImportTrap(kUnresolvedImportTrap)) {
        fail("IsUnresolvedImportTrap rejects the sentinel");
    }
    if (hit.diagnostic.find("nn::fs::MountSdCard") == std::string::npos) {
        fail("halt diagnostic missing symbol name: " + hit.diagnostic);
    } else {
        pass("halt diagnostic names the unresolved symbol");
    }
    if (hit.diagnostic.find("R_AARCH64_IRELATIVE") == std::string::npos) {
        fail("halt diagnostic missing IRELATIVE: " + hit.diagnostic);
    } else {
        pass("halt diagnostic names unsupported IRELATIVE");
    }

    const std::string irel =
        FormatUnresolvedImportDiagnostic("", base, 0x2010, UnresolvedReloc::Irelative);
    if (irel.find("R_AARCH64_IRELATIVE") == std::string::npos ||
        irel.find("<no name>") == std::string::npos) {
        fail("IRELATIVE diagnostic is imprecise: " + irel);
    } else {
        pass("IRELATIVE diagnostic names the reloc and missing resolver");
    }
}

struct SessionModule {
    const char* name;
    u64 base;
};

struct SessionDispatcher {
    SessionModule bases[8]{};
    size_t count = 0;
    void SetBase(size_t index, const char* name, u64 base) {
        if (index >= 8) {
            return;
        }
        if (index >= count) {
            count = index + 1;
        }
        bases[index] = SessionModule{name, base};
    }
};

void RegisterModules(SessionDispatcher& disp, const SessionModule* mods, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        disp.SetBase(i, mods[i].name, mods[i].base);
    }
}

void TestModuleRegistrationSession() {
    using suyu::recomp::RecompSession;

    RecompSession session;
    SessionDispatcher disp;
    int process_a = 1;
    int process_b = 2;

    const SessionModule title_a_boot1[] = {
        {"rtld", 0x7100000000ULL},
        {"main", 0x7100200000ULL},
        {"nnSdk", 0x7101000000ULL},
    };
    session.AttachProcess(&process_a);
    session.EnsureModuleBasesRegistered(
        [&] { RegisterModules(disp, title_a_boot1, 3); });
    if (disp.count != 3 || disp.bases[1].base != 0x7100200000ULL) {
        fail("first boot did not register main at 0x7100200000");
    } else {
        pass("first boot registers module bases");
    }

    bool reregistered = false;
    session.EnsureModuleBasesRegistered([&] {
        reregistered = true;
        RegisterModules(disp, title_a_boot1, 3);
    });
    if (reregistered) {
        fail("second core of the same boot re-registered bases");
    } else {
        pass("second core of the same boot does not re-register");
    }

    session.NoteStaticBlock();
    session.NoteStaticBlock();
    if (session.static_blocks() != 2) {
        fail("coverage did not count this process");
    }

    session.DetachProcess(&process_a);
    session.AttachProcess(&process_a);
    const SessionModule title_a_boot2[] = {
        {"rtld", 0x7200000000ULL},
        {"main", 0x7200200000ULL},
        {"nnSdk", 0x7201000000ULL},
    };
    session.EnsureModuleBasesRegistered(
        [&] { RegisterModules(disp, title_a_boot2, 3); });
    if (disp.bases[1].base != 0x7200200000ULL) {
        fail("stop/start ASLR still has main at 0x7100200000");
    } else {
        pass("stop/start same title with ASLR re-registers main");
    }
    if (session.static_blocks() != 0) {
        fail("coverage still holds the previous process");
    } else {
        pass("stop/start resets coverage");
    }

    session.DetachProcess(&process_a);
    session.AttachProcess(&process_b);
    const SessionModule title_b[] = {
        {"rtld", 0x7300000000ULL},
        {"cross2_Release.nss", 0x7300400000ULL},
        {"nnSdk", 0x7302000000ULL},
    };
    session.EnsureModuleBasesRegistered([&] { RegisterModules(disp, title_b, 3); });
    if (disp.bases[1].base != 0x7300400000ULL) {
        fail("title switch still has the previous main base");
    } else {
        pass("switching titles re-registers main");
    }

    session.DetachProcess(&process_b);
    RecompSession cores;
    int process_c = 3;
    cores.AttachProcess(&process_c);
    SessionDispatcher core_disp;
    std::atomic<int> registrations{0};
    const SessionModule aslr_cores[] = {
        {"rtld", 0x7400000000ULL},
        {"main", 0x7400200000ULL},
        {"nnSdk", 0x7401000000ULL},
    };
    auto register_cores = [&] {
        cores.EnsureModuleBasesRegistered([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            registrations.fetch_add(1, std::memory_order_relaxed);
            RegisterModules(core_disp, aslr_cores, 3);
        });
    };
    std::thread t0(register_cores);
    std::thread t1(register_cores);
    std::thread t2(register_cores);
    std::thread t3(register_cores);
    t0.join();
    t1.join();
    t2.join();
    t3.join();
    if (const int n = registrations.load(std::memory_order_relaxed); n != 1) {
        fail("four cores registered bases " + std::to_string(n) + " times");
    } else if (core_disp.bases[1].base != 0x7400200000ULL) {
        fail("four cores did not publish the ASLR main base");
    } else {
        pass("four cores register once and wait");
    }
}

void TestCacheInvalidation() {
    using suyu::recomp::RecompICache;

    std::unordered_set<u64> blocks{0x1000, 0x1008};
    suyu::recomp::g_chain_blocks = &blocks;
    suyu::recomp::g_chain_mod = "icache";
    const std::string chain = TranslateInsn(kBPlus8, 0x1000);
    suyu::recomp::g_chain_blocks = nullptr;
    suyu::recomp::g_chain_mod = nullptr;
    if (chain.find("--c->chain_budget <= 0") == std::string::npos) {
        fail("ChainTo no longer parks when the chain budget is spent: " + chain);
    } else {
        pass("ChainTo parks when chain_budget hits 0");
    }

    RecompICache cache;
    char aot_block = 0;
    auto select = [&](u64 pc) -> void* {
        if (!cache.AllowsAot()) {
            return nullptr;
        }
        return pc == 0x1008 ? &aot_block : nullptr;
    };

    if (select(0x1008) != &aot_block) {
        fail("AOT lookup missed 0x1008 before invalidate");
        return;
    }

    cache.Clear();
    if (select(0x1008) == &aot_block) {
        fail("InvalidateCacheRange left AOT block 0x1008 selected");
    } else {
        pass("InvalidateCacheRange stopped selecting AOT block 0x1008");
    }
    if (cache.AllowsAot()) {
        fail("direct block chain can still enter invalidated AOT");
    } else {
        pass("direct block chain cannot enter invalidated AOT");
    }

    RecompICache cleared;
    cleared.Clear();
    if (cleared.AllowsAot()) {
        fail("ClearInstructionCache left AOT selectable");
    } else {
        pass("ClearInstructionCache rejects AOT");
    }

    RecompICache from_nested_ic;
    from_nested_ic.Clear();
    if (from_nested_ic.AllowsAot()) {
        fail("nested JIT CacheInvalidation halt left AOT selectable");
    } else {
        pass("nested JIT CacheInvalidation halt rejects AOT");
    }
}

void DummyBlock(void* c) {
    (void)c;
}

suyu::recomp::RecompImageBlockFn DummyLookup(u64 pc) {
    (void)pc;
    return DummyBlock;
}

void TestSharedImageAbi(const fs::path& root) {
    using suyu::recomp::ApplyModuleBase;
    using suyu::recomp::ImageExpect;
    using suyu::recomp::ImageReject;
    using suyu::recomp::PlaceLoadedModule;
    using suyu::recomp::RecompImageAbi;
    using suyu::recomp::RecompImageAbiFn;
    using suyu::recomp::RecompImageExports;
    using suyu::recomp::RecompModuleMap;
    using suyu::recomp::SlotByName;
    using suyu::recomp::ValidateImageExports;
    using suyu::recomp::kRecompBuildIdSize;
    using suyu::recomp::kRecompImageAbiVersion;
    using suyu::recomp::kRecompMaxModules;
    using suyu::recomp::kRecompRegsPrefixSize;

    RecompImageExports lookup_only{};
    lookup_only.lookup = DummyLookup;
    if (ValidateImageExports(lookup_only) == ImageReject::Ok) {
        fail("lookup-only image accepted with no ABI, hash, or setter");
    } else {
        pass("lookup-only image rejected");
    }

    static RecompImageAbi main_abi{};
    main_abi.abi_version = kRecompImageAbiVersion;
    main_abi.abi_size = static_cast<uint32_t>(sizeof(RecompImageAbi));
    main_abi.context_size = 2048;
    main_abi.regs_prefix_size = kRecompRegsPrefixSize;
    main_abi.module_index = 1;
    std::memset(main_abi.build_id, 0x11, kRecompBuildIdSize);
    std::strncpy(main_abi.module_name, "main", sizeof(main_abi.module_name) - 1);
    auto main_abi_fn = []() -> const RecompImageAbi* { return &main_abi; };

    RecompImageExports no_setter{};
    no_setter.lookup = DummyLookup;
    no_setter.abi = main_abi_fn;
    if (ValidateImageExports(no_setter) == ImageReject::Ok) {
        fail("image without recomp_image_set_base was accepted");
    } else {
        pass("missing set_base rejected");
    }

    RecompImageExports no_abi{};
    no_abi.lookup = DummyLookup;
    no_abi.set_base = +[](u64) {};
    if (ValidateImageExports(no_abi) == ImageReject::Ok) {
        fail("image without recomp_image_abi was accepted");
    } else {
        pass("missing ABI export rejected");
    }

    static RecompImageAbi bad_ver = main_abi;
    bad_ver.abi_version = 0;
    RecompImageExports wrong_ver{};
    wrong_ver.lookup = DummyLookup;
    wrong_ver.set_base = +[](u64) {};
    wrong_ver.abi = []() -> const RecompImageAbi* { return &bad_ver; };
    if (ValidateImageExports(wrong_ver) == ImageReject::Ok) {
        fail("ABI version 0 was accepted");
    } else {
        pass("ABI version mismatch rejected");
    }

    static RecompImageAbi bad_prefix = main_abi;
    bad_prefix.regs_prefix_size = 256;
    RecompImageExports wrong_prefix{};
    wrong_prefix.lookup = DummyLookup;
    wrong_prefix.set_base = +[](u64) {};
    wrong_prefix.abi = []() -> const RecompImageAbi* { return &bad_prefix; };
    if (ValidateImageExports(wrong_prefix) == ImageReject::Ok) {
        fail("regs prefix size 256 was accepted");
    } else {
        pass("context prefix mismatch rejected");
    }

    uint8_t expected_hash[kRecompBuildIdSize];
    std::memset(expected_hash, 0x22, kRecompBuildIdSize);
    ImageExpect expect_hash{};
    expect_hash.build_id = expected_hash;
    RecompImageExports hashed{};
    hashed.lookup = DummyLookup;
    hashed.set_base = +[](u64) {};
    hashed.abi = main_abi_fn;
    if (ValidateImageExports(hashed, expect_hash) == ImageReject::Ok) {
        fail("image build_id 0x11 accepted for title hash 0x22");
    } else {
        pass("title/update content hash mismatch rejected");
    }

    ImageExpect require_missing{};
    require_missing.require_build_id = true;
    if (ValidateImageExports(hashed, require_missing) != ImageReject::MissingExpectBuildId) {
        fail("require_build_id accepted a null expect.build_id");
    } else {
        pass("missing required live build_id rejected");
    }

    static RecompImageAbi sdk_abi{};
    sdk_abi.abi_version = kRecompImageAbiVersion;
    sdk_abi.abi_size = static_cast<uint32_t>(sizeof(RecompImageAbi));
    sdk_abi.context_size = 2048;
    sdk_abi.regs_prefix_size = kRecompRegsPrefixSize;
    sdk_abi.module_index = 2;
    std::memset(sdk_abi.build_id, 0x33, kRecompBuildIdSize);
    std::strncpy(sdk_abi.module_name, "sdk", sizeof(sdk_abi.module_name) - 1);

    RecompModuleMap map{};
    RecompImageExports main_ex{};
    main_ex.lookup = DummyLookup;
    main_ex.set_base = +[](u64) {};
    main_ex.abi = main_abi_fn;
    RecompImageExports sdk_ex{};
    sdk_ex.lookup = DummyLookup;
    sdk_ex.set_base = +[](u64) {};
    sdk_ex.abi = []() -> const RecompImageAbi* { return &sdk_abi; };
    PlaceLoadedModule(map, main_ex);
    PlaceLoadedModule(map, sdk_ex);
    ApplyModuleBase(map, 0, "rtld", 0x7100000000ULL);
    ApplyModuleBase(map, 1, "main", 0x7100200000ULL);
    ApplyModuleBase(map, 2, "sdk", 0x7101000000ULL);
    const auto* main_slot = SlotByName(map, "main");
    const auto* sdk_slot = SlotByName(map, "sdk");
    if (!main_slot || main_slot->base == 0x7100000000ULL) {
        fail("omitting rtld assigned rtld's base 0x7100000000 to main");
    } else if (main_slot->base != 0x7100200000ULL) {
        fail("main base is not 0x7100200000");
    } else {
        pass("main kept its own base when rtld was omitted");
    }
    if (!sdk_slot || sdk_slot->base == 0x7100200000ULL) {
        fail("omitting rtld assigned main's base 0x7100200000 to sdk");
    } else if (sdk_slot->base != 0x7101000000ULL) {
        fail("sdk base is not 0x7101000000");
    } else {
        pass("sdk kept its own base when rtld was omitted");
    }

    // Sparse load order rtld, main, subsdk1, sdk — runtime indices 0..3 must not
    // be treated as ABI ordinals (subsdk1=3, sdk=12). Reproduce the review case.
    {
        using suyu::recomp::ModuleIndexForName;
        using suyu::recomp::SlotByIdentity;

        static RecompImageAbi sparse_rtld{};
        static RecompImageAbi sparse_main{};
        static RecompImageAbi sparse_subsdk1{};
        static RecompImageAbi sparse_sdk{};
        auto fill = [](RecompImageAbi& abi, const char* name, uint8_t fill_byte) {
            abi = {};
            abi.abi_version = kRecompImageAbiVersion;
            abi.abi_size = static_cast<uint32_t>(sizeof(RecompImageAbi));
            abi.context_size = 2048;
            abi.regs_prefix_size = kRecompRegsPrefixSize;
            abi.module_index = static_cast<uint32_t>(ModuleIndexForName(name));
            std::memset(abi.build_id, fill_byte, kRecompBuildIdSize);
            std::strncpy(abi.module_name, name, sizeof(abi.module_name) - 1);
        };
        fill(sparse_rtld, "rtld", 0xA0);
        fill(sparse_main, "main", 0xA1);
        fill(sparse_subsdk1, "subsdk1", 0xA2);
        fill(sparse_sdk, "sdk", 0xA3);

        RecompModuleMap sparse{};
        auto place_ok = [&](RecompImageAbiFn abi_fn) {
            RecompImageExports ex{};
            ex.lookup = DummyLookup;
            ex.set_base = +[](u64) {};
            ex.abi = abi_fn;
            return PlaceLoadedModule(sparse, ex) == ImageReject::Ok;
        };
        if (!place_ok([]() -> const RecompImageAbi* { return &sparse_rtld; }) ||
            !place_ok([]() -> const RecompImageAbi* { return &sparse_main; }) ||
            !place_ok([]() -> const RecompImageAbi* { return &sparse_subsdk1; }) ||
            !place_ok([]() -> const RecompImageAbi* { return &sparse_sdk; })) {
            fail("sparse rtld/main/subsdk1/sdk images failed to place on ABI slots");
        } else {
            pass("sparse modules placed on canonical ABI slots");
        }

        // Dense runtime indices as FindModules would number them.
        ApplyModuleBase(sparse, 0, "rtld", 0x100000ULL);
        ApplyModuleBase(sparse, 1, "main", 0x200000ULL);
        ApplyModuleBase(sparse, 2, "subsdk1", 0x400000ULL);
        ApplyModuleBase(sparse, 3, "sdk", 0x800000ULL);

        const auto* s_subsdk1 = SlotByIdentity(sparse, "subsdk1");
        const auto* s_sdk = SlotByIdentity(sparse, "sdk");
        if (!s_subsdk1 || !s_sdk) {
            fail("sparse layout lost subsdk1 or sdk slot");
        } else if (s_sdk->base != 0x800000ULL) {
            fail("rtld/main/subsdk1/sdk: sdk base overwritten by dense index 3");
        } else if (s_subsdk1->base != 0x400000ULL) {
            fail("rtld/main/subsdk1/sdk: subsdk1 base is not 0x400000");
        } else if (s_subsdk1->base == 0x800000ULL) {
            fail("rtld/main/subsdk1/sdk: sdk base=0 expected=800000; "
                 "subsdk1 base=800000 expected=0");
        } else {
            pass("sparse rtld/main/subsdk1/sdk bases follow ABI identity");
        }

        // Build-id match when the guest name does not equal the export filename.
        uint8_t sdk_id[kRecompBuildIdSize];
        std::memset(sdk_id, 0xA3, kRecompBuildIdSize);
        ApplyModuleBase(sparse, 99, "nnUnexpected", 0x900000ULL, sdk_id);
        if (SlotByIdentity(sparse, "sdk")->base != 0x900000ULL) {
            fail("build-id identity did not rebind sdk base");
        } else {
            pass("build-id identity rebinds module base");
        }
    }

    const auto* nn_main = SlotByName(map, "nnmain");
    if (!nn_main || nn_main != main_slot) {
        fail("nnmain did not match the main slot");
    } else {
        pass("nnmain matches main");
    }

    static RecompImageAbi unknown_abi = main_abi;
    unknown_abi.module_index = kRecompMaxModules;
    std::strncpy(unknown_abi.module_name, "abi", sizeof(unknown_abi.module_name) - 1);
    RecompModuleMap unknown_map{};
    RecompImageExports unknown_ex{};
    unknown_ex.lookup = DummyLookup;
    unknown_ex.set_base = +[](u64) {};
    unknown_ex.abi = []() -> const RecompImageAbi* { return &unknown_abi; };
    if (PlaceLoadedModule(unknown_map, unknown_ex) != ImageReject::IndexRange) {
        fail("unknown module_index occupied a load slot");
    } else {
        pass("unknown module_index rejected as out of range");
    }

    const fs::path out = root / "abi_export";
    fs::create_directories(out);
    u32 text[1] = {kSvc0};
    suyu::recomp::EmitProject("abi", reinterpret_cast<const suyu::recomp::u8*>(text), sizeof(text),
                              0x1000, out.string(), true);
    const std::string generated = ReadFile(out / "recomp_export.c");
    if (generated.find("recomp_image_abi") == std::string::npos) {
        fail("EmitProject export has no recomp_image_abi");
    } else {
        pass("EmitProject exports recomp_image_abi");
    }
    if (generated.find("build_id") == std::string::npos &&
        generated.find("0x11") == std::string::npos) {
        fail("EmitProject export has no content hash");
    } else {
        pass("EmitProject export carries a content hash");
    }
    const std::string unknown_index =
        std::to_string(kRecompRegsPrefixSize) + "u,\n  " + std::to_string(kRecompMaxModules) + "u,";
    if (generated.find(unknown_index) == std::string::npos) {
        fail("EmitProject unknown name stored module_index 0");
    } else {
        pass("EmitProject unknown name emits out-of-range module_index");
    }
}

std::string MakeManifest(uint32_t version, bool full_scan, uint32_t emitter, uint32_t abi,
                         std::string_view backend, std::string_view modules_json) {
    std::ostringstream out;
    out << "{\n"
        << "  \"version\": " << version << ",\n"
        << "  \"effective_backend\": \"" << backend << "\",\n"
        << "  \"full_scan\": " << (full_scan ? "true" : "false") << ",\n"
        << "  \"emitter_revision\": " << emitter << ",\n"
        << "  \"abi_version\": " << abi << ",\n"
        << "  \"modules\": [\n"
        << modules_json << "\n"
        << "  ]\n"
        << "}\n";
    return out.str();
}

void TestAotCacheReuse() {
    using suyu::recomp::AotCacheModuleIdentity;
    using suyu::recomp::AotCacheReject;
    using suyu::recomp::AotCacheReuseRequest;
    using suyu::recomp::EvaluateAotCacheReuse;
    using suyu::recomp::ReadNsoBuildId;
    using suyu::recomp::BuildIdToHexLower;
    using suyu::recomp::kRecompAotManifestVersion;
    using suyu::recomp::kRecompBuildIdSize;
    using suyu::recomp::kRecompEmitterRevision;
    using suyu::recomp::kRecompImageAbiVersion;

    const std::string main_id(64, '1');
    const std::string sdk_id(64, '2');
    const std::string modules = std::string("    {\"name\": \"main\", \"build_id\": \"") + main_id +
                                "\"},\n"
                                "    {\"name\": \"sdk\", \"build_id\": \"" +
                                sdk_id + "\"}";
    const std::string manifest =
        MakeManifest(kRecompAotManifestVersion, false, kRecompEmitterRevision,
                     kRecompImageAbiVersion, "dynarmic", modules);

    AotCacheReuseRequest req;
    req.full_scan = false;
    req.effective_backend = "dynarmic";
    req.has_recompiled_project = true;
    req.current_modules = {
        AotCacheModuleIdentity{"main", main_id},
        AotCacheModuleIdentity{"sdk", sdk_id},
    };
    if (!EvaluateAotCacheReuse(manifest, req).ok()) {
        fail("identical current modules should reuse AOT cache");
    } else {
        pass("AOT cache reused when module identities match");
    }

    AotCacheReuseRequest no_ids = req;
    no_ids.current_modules.clear();
    if (EvaluateAotCacheReuse(manifest, no_ids).reason != AotCacheReject::MissingRequiredIdentity) {
        fail("empty current module list was allowed to reuse cache");
    } else {
        pass("missing current identities invalidate AOT cache");
    }

    AotCacheReuseRequest updated = req;
    updated.current_modules[0].build_id_hex = std::string(64, 'a');
    const auto id_miss = EvaluateAotCacheReuse(manifest, updated);
    if (id_miss.reason != AotCacheReject::ModuleIdentity || id_miss.detail != "main") {
        fail("update build_id change did not invalidate as ModuleIdentity/main");
    } else {
        pass("title/update build_id change invalidates AOT cache");
    }

    AotCacheReuseRequest emitter = req;
    emitter.emitter_revision = kRecompEmitterRevision + 1;
    if (EvaluateAotCacheReuse(manifest, emitter).reason != AotCacheReject::EmitterRevision) {
        fail("emitter_revision bump did not invalidate cache");
    } else {
        pass("emitter revision change invalidates AOT cache");
    }

    const std::string old_manifest =
        MakeManifest(2, false, kRecompEmitterRevision, kRecompImageAbiVersion, "dynarmic", modules);
    if (EvaluateAotCacheReuse(old_manifest, req).reason != AotCacheReject::ManifestVersion) {
        fail("v2 manifest without identity schema was reused");
    } else {
        pass("legacy manifest version refused for reuse");
    }

    // NSO0 magic + build_id at 0x40
    std::vector<uint8_t> nso(0x60, 0);
    nso[0] = 'N';
    nso[1] = 'S';
    nso[2] = 'O';
    nso[3] = '0';
    std::memset(nso.data() + 0x40, 0x5a, kRecompBuildIdSize);
    uint8_t got[kRecompBuildIdSize]{};
    if (!ReadNsoBuildId(nso.data(), nso.size(), got) || got[0] != 0x5a) {
        fail("ReadNsoBuildId failed on synthetic NSO0 header");
    } else if (BuildIdToHexLower(got).substr(0, 2) != "5a") {
        fail("BuildIdToHexLower mismatch");
    } else {
        pass("NSO build_id read from module content");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("suyu-exporter-smoke-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    std::cout << "exporter smoke workdir: " << root << std::endl;
    TestTranslatedShape();
    TestEmitProjectCompile(root);
    TestBranchProbes(root);
    TestFpControl(root);
    TestUnresolvedImportPolicy();
    TestModuleRegistrationSession();
    TestCacheInvalidation();
    TestSharedImageAbi(root);
    TestAotCacheReuse();

    if (const char* ev = std::getenv("SUYU_SMOKE_EVIDENCE_DIR")) {
        const fs::path dest(ev);
        fs::create_directories(dest);
        const fs::path runtime = root / "emit_project" / "recomp_runtime.c";
        const fs::path probe = root / "branch_probe" / "probe.c";
        const fs::path fp_probe = root / "fp_probe" / "probe.c";
        if (fs::exists(runtime)) {
            fs::copy_file(runtime, dest / "recomp_runtime.c",
                          fs::copy_options::overwrite_existing);
        }
        if (fs::exists(probe)) {
            fs::copy_file(probe, dest / "branch_probe.c",
                          fs::copy_options::overwrite_existing);
        }
        if (fs::exists(fp_probe)) {
            fs::copy_file(fp_probe, dest / "fp_probe.c",
                          fs::copy_options::overwrite_existing);
        }
        std::cout << "copied evidence to " << dest << std::endl;
    }

    if (g_fails == 0) {
        fs::remove_all(root, ec);
        std::cout << "exporter_smoke: all checks passed" << std::endl;
        return 0;
    }
    std::cerr << "exporter_smoke: " << g_fails << " failure(s); keeping " << root << "\n";
    return 1;
}
