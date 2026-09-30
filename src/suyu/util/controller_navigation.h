// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QKeyEvent>
#include <QObject>
#include <QTimer>

#include "common/input.h"
#include "common/settings_input.h"

namespace Core::HID {
using ButtonValues = std::array<Common::Input::ButtonStatus, Settings::NativeButton::NumButtons>;
using SticksValues = std::array<Common::Input::StickStatus, Settings::NativeAnalog::NumAnalogs>;
enum class ControllerTriggerType;
class EmulatedController;
class HIDCore;
} // namespace Core::HID

class ControllerNavigation : public QObject {
    Q_OBJECT

public:
    explicit ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent = nullptr);
    ~ControllerNavigation();

    /// Disables events from the emulated controller
    void UnloadController();

    enum class FocusTarget {
        MainView,
        DetailsView,
    };

    /// Switches focus between main list and details
    void toggleFocus();
    void setFocus(FocusTarget target);
    FocusTarget currentFocus() const {
        return m_current_focus;
    }

signals:
    void TriggerKeyboardEvent(Qt::Key key); // Kept for existing consumers (game list, applets)
    void navigated(int dx, int dy);
    void activated(); // Controller 'A'
    void cancelled(); // Controller 'B'
    void focusChanged(FocusTarget new_focus);
    void auxiliaryAction(int action_id); // For mapping X, Y, etc.
    void activityDetected();             // Emitted on any controller input
    void leftShoulderPressed();          // L / ZL, in addition to cancelled()
    void rightShoulderPressed();         // R / ZR, in addition to toggleFocus()
    void backPressed();                  // B specifically (not L/ZL), in addition to cancelled()

private slots:
    void navigationRepeat();

private:
    void LoadController(Core::HID::HIDCore& hid_core);
    // True when the owning window (or one of its children, e.g. a Friends-list
    // child dialog) is the active window. Navigation signals are gated on this
    // so a controller press doesn't drive a background window.
    bool IsOwnerWindowActive() const;
    void startRepeatTimer(int dx, int dy);
    void stopRepeatTimer();
    void TriggerButton(Settings::NativeButton::Values native_button, Qt::Key key);
    void ControllerUpdateEvent(Core::HID::ControllerTriggerType type);

    void ControllerUpdateButton();

    void ControllerUpdateStick();

    FocusTarget m_current_focus{FocusTarget::MainView};
    QTimer* m_repeat_timer{};
    int m_repeat_dx{};
    int m_repeat_dy{};

    Core::HID::ButtonValues button_values{};
    Core::HID::SticksValues stick_values{};

    int player1_callback_key{};
    int handheld_callback_key{};
    bool is_controller_set{};
    mutable std::mutex mutex;
    Core::HID::EmulatedController* player1_controller;
    Core::HID::EmulatedController* handheld_controller;
};
