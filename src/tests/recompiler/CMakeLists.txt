# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone (and optionally nested) exporter smoke: EmitProject compile plus
# RET/BLR branch probes against the active arm64_to_c.h header. Does not use
# tools/static_recompiler.

cmake_minimum_required(VERSION 3.13)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(suyu_exporter_smoke LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_C_STANDARD 11)
    enable_testing()
endif()

enable_language(C)

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
    TIMEOUT 180
)

# Real ArmRecomp + Dynarmic + kernel stack. Only when nested in the full
# emulator build (links `core`). Standalone recompiler/ CMake project skips it.
if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(recomp_stack_harness recomp_stack_harness.cpp)
    target_compile_features(recomp_stack_harness PRIVATE cxx_std_20)
    # frontend_common supplies PlayTimeManager referenced from core's NS services
    # once System symbols pull that translation unit into the link.
    target_link_libraries(recomp_stack_harness PRIVATE core common frontend_common dynarmic::dynarmic
                          ${CMAKE_DL_LIBS})
    target_include_directories(recomp_stack_harness PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME recomp_stack_harness COMMAND recomp_stack_harness)
    set_tests_properties(recomp_stack_harness PROPERTIES
        TIMEOUT 180
    )
endif()
