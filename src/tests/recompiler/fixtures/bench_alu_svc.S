// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Identical-PC JIT vs hybrid AOT benchmark fixture (drippu backlog #3).
// Not assembled by the build; encodings are hard-coded in recomp_stack_harness
// and translated through arm64_to_c.h.
//
// STR/LDR of x0 through the opaque host_mem callbacks sit on the ADD
// recurrence so gcc -O3 cannot strength-reduce 512 ADDs to `x1 << 9`.
// JudgeAotBenchDump: PLT jmp is not a loop (store+load + 1 add + PLT jmp
// + no shl/imul must FAIL). Live store/load pin is host_mem callback
// counts, not named `recomp_store64@plt`.
// X1 = 1 and X3 = data-segment scratch, both set by the harness.
// X1 = 1 and X3 = data-segment scratch, both set by the harness.
//
// Guest PC offset: 0x2400 (past the 8-byte STR at 0x1FFC).

.global bench_alu_svc
bench_alu_svc:
    movz x0, #0
    .rept 512
    str x0, [x3]
    ldr x0, [x3]
    add x0, x0, x1
    .endr
    svc #3
