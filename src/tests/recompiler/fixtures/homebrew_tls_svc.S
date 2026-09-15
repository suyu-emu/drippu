// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal guest instruction fixtures for homebrew_harness.
// Not assembled by the build; encodings are hard-coded in the harness and
// translated through arm64_to_c.h. Kept here so the expected guest sequence
// is reviewable without dumping the generated probe.c.

.global homebrew_tls_svc
homebrew_tls_svc:
    movz x0, #0x1234
    msr tpidr_el0, x0
    mrs x1, tpidr_el0
    mrs x3, tpidrro_el0
    movz x2, #0xabcd
    str x2, [x4]
    svc #42

.global homebrew_unhandled
homebrew_unhandled:
    brk #0

.global homebrew_plain
homebrew_plain:
    movz x0, #7
    ret
