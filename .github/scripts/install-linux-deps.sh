#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Install the system dependencies required to build and package drippu on
# Ubuntu 24.04.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y ninja-build glslang-tools patchelf ccache \
  qt6-base-dev qt6-base-dev-tools qt6-base-private-dev qt6-qpa-plugins \
  qt6-wayland \
  qt6-multimedia-dev qt6-tools-dev qt6-svg-dev qt6-webengine-dev \
  qt6-charts-dev \
  libssl-dev libusb-1.0-0-dev libpulse-dev \
  libvulkan-dev gamemode-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxss-dev libxkbcommon-dev libxtst-dev \
  libwayland-dev libwayland-egl1 wayland-protocols libdecor-0-dev \
  libegl-dev libdrm-dev libgbm-dev \
  libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev \
  libboost-all-dev
