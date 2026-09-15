#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

out="${1:-toolchain.txt}"
{
  echo "commit=${GITHUB_SHA:-unknown}"
  echo "runner_os=${RUNNER_OS:-unknown}"
  echo "runner_arch=${RUNNER_ARCH:-unknown}"
  echo "job=${GITHUB_JOB:-unknown}"
  if command -v cmake >/dev/null 2>&1; then
    cmake --version
  else
    echo "cmake: not found"
  fi
  if command -v clang++ >/dev/null 2>&1; then
    echo "compiler=clang++"
    clang++ --version | head -n 2
  elif command -v g++ >/dev/null 2>&1; then
    echo "compiler=g++"
    g++ --version | head -n 2
  elif command -v cl >/dev/null 2>&1; then
    echo "compiler=msvc"
    cl 2>&1 | head -n 3 || true
  else
    echo "compiler=unknown"
  fi
} >"$out"
