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
//
// Conditional branches (B.cond/CBZ/CBNZ/TBZ/TBNZ) appear only in
// ReferenceBlocks, not in Catalog: they set c->pc and return, so a single
// hosted function cannot observe them the way the x[0]-checking probe does.
// The stack harness covers them via paired blocks whose taken target is a
// registered block start, comparing final GPRs, NZCV, and PC against
// Dynarmic in both directions.

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
constexpr int kInsnBlockCount = 14;

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
    // P0: shifted flag-setting ALU (ADDS/SUBS/ANDS/BICS/TST with a nonzero
    // shift; ROR is reserved for ADD/SUB and is covered via ANDS/BICS).
    Adds64Lsl1,
    Adds64Lsl63,
    Subs64Asr31,
    Adds32Lsl2,
    Ands64Lsr7,
    Ands32Asr5,
    Bics64Ror13,
    Tst64Lsl3,
    // P0: add/subtract with carry.
    Adc64,
    Sbc64,
    Adcs64,
    Sbcs64,
    Adcs32,
    // P0: conditional compare (register and immediate forms).
    Ccmn64,
    Ccmp64,
    CcmpImm64,
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

// Shifted-register ADD/SUB with an explicit shift type (0=LSL, 1=LSR, 2=ASR)
// and amount. ROR is architecturally reserved for ADD/SUB and is rejected by
// the translator, so it is not representable here.
constexpr u32 EncAddShiftedEx(bool sf, bool sub, bool setflags, u32 rd, u32 rn, u32 rm, u32 shift,
                              u32 imm6) {
    return (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (setflags ? 0x20000000u : 0) |
           0x0B000000u | ((shift & 3) << 22) | (rm << 16) | ((imm6 & 0x3F) << 10) | (rn << 5) | rd;
}

// ADC/SBC/ADCS/SBCS: op selects SBC, setflags selects the S variant.
constexpr u32 EncAdcSbc(bool sf, bool sub, bool setflags, u32 rd, u32 rn, u32 rm) {
    return (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (setflags ? 0x20000000u : 0) |
           0x1A000000u | (rm << 16) | (rn << 5) | rd;
}

// CCMN/CCMP, register and immediate forms. cond is the 4-bit ARM condition,
// nzcv is the 4-bit flag literal loaded when the condition is false.
constexpr u32 EncCcmp(bool sf, bool ccmp, bool is_imm, u32 rn, u32 rm_imm5, u32 cond, u32 nzcv) {
    return (sf ? 0x80000000u : 0) | (ccmp ? 0x40000000u : 0) | 0x3A400000u |
           ((is_imm ? 1u : 0u) << 11) | (rm_imm5 << 16) | ((cond & 15) << 12) | (rn << 5) |
           (nzcv & 15);
}

// CBZ/CBNZ. imm19 is the signed word displacement from the branch.
constexpr u32 EncCbz(bool cbnz, bool sf, u32 rt, s32 imm19) {
    return (sf ? 0x80000000u : 0) | (cbnz ? 0x35000000u : 0x34000000u) |
           ((static_cast<u32>(imm19) & 0x7FFFFu) << 5) | (rt & 31);
}

// TBZ/TBNZ. imm14 is the signed word displacement from the branch.
constexpr u32 EncTbz(bool tbnz, u32 rt, u32 bit, s32 imm14) {
    return (((bit >> 5) & 1u) << 31) | (tbnz ? 0x37000000u : 0x36000000u) |
           (((bit & 31) & 0x1Fu) << 19) | ((static_cast<u32>(imm14) & 0x3FFFu) << 5) | (rt & 31);
}

// B.cond. imm19 is the signed word displacement from the branch.
constexpr u32 EncBCond(u32 cond, s32 imm19) {
    return 0x54000000u | ((static_cast<u32>(imm19) & 0x7FFFFu) << 5) | (cond & 15);
}

// Word displacement between two ReferenceBlocks, for branch encodings.
// Branch targets must land exactly on a registered block start: the stack
// harness dispatcher's Lookup only resolves stride-aligned PCs, so a branch
// to any other PC would fall back to Dynarmic instead of staying in AOT.
constexpr s32 BranchWords(int from_block, int to_block) {
    return static_cast<s32>((to_block - from_block) * (kInsnStride / 4));
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
        // P0: shifted flag-setting ALU. Amounts span 1/63 (64-bit) and the
        // 32-bit forms stay below the architecturally-valid imm6 < 32.
        {"ADDS X0,X1,X2,LSL#1", Kind::Adds64Lsl1, EncAddShiftedEx(true, false, true, 0, 1, 2, 0, 1)},
        {"ADDS X0,X1,X2,LSL#63", Kind::Adds64Lsl63,
         EncAddShiftedEx(true, false, true, 0, 1, 2, 0, 63)},
        {"SUBS X0,X1,X2,ASR#31", Kind::Subs64Asr31,
         EncAddShiftedEx(true, true, true, 0, 1, 2, 2, 31)},
        {"ADDS W0,W1,W2,LSL#2", Kind::Adds32Lsl2,
         EncAddShiftedEx(false, false, true, 0, 1, 2, 0, 2)},
        {"ANDS X0,X1,X2,LSR#7", Kind::Ands64Lsr7,
         EncLogicalShifted(true, 3, false, 0, 1, 2, 1, 7)},
        {"ANDS W0,W1,W2,ASR#5", Kind::Ands32Asr5,
         EncLogicalShifted(false, 3, false, 0, 1, 2, 2, 5)},
        {"BICS X0,X1,X2,ROR#13", Kind::Bics64Ror13,
         EncLogicalShifted(true, 3, true, 0, 1, 2, 3, 13)},
        {"TST X1,X2,LSL#3", Kind::Tst64Lsl3, EncLogicalShifted(true, 3, false, 31, 1, 2, 0, 3)},
        // P0: add/subtract with carry. The hosted harness alternates the
        // carry preset, so carry-in clear/set is covered per trial.
        {"ADC X0,X1,X2", Kind::Adc64, EncAdcSbc(true, false, false, 0, 1, 2)},
        {"SBC X0,X1,X2", Kind::Sbc64, EncAdcSbc(true, true, false, 0, 1, 2)},
        {"ADCS X0,X1,X2", Kind::Adcs64, EncAdcSbc(true, false, true, 0, 1, 2)},
        {"SBCS X0,X1,X2", Kind::Sbcs64, EncAdcSbc(true, true, true, 0, 1, 2)},
        {"ADCS W0,W1,W2", Kind::Adcs32, EncAdcSbc(false, false, true, 0, 1, 2)},
        // P0: conditional compare. CCMP/EQ is always false under the probe's
        // Z=0 preset (literal-nzcv path), CCMN/NE always true (compare path),
        // and CCMP/CS flips with the alternating carry preset.
        {"CCMN X1,X2,#0xA,NE", Kind::Ccmn64, EncCcmp(true, false, false, 1, 2, 1, 0xA)},
        {"CCMP X1,X2,#0x5,EQ", Kind::Ccmp64, EncCcmp(true, true, false, 1, 2, 0, 0x5)},
        {"CCMP X1,#7,#0x3,CS", Kind::CcmpImm64, EncCcmp(true, true, true, 1, 7, 2, 0x3)},
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
        // P0: shifted flag-setting ALU plus ADC/SBC and CCMN/CCMP, mirroring
        // the new catalog entries so the stack harness checks them against
        // Dynarmic as well. The ADC/SBC carry-in here is the TST's C (=0); the
        // hosted harness varies it via the carry preset.
        {"p0_alu", kOffInsn + 6 * kInsnStride,
         park({
             EncAddShiftedEx(true, false, true, 0, 1, 2, 0, 1),  // ADDS X0,X1,X2,LSL#1
             EncAddShiftedEx(true, false, true, 3, 4, 5, 0, 63), // ADDS X3,X4,X5,LSL#63
             EncAddShiftedEx(true, true, true, 6, 1, 2, 2, 31),  // SUBS X6,X1,X2,ASR#31
             EncAddShiftedEx(false, false, true, 7, 4, 5, 0, 2), // ADDS W7,W4,W5,LSL#2
             EncLogicalShifted(true, 3, false, 8, 1, 2, 1, 7),   // ANDS X8,X1,X2,LSR#7
             EncLogicalShifted(false, 3, false, 9, 4, 5, 2, 5),  // ANDS W9,W4,W5,ASR#5
             EncLogicalShifted(true, 3, true, 10, 1, 2, 3, 13),  // BICS X10,X1,X2,ROR#13
             EncLogicalShifted(true, 3, false, 31, 1, 2, 0, 3),  // TST X1,X2,LSL#3
             EncAdcSbc(true, false, false, 11, 1, 2),            // ADC X11,X1,X2
             EncAdcSbc(true, true, false, 12, 1, 2),             // SBC X12,X1,X2
             EncAdcSbc(true, false, true, 13, 1, 2),             // ADCS X13,X1,X2
             EncAdcSbc(true, true, true, 14, 1, 2),             // SBCS X14,X1,X2
             EncAdcSbc(false, false, true, 15, 4, 5),            // ADCS W15,W4,W5
             EncCcmp(true, false, false, 1, 2, 1, 0xA),          // CCMN X1,X2,#0xA,NE
             EncCcmp(true, true, false, 1, 2, 0, 0x5),           // CCMP X1,X2,#0x5,EQ
             EncCcmp(true, true, true, 1, 7, 2, 0x3),           // CCMP X1,#7,#0x3,CS
         })},
        // P0: shifted ANDS/BICS must clear C/V (matches the existing
        // logic_flags pin: the scenario asserts C=V=0 at the park).
        {"p0_logic_flags", kOffInsn + 7 * kInsnStride,
         park({
             EncLogicalShifted(true, 3, false, 0, 1, 2, 1, 7), // ANDS X0,X1,X2,LSR#7
             EncLogicalShifted(true, 3, true, 3, 1, 2, 3, 13),  // BICS X3,X1,X2,ROR#13
         })},
        // P0: conditional branches. Each branch block's taken target is the
        // shared b_taken block (index 13); the fallthrough parks in the branch
        // block itself. Taken and not-taken therefore park at different PCs,
        // so a wrong direction fails the final state comparison in either
        // case. Edge presets below force each direction deterministically.
        {"b_cbz", kOffInsn + 8 * kInsnStride,
         {EncCbz(false, true, 0, BranchWords(8, 13)), kSvcPark}},
        {"b_cbnz", kOffInsn + 9 * kInsnStride,
         {EncCbz(true, true, 0, BranchWords(9, 13)), kSvcPark}},
        {"b_tbz", kOffInsn + 10 * kInsnStride,
         {EncTbz(false, 0, 3, BranchWords(10, 13)), kSvcPark}},
        {"b_tbnz", kOffInsn + 11 * kInsnStride,
         {EncTbz(true, 0, 3, BranchWords(11, 13)), kSvcPark}},
        {"b_beq", kOffInsn + 12 * kInsnStride,
         {EncAddShiftedEx(true, true, true, 31, 0, 1, 0, 0), // SUBS XZR,X0,X1
          EncBCond(0, BranchWords(12, 13) - 1),              // B.EQ -> b_taken
          kSvcPark}},
        {"b_taken", kOffInsn + 13 * kInsnStride,
         {EncMovWide(2, true, 11, 0xB7, 0), kSvcPark}}, // MOVZ X11,#0xB7 marker
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
