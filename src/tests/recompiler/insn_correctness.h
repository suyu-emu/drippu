// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared AArch64 instruction catalog for drippu backlog #4.
// Encodings are consumed by:
//   - homebrew_harness: Translate() → hosted C execution vs independent ARM
//     arithmetic (edge cases + randomized inputs)
//   - recomp_stack_harness: same encodings in guest RX, Translate AOT vs
//     in-tree Dynarmic (the production reference backend)
//
// Do not treat this header as a second emulator: expected values for the
// hosted path are plain C++/C arithmetic for a fixed whitelist of templates.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/recompiler/arm64_to_c.h"

namespace suyu::recomp::insn_test {

using u32 = suyu::recomp::u32;
using u64 = suyu::recomp::u64;

constexpr u32 kInsnSvcImm = 4;
constexpr u32 kSvcPark = 0xD4000001u | (kInsnSvcImm << 5); // SVC #4

// Guest layout used by recomp_stack_harness. One 4KiB page after the ADD bench.
constexpr u64 kOffInsn = 0x4000;
constexpr u64 kInsnStride = 0x80;
constexpr int kInsnBlockCount = 6;

enum class Kind : int {
    Add64 = 1,
    Add32,
    Sub64,
    Sub32,
    Adds64,
    Subs64,
    And64,
    Orr64,
    Eor64,
    Ands64,
    BicAsr31W, // BIC W0, W0, W0, ASR #31  (max(x,0) idiom)
    AddImm64,
    Movz64,
    Movk64,
    Movn64,
    Udiv64,
    Sdiv64,
    Udiv32,
    Sdiv32,
    Lslv64,
    Lsrv64,
    Asrv64,
    Rorv64,
    Lslv32,
    Asrv32,
    Madd64,
    Str64,
    Ldr64,
    Strb,
    Ldrb,
    Ldrsb64,
};

struct NamedInsn {
    const char* name;
    Kind kind;
    u32 encoding;
};

// AArch64 data-processing encodings. rd/rn/rm in 0..31.
constexpr u32 EncAddShifted(bool sf, bool sub, bool setflags, u32 rd, u32 rn, u32 rm) {
    return (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (setflags ? 0x20000000u : 0) |
           0x0B000000u | (rm << 16) | (rn << 5) | rd;
}

constexpr u32 EncLogicalShifted(bool sf, u32 opc, bool invert, u32 rd, u32 rn, u32 rm, u32 shift,
                                u32 imm6) {
    return (sf ? 0x80000000u : 0) | ((opc & 3) << 29) | 0x0A000000u | ((shift & 3) << 22) |
           (invert ? (1u << 21) : 0) | (rm << 16) | ((imm6 & 0x3F) << 10) | (rn << 5) | rd;
}

constexpr u32 EncDp2Src(bool sf, u32 opcode, u32 rd, u32 rn, u32 rm) {
    return (sf ? 0x80000000u : 0) | 0x1AC00000u | (rm << 16) | ((opcode & 0x3F) << 10) | (rn << 5) |
           rd;
}

constexpr u32 EncMadd(bool sf, bool msub, u32 rd, u32 rn, u32 rm, u32 ra) {
    return (sf ? 0x80000000u : 0) | 0x1B000000u | (rm << 16) | (msub ? (1u << 15) : 0) |
           (ra << 10) | (rn << 5) | rd;
}

constexpr u32 EncMovWide(u32 opc, bool sf, u32 hw, u32 imm16, u32 rd) {
    return (sf ? 0x80000000u : 0) | ((opc & 3) << 29) | 0x12800000u | ((hw & 3) << 21) |
           ((imm16 & 0xFFFF) << 5) | rd;
}

constexpr u32 EncAddImm(bool sf, bool sub, bool setflags, u32 rd, u32 rn, u32 imm12) {
    return (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (setflags ? 0x20000000u : 0) |
           0x11000000u | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}

constexpr u32 EncLdrStrUoff(u32 size, u32 opc, u32 rt, u32 rn, u32 imm12) {
    return ((size & 3) << 30) | 0x39000000u | ((opc & 3) << 22) | ((imm12 & 0xFFF) << 10) |
           (rn << 5) | rt;
}

// Fixed catalog. Dest is X0 (or W0); ALU sources X1/X2; MADD addend X3;
// addresses in X1 for memory ops. MOV wide ops bake the immediate.
inline std::vector<NamedInsn> Catalog() {
    return {
        {"ADD X0,X1,X2", Kind::Add64, EncAddShifted(true, false, false, 0, 1, 2)},
        {"ADD W0,W1,W2", Kind::Add32, EncAddShifted(false, false, false, 0, 1, 2)},
        {"SUB X0,X1,X2", Kind::Sub64, EncAddShifted(true, true, false, 0, 1, 2)},
        {"SUB W0,W1,W2", Kind::Sub32, EncAddShifted(false, true, false, 0, 1, 2)},
        {"ADDS X0,X1,X2", Kind::Adds64, EncAddShifted(true, false, true, 0, 1, 2)},
        {"SUBS X0,X1,X2", Kind::Subs64, EncAddShifted(true, true, true, 0, 1, 2)},
        {"AND X0,X1,X2", Kind::And64, EncLogicalShifted(true, 0, false, 0, 1, 2, 0, 0)},
        {"ORR X0,X1,X2", Kind::Orr64, EncLogicalShifted(true, 1, false, 0, 1, 2, 0, 0)},
        {"EOR X0,X1,X2", Kind::Eor64, EncLogicalShifted(true, 2, false, 0, 1, 2, 0, 0)},
        {"ANDS X0,X1,X2", Kind::Ands64, EncLogicalShifted(true, 3, false, 0, 1, 2, 0, 0)},
        {"BIC W0,W0,W0,ASR#31", Kind::BicAsr31W,
         EncLogicalShifted(false, 0, true, 0, 0, 0, 2, 31)},
        {"ADD X0,X1,#1", Kind::AddImm64, EncAddImm(true, false, false, 0, 1, 1)},
        {"MOVZ X0,#0x1234", Kind::Movz64, EncMovWide(2, true, 0, 0x1234, 0)},
        {"MOVK X0,#0xABCD,LSL#16", Kind::Movk64, EncMovWide(3, true, 1, 0xABCD, 0)},
        {"MOVN X0,#0", Kind::Movn64, EncMovWide(0, true, 0, 0, 0)},
        {"UDIV X0,X1,X2", Kind::Udiv64, EncDp2Src(true, 2, 0, 1, 2)},
        {"SDIV X0,X1,X2", Kind::Sdiv64, EncDp2Src(true, 3, 0, 1, 2)},
        {"UDIV W0,W1,W2", Kind::Udiv32, EncDp2Src(false, 2, 0, 1, 2)},
        {"SDIV W0,W1,W2", Kind::Sdiv32, EncDp2Src(false, 3, 0, 1, 2)},
        {"LSLV X0,X1,X2", Kind::Lslv64, EncDp2Src(true, 8, 0, 1, 2)},
        {"LSRV X0,X1,X2", Kind::Lsrv64, EncDp2Src(true, 9, 0, 1, 2)},
        {"ASRV X0,X1,X2", Kind::Asrv64, EncDp2Src(true, 10, 0, 1, 2)},
        {"RORV X0,X1,X2", Kind::Rorv64, EncDp2Src(true, 11, 0, 1, 2)},
        {"LSLV W0,W1,W2", Kind::Lslv32, EncDp2Src(false, 8, 0, 1, 2)},
        {"ASRV W0,W1,W2", Kind::Asrv32, EncDp2Src(false, 10, 0, 1, 2)},
        {"MADD X0,X1,X2,X3", Kind::Madd64, EncMadd(true, false, 0, 1, 2, 3)},
        {"STR X0,[X1]", Kind::Str64, EncLdrStrUoff(3, 0, 0, 1, 0)},
        {"LDR X0,[X1]", Kind::Ldr64, EncLdrStrUoff(3, 1, 0, 1, 0)},
        {"STRB W0,[X1]", Kind::Strb, EncLdrStrUoff(0, 0, 0, 1, 0)},
        {"LDRB W0,[X1]", Kind::Ldrb, EncLdrStrUoff(0, 1, 0, 1, 0)},
        {"LDRSB X0,[X1]", Kind::Ldrsb64, EncLdrStrUoff(0, 2, 0, 1, 0)},
    };
}

struct RefBlock {
    const char* name;
    u64 offset;
    std::vector<u32> insns;
};

// Grouped sequences for the stack harness: Translate AOT and Dynarmic execute
// the same bytes, then park on SVC #4.
inline std::vector<RefBlock> ReferenceBlocks() {
    auto park = [](std::vector<u32> v) {
        v.push_back(kSvcPark);
        return v;
    };
    return {
        {"alu", kOffInsn + 0 * kInsnStride,
         park({
             EncAddShifted(true, false, false, 0, 1, 2),
             EncAddShifted(false, false, false, 3, 4, 5),
             EncAddShifted(true, true, false, 6, 1, 2),
             EncLogicalShifted(true, 0, false, 7, 1, 2, 0, 0),
             EncLogicalShifted(true, 1, false, 8, 1, 2, 0, 0),
             EncLogicalShifted(true, 2, false, 9, 1, 2, 0, 0),
             EncMovWide(2, true, 0, 0x1234, 10),
             EncMovWide(3, true, 1, 0xABCD, 10),
             EncMovWide(0, true, 0, 0, 11),
             EncAddImm(true, false, false, 12, 1, 1),
             EncMadd(true, false, 13, 1, 2, 3),
         })},
        {"flags", kOffInsn + 1 * kInsnStride,
         park({
             EncAddShifted(true, false, true, 0, 1, 2), // ADDS
             EncAddShifted(true, true, true, 16, 1, 2), // SUBS X16 (keep NZCV)
         })},
        {"logic_flags", kOffInsn + 2 * kInsnStride,
         park({
             EncLogicalShifted(true, 3, false, 0, 1, 2, 0, 0), // ANDS; A64 C=V=0
         })},
        {"shift_div", kOffInsn + 3 * kInsnStride,
         park({
             EncDp2Src(true, 8, 0, 1, 2),   // LSLV
             EncDp2Src(true, 9, 4, 1, 2),   // LSRV
             EncDp2Src(true, 10, 5, 1, 2),  // ASRV
             EncDp2Src(true, 11, 6, 1, 2),  // RORV
             EncDp2Src(false, 8, 7, 1, 2),  // LSLV W
             EncDp2Src(false, 10, 8, 1, 2), // ASRV W
             EncDp2Src(true, 2, 9, 1, 2),   // UDIV
             EncDp2Src(true, 3, 10, 1, 2),  // SDIV
             EncDp2Src(false, 2, 11, 1, 2), // UDIV W
             EncDp2Src(false, 3, 12, 1, 2), // SDIV W
             EncLogicalShifted(false, 0, true, 13, 13, 13, 2, 31), // BIC ASR#31 into x13
         })},
        {"mem", kOffInsn + 4 * kInsnStride,
         park({
             EncLdrStrUoff(3, 0, 2, 1, 0), // STR X2,[X1]
             EncLdrStrUoff(3, 1, 0, 1, 0), // LDR X0,[X1]
             EncLdrStrUoff(0, 0, 3, 1, 8), // STRB W3,[X1,#8]
             EncLdrStrUoff(0, 1, 4, 1, 8), // LDRB W4,[X1,#8]
             EncLdrStrUoff(0, 2, 5, 1, 8), // LDRSB X5,[X1,#8]
         })},
        {"edge_sdiv", kOffInsn + 5 * kInsnStride,
         park({
             EncDp2Src(true, 3, 0, 1, 2),  // SDIV X0,X1,X2
             EncDp2Src(false, 3, 3, 4, 5), // SDIV W3,W4,W5
             EncDp2Src(true, 2, 6, 1, 2),  // UDIV (incl. /0)
         })},
    };
}

struct XorShift64 {
    u64 s;
    explicit XorShift64(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    u64 next() {
        u64 x = s;
        x ^= x << 7;
        x ^= x >> 9;
        x ^= x << 8;
        s = x;
        return x;
    }
};

inline int RandomTrials() {
    int n = 32;
    if (const char* env = std::getenv("SUYU_INSN_RANDOM_TRIALS"); env && env[0] != '\0') {
        n = std::atoi(env);
    }
    return n < 4 ? 4 : (n > 256 ? 256 : n);
}

inline u64 RandomSeed() {
    u64 s = 1;
    if (const char* env = std::getenv("SUYU_INSN_SEED"); env && env[0] != '\0') {
        s = std::strtoull(env, nullptr, 0);
    }
    return s ? s : 1;
}

inline bool TranslateOk(u32 encoding, u64 pc, std::string* body_out = nullptr) {
    std::string body;
    bool unhandled = false;
    suyu::recomp::Translate(encoding, pc, body, &unhandled);
    if (body_out) {
        *body_out = std::move(body);
    }
    return !unhandled;
}

} // namespace suyu::recomp::insn_test
