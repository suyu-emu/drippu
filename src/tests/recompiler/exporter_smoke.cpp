// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Portable acceptance check for the active AArch64 exporter (arm64_to_c.h):
//   1. EmitProject on a tiny supported sequence, then CMake-compile the project
//   2. RET Rn / BLR X30 probes compiled and executed as permanent regressions
//
// Deliberately does not invoke tools/static_recompiler.

#include "core/recompiler/arm64_to_c.h"
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

fs::path FindBuiltExe(const fs::path& build, const char* name) {
    const fs::path candidates[] = {
        build / name,
#ifdef _WIN32
        build / "Release" / (std::string(name) + ".exe"),
        build / "Debug" / (std::string(name) + ".exe"),
#else
        build / "Release" / name,
#endif
    };
    for (const fs::path& p : candidates) {
        if (fs::exists(p)) {
            return p;
        }
    }
    return {};
}

void TestMemoryBoundaries(const fs::path& root) {
    // Drive the actual RuntimeC helpers with a crafted page table: guest page 0
    // and page 1 map to nonadjacent host regions, and a second case leaves page 1
    // unmapped/tracked (null entry). Wide accesses at the page edge must take the
    // callback path; same-page accesses may stay on the fast path.
    const fs::path probe_src = root / "memory_probe";
    fs::create_directories(probe_src);
    if (!WriteFile(probe_src / "recomp_runtime.h", suyu::recomp::RuntimeH())) {
        return;
    }
    if (!WriteFile(probe_src / "recomp_runtime.c", suyu::recomp::RuntimeC())) {
        return;
    }
    if (!WriteFile(probe_src / "stub_lookup.c",
                   "void* recomp_lookup(unsigned long long pc){ (void)pc; return 0; }\n")) {
        return;
    }

    const char* probe_c = R"C(#include "recomp_runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PAGE_BITS = 12, PAGE_SIZE = 1 << PAGE_BITS, N_PAGES = 4 };

static uintptr_t page_entries[N_PAGES];
static unsigned char backing[PAGE_SIZE * 4];
static unsigned char* page0_host;
static unsigned char* page1_host;
static int callbacks;
/* page1 may be mapped discontiguous or have a null PTE (tracked/unmapped). */

static unsigned char* host_for(uint64_t va) {
  if (va < PAGE_SIZE) return page0_host;
  if (va < 2 * PAGE_SIZE) return page1_host;
  return 0;
}

static uint64_t probe_load(void* user, uint64_t va, uint32_t size) {
  (void)user;
  ++callbacks;
  uint64_t v = 0;
  for (uint32_t i = 0; i < size; ++i) {
    unsigned char* h = host_for(va + i);
    if (!h) return 0;
    ((unsigned char*)&v)[i] = h[(va + i) & (PAGE_SIZE - 1)];
  }
  return v;
}

static void probe_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
  (void)user;
  ++callbacks;
  for (uint32_t i = 0; i < size; ++i) {
    unsigned char* h = host_for(va + i);
    if (!h) return;
    h[(va + i) & (PAGE_SIZE - 1)] = ((unsigned char*)&value)[i];
  }
}

static void map_page(int page, unsigned char* host) {
  uint64_t va_base = (uint64_t)page << PAGE_BITS;
  page_entries[page] = host ? ((uintptr_t)host - (uintptr_t)va_base) : 0;
}

static int expect_eq(const char* name, uint64_t got, uint64_t want) {
  printf("%s: got=%llx want=%llx callbacks=%d\n", name,
         (unsigned long long)got, (unsigned long long)want, callbacks);
  return got != want;
}

int main(void) {
  int fail = 0;
  page0_host = backing;
  page1_host = backing + 2 * PAGE_SIZE;
  memset(backing, 0xCC, sizeof backing);

  RecompHostMem hm;
  memset(&hm, 0, sizeof hm);
  hm.load = probe_load;
  hm.store = probe_store;
  hm.page_entries = page_entries;
  hm.page_entry_stride = sizeof(uintptr_t);
  hm.page_bits = PAGE_BITS;
  hm.pointer_mask = ~(uintptr_t)0;
  hm.address_space_max = (uint64_t)N_PAGES << PAGE_BITS;

  GuestContext ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.host_mem = &hm;
  ctx.pending_svc = ~0ULL;

  map_page(0, page0_host);
  map_page(1, page1_host);
  memset(page0_host, 0, PAGE_SIZE);
  page0_host[0x100] = 0x11; page0_host[0x101] = 0x22;
  page0_host[0x102] = 0x33; page0_host[0x103] = 0x44;
  page0_host[0x104] = 0x55; page0_host[0x105] = 0x66;
  page0_host[0x106] = 0x77; page0_host[0x107] = 0x88;
  callbacks = 0;
  fail |= expect_eq("same-page load16", recomp_load16(&ctx, 0x100), 0x2211);
  if (callbacks != 0) { printf("same-page load16 took callback\n"); fail = 1; }
  callbacks = 0;
  fail |= expect_eq("same-page load32", recomp_load32(&ctx, 0x100), 0x44332211ULL);
  if (callbacks != 0) fail = 1;
  callbacks = 0;
  fail |= expect_eq("same-page load64", recomp_load64(&ctx, 0x100), 0x8877665544332211ULL);
  if (callbacks != 0) fail = 1;

  memset(backing, 0xCC, sizeof backing);
  page0_host[PAGE_SIZE - 1] = 0x11;
  page1_host[0] = 0x22;
  backing[PAGE_SIZE] = 0xAA;
  callbacks = 0;
  fail |= expect_eq("cross-page load16 discontig", recomp_load16(&ctx, PAGE_SIZE - 1), 0x2211);
  if (callbacks == 0) { printf("cross-page load16 skipped callback\n"); fail = 1; }

  page0_host[PAGE_SIZE - 1] = 0x11;
  page1_host[0] = 0x22;
  backing[PAGE_SIZE] = 0xAA;
  callbacks = 0;
  recomp_store16(&ctx, PAGE_SIZE - 1, 0x4433);
  printf("cross-page store16: p0=%02x p1=%02x gap=%02x callbacks=%d\n",
         page0_host[PAGE_SIZE - 1], page1_host[0], backing[PAGE_SIZE], callbacks);
  if (page0_host[PAGE_SIZE - 1] != 0x33 || page1_host[0] != 0x44 ||
      backing[PAGE_SIZE] != 0xAA || callbacks == 0) fail = 1;

  memset(page0_host, 0, PAGE_SIZE);
  memset(page1_host, 0, PAGE_SIZE);
  page0_host[PAGE_SIZE - 1] = 0x01;
  page1_host[0] = 0x02; page1_host[1] = 0x03; page1_host[2] = 0x04;
  callbacks = 0;
  fail |= expect_eq("cross-page load32 discontig", recomp_load32(&ctx, PAGE_SIZE - 1),
                    0x04030201ULL);
  if (callbacks == 0) fail = 1;

  memset(page0_host, 0, PAGE_SIZE);
  memset(page1_host, 0, PAGE_SIZE);
  page0_host[PAGE_SIZE - 1] = 0x09;
  for (int i = 0; i < 7; i++) page1_host[i] = (unsigned char)(0x10 + i);
  callbacks = 0;
  fail |= expect_eq("cross-page load64 discontig", recomp_load64(&ctx, PAGE_SIZE - 1),
                    0x1615141312111009ULL);
  if (callbacks == 0) fail = 1;

  memset(page0_host, 0, PAGE_SIZE);
  memset(page1_host, 0, PAGE_SIZE);
  page0_host[PAGE_SIZE - 1] = 0xA0;
  for (int i = 0; i < 15; i++) page1_host[i] = (unsigned char)(0xA1 + i);
  callbacks = 0;
  {
    uint64_t lo = recomp_load64(&ctx, PAGE_SIZE - 1);
    uint64_t hi = recomp_load64(&ctx, PAGE_SIZE - 1 + 8);
    printf("simd composed: lo=%llx hi=%llx callbacks=%d\n",
           (unsigned long long)lo, (unsigned long long)hi, callbacks);
    if (lo != 0xA7A6A5A4A3A2A1A0ULL || hi != 0xAFAEADACABAAA9A8ULL) fail = 1;
    if (callbacks == 0) fail = 1;
  }

  /* Tracked / unmapped second page: null PTE forces callback. */
  map_page(1, 0);
  memset(page0_host, 0, PAGE_SIZE);
  page0_host[PAGE_SIZE - 1] = 0x11;
  page1_host[0] = 0x22; page1_host[1] = 0x33; page1_host[2] = 0x44;
  page1_host[3] = 0x55; page1_host[4] = 0x66; page1_host[5] = 0x77;
  page1_host[6] = 0x88;
  callbacks = 0;
  fail |= expect_eq("cross-page load16 tracked", recomp_load16(&ctx, PAGE_SIZE - 1), 0x2211);
  if (callbacks == 0) { printf("tracked load16 skipped callback\n"); fail = 1; }
  callbacks = 0;
  fail |= expect_eq("cross-page load32 unmapped-pte", recomp_load32(&ctx, PAGE_SIZE - 1),
                    0x44332211ULL);
  if (callbacks == 0) fail = 1;
  callbacks = 0;
  fail |= expect_eq("cross-page load64 unmapped-pte", recomp_load64(&ctx, PAGE_SIZE - 1),
                    0x8877665544332211ULL);
  if (callbacks == 0) fail = 1;

  backing[PAGE_SIZE] = 0xAA;
  page0_host[PAGE_SIZE - 1] = 0x00;
  page1_host[0] = 0x00;
  callbacks = 0;
  recomp_store16(&ctx, PAGE_SIZE - 1, 0xBBAA);
  printf("tracked store16: p0=%02x p1=%02x gap=%02x callbacks=%d\n",
         page0_host[PAGE_SIZE - 1], page1_host[0], backing[PAGE_SIZE], callbacks);
  if (page0_host[PAGE_SIZE - 1] != 0xAA || page1_host[0] != 0xBB ||
      backing[PAGE_SIZE] != 0xAA || callbacks == 0) fail = 1;

  return fail;
}
)C";
    if (!WriteFile(probe_src / "probe.c", probe_c)) {
        return;
    }
    if (!WriteFile(probe_src / "CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.13)\n"
                   "project(suyu_memory_probe C)\n"
                   "set(CMAKE_C_STANDARD 11)\n"
                   "add_executable(memory_probe probe.c recomp_runtime.c stub_lookup.c)\n"
                   "target_include_directories(memory_probe PRIVATE "
                   "${CMAKE_CURRENT_SOURCE_DIR})\n")) {
        return;
    }

    const fs::path probe_build = probe_src / "build";
    if (CmakeBuild(probe_src, probe_build, "memory_probe", false) != 0) {
        return;
    }
    const fs::path exe = FindBuiltExe(probe_build, "memory_probe");
    if (exe.empty()) {
        fail("memory_probe executable not found under " + probe_build.string());
        return;
    }
    if (RunArgs({exe.string()}) != 0) {
        fail("memory_probe execution");
        return;
    }
    pass("page-edge load/store (discontig/tracked/unmapped) via RuntimeC");
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
    TestMemoryBoundaries(root);
    TestBranchProbes(root);
    TestFpControl(root);
    TestUnresolvedImportPolicy();
    TestModuleRegistrationSession();
    TestCacheInvalidation();
    TestSharedImageAbi(root);

    if (const char* ev = std::getenv("SUYU_SMOKE_EVIDENCE_DIR")) {
        const fs::path dest(ev);
        fs::create_directories(dest);
        const fs::path runtime = root / "emit_project" / "recomp_runtime.c";
        const fs::path mem_probe = root / "memory_probe" / "probe.c";
        const fs::path probe = root / "branch_probe" / "probe.c";
        const fs::path fp_probe = root / "fp_probe" / "probe.c";
        if (fs::exists(runtime)) {
            fs::copy_file(runtime, dest / "recomp_runtime.c",
                          fs::copy_options::overwrite_existing);
        }
        if (fs::exists(mem_probe)) {
            fs::copy_file(mem_probe, dest / "memory_probe.c",
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
