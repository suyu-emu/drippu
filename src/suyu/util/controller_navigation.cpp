// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings_input.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "suyu/util/controller_navigation.h"

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>

ControllerNavigation::ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent)
    : QObject(parent) {
    m_repeat_timer = new QTimer(this);
    connect(m_repeat_timer, &QTimer::timeout, this, &ControllerNavigation::navigationRepeat);
    LoadController(hid_core);
}

ControllerNavigation::~ControllerNavigation() {
    UnloadController();
}

bool ControllerNavigation::IsOwnerWindowActive() const {
    const auto* owner = qobject_cast<const QWidget*>(parent());
    if (!owner) {
        return true;
    }
    const QWidget* window = owner->window();
    if (!window || window->isActiveWindow()) {
        return true;
    }
    const QWidget* active = QApplication::activeWindow();
    return active && window->isAncestorOf(active);
}

void ControllerNavigation::LoadController(Core::HID::HIDCore& hid_core) {
    std::scoped_lock lock{mutex};
    if (is_controller_set) {
        return;
    }

    player1_callback_key = -1;
    handheld_callback_key = -1;

    player1_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    handheld_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);

    Core::HID::ControllerUpdateCallback engine_callback{
        .on_change = [this](Core::HID::ControllerTriggerType type) { ControllerUpdateEvent(type); },
        .is_npad_service = false,
    };

    if (player1_controller) {
        player1_callback_key = player1_controller->SetCallback(engine_callback);
    }
    if (handheld_controller) {
        handheld_callback_key = handheld_controller->SetCallback(engine_callback);
    }

    is_controller_set = true;
}

void ControllerNavigation::UnloadController() {
    if (QCoreApplication::closingDown()) {
        is_controller_set = false;
        player1_controller = nullptr;
        handheld_controller = nullptr;
        return;
    }

    if (is_controller_set) {
        if (player1_controller && player1_callback_key >= 0) {
            player1_controller->DeleteCallback(player1_callback_key);
            player1_callback_key = -1;
        }
        if (handheld_controller && handheld_callback_key >= 0) {
            handheld_controller->DeleteCallback(handheld_callback_key);
            handheld_callback_key = -1;
        }
        is_controller_set = false;
    }
}

void ControllerNavigation::toggleFocus() {
    m_current_focus =
        (m_current_focus == FocusTarget::MainView) ? FocusTarget::DetailsView : FocusTarget::MainView;
    emit focusChanged(m_current_focus);
}

void ControllerNavigation::setFocus(FocusTarget target) {
    if (m_current_focus != target) {
        m_current_focus = target;
        emit focusChanged(m_current_focus);
    }
}

void ControllerNavigation::startRepeatTimer(int dx, int dy) {
    m_repeat_dx = dx;
    m_repeat_dy = dy;
    m_repeat_timer->start(Settings::values.navigation_repeat_delay.GetValue());
}

void ControllerNavigation::stopRepeatTimer() {
    m_repeat_timer->stop();
    m_repeat_dx = 0;
    m_repeat_dy = 0;
}

void ControllerNavigation::navigationRepeat() {
    if (m_repeat_dx != 0 || m_repeat_dy != 0) {
        if (IsOwnerWindowActive()) {
            emit navigated(m_repeat_dx, m_repeat_dy);
        }
        m_repeat_timer->start(Settings::values.navigation_repeat_interval.GetValue());
    }
}

void ControllerNavigation::TriggerButton(Settings::NativeButton::Values native_button,
                                         Qt::Key key) {
    if (button_values[native_button].value && !button_values[native_button].locked) {
        emit TriggerKeyboardEvent(key);
    }
}

void ControllerNavigation::ControllerUpdateEvent(Core::HID::ControllerTriggerType type) {
    std::scoped_lock lock{mutex};
    if (!Settings::values.controller_navigation || !is_controller_set) {
        return;
    }

    emit activityDetected();

    if (type == Core::HID::ControllerTriggerType::Button) {
        ControllerUpdateButton();
    } else if (type == Core::HID::ControllerTriggerType::Stick) {
        ControllerUpdateStick();
    } else if (type == Core::HID::ControllerTriggerType::Connected ||
               type == Core::HID::ControllerTriggerType::Disconnected) {
        // Reset state to avoid stuck buttons on hotplug
        button_values.fill({});
        stick_values.fill({});
        stopRepeatTimer();
    }
}

void ControllerNavigation::ControllerUpdateButton() {
    if (!player1_controller || !handheld_controller) {
        return;
    }

    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_buttons = player1_controller->GetButtonsValues();
    const auto& handheld_buttons = handheld_controller->GetButtonsValues();
    const bool window_active = IsOwnerWindowActive();

    for (std::size_t i = 0; i < player1_buttons.size(); ++i) {
        const bool button = player1_buttons[i].value || handheld_buttons[i].value;
        const bool pressed = button && !button_values[i].value;
        const bool released = !button && button_values[i].value;
        // Trigger only once
        button_values[i].locked = button == button_values[i].value;
        button_values[i].value = button;

        // Navigation signals for overlay-style consumers (e.g. the Nextendo
        // Account dialog). Additive: the keyboard mapping below is untouched,
        // so existing TriggerKeyboardEvent consumers keep working.
        if (pressed && window_active) {
            switch (i) {
            case Settings::NativeButton::A: // Nintendo A / PS Circle (East)
                emit activated();
                break;
            case Settings::NativeButton::B: // Nintendo B / PS Cross (South)
                emit cancelled();
                emit backPressed();
                break;
            case Settings::NativeButton::L:  // L1
            case Settings::NativeButton::ZL: // L2
                emit cancelled();
                emit leftShoulderPressed();
                break;
            case Settings::NativeButton::DDown:
                emit navigated(0, 1);
                startRepeatTimer(0, 1);
                break;
            case Settings::NativeButton::DUp:
                emit navigated(0, -1);
                startRepeatTimer(0, -1);
                break;
            case Settings::NativeButton::DLeft:
                emit navigated(-1, 0);
                startRepeatTimer(-1, 0);
                break;
            case Settings::NativeButton::DRight:
                emit navigated(1, 0);
                startRepeatTimer(1, 0);
                break;
            case Settings::NativeButton::R:  // R1
            case Settings::NativeButton::ZR: // R2
                toggleFocus();
                emit rightShoulderPressed();
                break;
            case Settings::NativeButton::Plus:  // Options
            case Settings::NativeButton::Minus: // Select
                toggleFocus();
                break;
            case Settings::NativeButton::X:
                emit auxiliaryAction(0); // Cycle alphabetical sections
                break;
            default:
                break;
            }
        } else if (released && window_active) {
            switch (i) {
            case Settings::NativeButton::DDown:
            case Settings::NativeButton::DUp:
            case Settings::NativeButton::DLeft:
            case Settings::NativeButton::DRight:
                stopRepeatTimer();
                break;
            default:
                break;
            }
        }
    }

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        TriggerButton(Settings::NativeButton::A, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::B, Qt::Key_Escape);
        TriggerButton(Settings::NativeButton::DDown, Qt::Key_Down);
        TriggerButton(Settings::NativeButton::DLeft, Qt::Key_Left);
        TriggerButton(Settings::NativeButton::DRight, Qt::Key_Right);
        TriggerButton(Settings::NativeButton::DUp, Qt::Key_Up);
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        TriggerButton(Settings::NativeButton::DDown, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::DLeft, Qt::Key_Escape);
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        TriggerButton(Settings::NativeButton::X, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::A, Qt::Key_Escape);
        break;
    default:
        break;
    }
}

void ControllerNavigation::ControllerUpdateStick() {
    if (!player1_controller || !handheld_controller) {
        return;
    }

    // Navigation signals from stick deflection, alongside the keyboard mapping
    // below. Uses the navigation deadzone so menu focus doesn't drift on a
    // loose stick.
    if (IsOwnerWindowActive()) {
        const float deadzone = Settings::values.navigation_deadzone.GetValue();
        const auto& p1_sticks = player1_controller->GetSticksValues();
        const auto& hh_sticks = handheld_controller->GetSticksValues();
        for (std::size_t i = 0; i < p1_sticks.size(); ++i) {
            const bool down =
                (p1_sticks[i].y.value < -deadzone) || (hh_sticks[i].y.value < -deadzone);
            const bool up =
                (p1_sticks[i].y.value > deadzone) || (hh_sticks[i].y.value > deadzone);
            const bool left =
                (p1_sticks[i].x.value < -deadzone) || (hh_sticks[i].x.value < -deadzone);
            const bool right =
                (p1_sticks[i].x.value > deadzone) || (hh_sticks[i].x.value > deadzone);
            if (down) {
                emit navigated(0, 1);
                startRepeatTimer(0, 1);
            } else if (up) {
                emit navigated(0, -1);
                startRepeatTimer(0, -1);
            } else if (left) {
                emit navigated(-1, 0);
                startRepeatTimer(-1, 0);
            } else if (right) {
                emit navigated(1, 0);
                startRepeatTimer(1, 0);
            }
        }
    }

    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_sticks = player1_controller->GetSticksValues();
    const auto& handheld_sticks = player1_controller->GetSticksValues();
    bool update = false;

    for (std::size_t i = 0; i < player1_sticks.size(); ++i) {
        const Common::Input::StickStatus stick{
            .left = player1_sticks[i].left || handheld_sticks[i].left,
            .right = player1_sticks[i].right || handheld_sticks[i].right,
            .up = player1_sticks[i].up || handheld_sticks[i].up,
            .down = player1_sticks[i].down || handheld_sticks[i].down,
        };
        // Trigger only once
        if (stick.down != stick_values[i].down || stick.left != stick_values[i].left ||
            stick.right != stick_values[i].right || stick.up != stick_values[i].up) {
            update = true;
        }
        stick_values[i] = stick;
    }

    if (!update) {
        return;
    }

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        if (stick_values[Settings::NativeAnalog::LStick].down) {
            emit TriggerKeyboardEvent(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].left) {
            emit TriggerKeyboardEvent(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].right) {
            emit TriggerKeyboardEvent(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].up) {
            emit TriggerKeyboardEvent(Qt::Key_Up);
            return;
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        if (stick_values[Settings::NativeAnalog::LStick].left) {
            emit TriggerKeyboardEvent(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].up) {
            emit TriggerKeyboardEvent(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].down) {
            emit TriggerKeyboardEvent(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].right) {
            emit TriggerKeyboardEvent(Qt::Key_Up);
            return;
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        if (stick_values[Settings::NativeAnalog::RStick].right) {
            emit TriggerKeyboardEvent(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].down) {
            emit TriggerKeyboardEvent(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].up) {
            emit TriggerKeyboardEvent(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].left) {
            emit TriggerKeyboardEvent(Qt::Key_Up);
            return;
        }
        break;
    default:
        break;
    }
}
