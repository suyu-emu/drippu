// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <QLabel>
#include <qnamespace.h>
#include <QCheckBox>
#include <QSlider>
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_graphics_extensions.h"
#include "suyu/configuration/configuration_shared.h"
#include "suyu/configuration/configure_graphics_extensions.h"
#include "suyu/configuration/shared_translation.h"
#include "suyu/configuration/shared_widget.h"

ConfigureGraphicsExtensions::ConfigureGraphicsExtensions(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphicsExtensions>()}, system{system_} {

    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();
}

ConfigureGraphicsExtensions::~ConfigureGraphicsExtensions() = default;

void ConfigureGraphicsExtensions::SetConfiguration() {}

void ConfigureGraphicsExtensions::Setup(const ConfigurationShared::Builder& builder) {
    auto& layout = *ui->populate_target->layout();
    std::map<u32, QWidget*> hold{}; // A map will sort the data for us

    // Populate from the renderer extensions category as well as renderer
    // advanced. The extensions category holds driver-facing toggles (e.g. the
    // graphics pipeline library switch) that users need to reach when a driver
    // misbehaves; leaving it out strands those settings with no UI at all.
    for (const auto category : {Settings::Category::RendererExtensions,
                                Settings::Category::RendererAdvanced}) {
        for (auto setting : Settings::values.linkage.by_category[category]) {
            ConfigurationShared::Widget* widget = builder.BuildWidget(setting, apply_funcs);

            if (widget == nullptr) {
                continue;
            }
            if (!widget->Valid()) {
                widget->deleteLater();
                continue;
            }

            hold.emplace(setting->Id(), widget);
        }
    }

    for (const auto& [id, widget] : hold) {
        layout.addWidget(widget);
    }
}

void ConfigureGraphicsExtensions::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureGraphicsExtensions::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphicsExtensions::RetranslateUI() {
    ui->retranslateUi(this);
}
