# SPDX-FileCopyrightText: Copyright 2026 drippu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared strict-warning configuration for the emulator tree (src/CMakeLists.txt)
# and the standalone recompiler smoke project (src/tests/recompiler). A single
# definition keeps the portable exporter-smoke CI honest: it must fail on the
# same warnings-as-errors that break the full build instead of passing with a
# non-fatal warning.
#
# Pure MSVC (cl.exe) is left to the tree's /W options; clang-cl is treated as
# Clang, matching the tree's `MSVC AND NOT CXX_CLANG` condition.

if (MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    return()
endif()

add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=all>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=extra>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=missing-declarations>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=shadow>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=unused>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-attributes>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-invalid-offsetof>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|Intel|IntelLLVM")
    if (NOT MSVC)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=shadow-uncaptured-local>
            $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=implicit-fallthrough>
            $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=type-limits>)
    endif()
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-braced-scalar-init>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-private-field>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-nullability-completeness>)
endif()
