// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "core/frontend/emu_window.h"

namespace InputCommon::TasInput {
class Tas;
}

namespace LibretroCore {

// Minimal EmuWindow for the libretro core frontend. Runs the video backend
// in WindowSystemType::Headless mode (render_surface = nullptr), which
// video_core already supports natively - see core/frontend/emu_window.h's
// WindowSystemInfo::render_surface doc comment. This lets Core::System boot
// and run a game with no real window at all.
//
// NOT YET IMPLEMENTED: presenting decoded frames back through libretro's
// retro_video_refresh callback. That requires either (a) a Vulkan
// RETRO_HW_RENDER_INTERFACE bridge so RetroArch supplies the swapchain
// surface directly, or (b) a CPU-side readback path (vkCmdCopyImageToBuffer
// on the presented swapchain image, mapped to host memory) - video_core's
// RendererBase currently exposes neither. This is real, separate work on
// the Vulkan renderer itself, not something to fake here.
class RetroEmuWindow final : public Core::Frontend::EmuWindow {
public:
    RetroEmuWindow();
    ~RetroEmuWindow() override;

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
    bool IsShown() const override;
    void SetTasPlayback(InputCommon::TasInput::Tas* tas);
    void OnFrameDisplayed() override;

    /// Frames the renderer has composited; the headless frame buffer changes only when this does.
    u64 FramesDisplayed() const {
        return frames_displayed.load(std::memory_order_acquire);
    }

private:
    std::atomic<u64> frames_displayed{};
    InputCommon::TasInput::Tas* tas_playback{};
    u64 tas_completion_generation{};
    u64 tas_frame_callbacks{};
};

} // namespace LibretroCore
