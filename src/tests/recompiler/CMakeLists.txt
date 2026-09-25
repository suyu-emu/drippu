# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone (and optionally nested) exporter smoke: EmitProject compile plus
# RET/BLR branch probes against the active arm64_to_c.h header. Does not use
# tools/static_recompiler.
#
# The former full-stack harness targets the retired image ABI and metrics API.
# The active ABI 5/6 generated-image and host guard contracts are exercised by
# tests/recompiler_smoke/run.py in CI.
#
# homebrew_harness runs Translate C vs independent ARM arithmetic (edge and
# random cases). The ABI smoke suite exercises emitted modules and host guards.
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
# GitHub tests.yml wires this on x86-64 and ARM64 Linux runners.
if (NOT DEFINED SUYU_ENABLE_SANITIZERS)
    option(SUYU_ENABLE_SANITIZERS "ASan+UBSan for recompiler tests" OFF)
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
