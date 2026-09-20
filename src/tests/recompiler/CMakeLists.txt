# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone (and optionally nested) exporter smoke: EmitProject compile plus
# RET/BLR branch probes against the active arm64_to_c.h header. Does not use
# tools/static_recompiler.
#
# Nested full-tree target recomp_stack_harness also races JIT vs hybrid AOT
# (drippu backlog #3) and writes recomp_benchmark.json. Fold pin: discriminating
# PLT dump must FAIL (store+load + 1 add + PLT jmp is not a loop); live store/load
# is host_mem callback counts, not named recomp_store64@plt.
#
# Backlog #4: homebrew_harness runs Translate C vs independent ARM arithmetic
# (edge + random). recomp_stack_harness compares Translate AOT vs Dynarmic.
# -DSUYU_ENABLE_SANITIZERS=ON (standalone) or -DYUZU_ENABLE_SANITIZERS=ON (tree)
# builds ASan+UBSan and forwards the flags into nested probe/AOT compiles. CI
# runs this natively on both x86-64 and ARM64 so generated code is checked with
# the host ABI it will execute under.

cmake_minimum_required(VERSION 3.13)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(suyu_exporter_smoke LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_C_STANDARD 11)
    enable_testing()
endif()

enable_language(C)

# ASan+UBSan for instruction-correctness / generated C. Nested AOT/probe
# compiles inherit the flags via smoke_config.h (SUYU_SMOKE_SANITIZER_FLAGS).
# GitHub tests.yml wires this on ubuntu-24.04 (x86-64). There is no ARM64
# sanitizer runner in this repo's workflows.
if (NOT DEFINED SUYU_ENABLE_SANITIZERS)
    option(SUYU_ENABLE_SANITIZERS "ASan+UBSan for recompiler tests (x86-64)" OFF)
endif()
if (DEFINED YUZU_ENABLE_SANITIZERS AND YUZU_ENABLE_SANITIZERS)
    set(SUYU_ENABLE_SANITIZERS ON)
endif()
if (SUYU_ENABLE_SANITIZERS)
    if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    set(SUYU_SMOKE_SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
else()
    set(SUYU_SMOKE_SANITIZER_FLAGS "")
endif()

add_executable(exporter_smoke exporter_smoke.cpp)
target_compile_features(exporter_smoke PRIVATE cxx_std_20)
target_include_directories(exporter_smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(homebrew_harness homebrew_harness.cpp)
target_compile_features(homebrew_harness PRIVATE cxx_std_20)
target_include_directories(homebrew_harness PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "${CMAKE_CURRENT_BINARY_DIR}"
)

file(TO_CMAKE_PATH "${CMAKE_COMMAND}" SUYU_SMOKE_CMAKE)
file(TO_CMAKE_PATH "${CMAKE_C_COMPILER}" SUYU_SMOKE_C_COMPILER)
set(SUYU_SMOKE_GENERATOR "${CMAKE_GENERATOR}")
set(SUYU_SMOKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
configure_file(smoke_config.h.in smoke_config.h @ONLY)

add_test(NAME exporter_smoke COMMAND exporter_smoke)
set_tests_properties(exporter_smoke PROPERTIES
    TIMEOUT 180
)

add_test(NAME homebrew_harness COMMAND homebrew_harness)
set_tests_properties(homebrew_harness PROPERTIES
    TIMEOUT 240
)

# Real ArmRecomp + Dynarmic + kernel stack. Only when nested in the full
# emulator build (links `core`). Standalone recompiler/ CMake project skips it.
if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(recomp_stack_harness recomp_stack_harness.cpp)
    target_compile_features(recomp_stack_harness PRIVATE cxx_std_20)
    # frontend_common supplies PlayTimeManager referenced from core's NS services
    # once System symbols pull that translation unit into the link.
    target_link_libraries(recomp_stack_harness PRIVATE core common frontend_common dynarmic::dynarmic
                          ${PLATFORM_LIBRARIES} ${CMAKE_DL_LIBS})
    target_include_directories(recomp_stack_harness PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME recomp_stack_harness COMMAND recomp_stack_harness)
    set_tests_properties(recomp_stack_harness PROPERTIES
        TIMEOUT 360
    )
endif()
