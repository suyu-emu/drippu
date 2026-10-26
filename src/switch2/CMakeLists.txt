# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.13)
project(suyu_switch2 CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(suyu_switch2 STATIC
    switch2.cpp
    switch2.h
)
target_include_directories(suyu_switch2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(suyu_switch2 PUBLIC cxx_std_20)
