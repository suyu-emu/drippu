// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include "common/common_funcs.h"
#include "hid_core/hid_types.h"

namespace Kernel {
class KernelCore;
}

namespace Core::HID {
class EmulatedConsole;
class EmulatedController;
class EmulatedDevices;
} // namespace Core::HID

namespace Core::HID {

class HIDCore {
public:
    explicit HIDCore(Kernel::KernelCore& kernel);
    ~HIDCore();

    YUZU_NON_COPYABLE(HIDCore);
    YUZU_NON_MOVEABLE(HIDCore);

    EmulatedController* GetEmulatedController(NpadIdType npad_id_type);
    const EmulatedController* GetEmulatedController(NpadIdType npad_id_type) const;

    EmulatedController* GetEmulatedControllerByIndex(std::size_t index);
    const EmulatedController* GetEmulatedControllerByIndex(std::size_t index) const;

    EmulatedConsole* GetEmulatedConsole();
    const EmulatedConsole* GetEmulatedConsole() const;

    EmulatedDevices* GetEmulatedDevices();
    const EmulatedDevices* GetEmulatedDevices() const;

    void SetSupportedStyleTag(NpadStyleTag style_tag);
    NpadStyleTag GetSupportedStyleTag() const;

    /// Counts the connected players from P1-P8
    s8 GetPlayerCount() const;

    /// Returns the first connected npad id
    NpadIdType GetFirstNpadId() const;

    /// Returns the first disconnected npad id
    NpadIdType GetFirstDisconnectedNpadId() const;

    /// Sets the npad id of the last active controller
    void SetLastActiveController(NpadIdType npad_id);

    /// Returns the npad id of the last controller that pushed a button
    NpadIdType GetLastActiveController() const;

    /// Sets all emulated controllers into configuring mode.
    void EnableAllControllerConfiguration();

    /// Sets all emulated controllers into normal mode.
    void DisableAllControllerConfiguration();

    /// Reloads all input devices from settings
    void ReloadInputDevices();

    /// Removes all callbacks from input common
    void UnloadInputDevices();

    /// Blocks the guest from seeing new NPad button/stick state (used while a
    /// suyu-side overlay like the Nextendo Account dialog is reading the same
    /// physical controller during gameplay, so it doesn't also drive the game
    /// underneath it) without touching input consumers that read the
    /// controller directly, like ControllerNavigation or hotkeys.
    void SetGuestInputSuspended(bool suspended) {
        guest_input_suspended.store(suspended, std::memory_order_relaxed);
    }
    bool IsGuestInputSuspended() const {
        return guest_input_suspended.load(std::memory_order_relaxed);
    }

    /// Number of emulated controllers
    static constexpr std::size_t available_controllers{10};

    std::unique_ptr<EmulatedController> player_1;
    std::unique_ptr<EmulatedController> player_2;
    std::unique_ptr<EmulatedController> player_3;
    std::unique_ptr<EmulatedController> player_4;
    std::unique_ptr<EmulatedController> player_5;
    std::unique_ptr<EmulatedController> player_6;
    std::unique_ptr<EmulatedController> player_7;
    std::unique_ptr<EmulatedController> player_8;
    std::unique_ptr<EmulatedController> other;
    std::unique_ptr<EmulatedController> handheld;
    std::unique_ptr<EmulatedConsole> console;
    std::unique_ptr<EmulatedDevices> devices;
    Kernel::KernelCore& kernel;
    NpadStyleTag supported_style_tag{NpadStyleSet::All};
    NpadIdType last_active_controller{NpadIdType::Handheld};
    std::atomic<bool> guest_input_suspended{false};
};

} // namespace Core::HID
