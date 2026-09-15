// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QIcon>
#include <fmt/format.h>
#include "common/scm_rev.h"
#include "suyu/about_dialog.h"
#include "ui_aboutdialog.h"

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent), ui{std::make_unique<Ui::AboutDialog>()} {
    const auto build_fullname = std::string(Common::g_build_fullname);
    const auto branch_name = std::string(Common::g_scm_branch);
    const auto description = std::string(Common::g_scm_desc);
    const auto build_id = std::string(Common::g_build_id);

    const auto drippu_build = fmt::format("drippu Development Build | {}-{}", branch_name, description);
    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto drippu_build_version = !build_fullname.empty() ? build_fullname
                                                            : (override_build.empty() ? drippu_build : override_build);

    ui->setupUi(this);
    // Try and request the icon from Qt theme (Linux?)
    const QIcon drippu_logo = QIcon::fromTheme(QStringLiteral("drippu"));
    if (drippu_logo.isNull()) {
        const QIcon legacy_logo = QIcon::fromTheme(QStringLiteral("dev.suyu_emu.suyu"));
        if (!legacy_logo.isNull()) {
            ui->labelLogo->setPixmap(legacy_logo.pixmap(200));
        }
    } else {
        ui->labelLogo->setPixmap(drippu_logo.pixmap(200));
    }
    ui->labelBuildInfo->setText(
        ui->labelBuildInfo->text().arg(QString::fromStdString(drippu_build_version),
                                       QString::fromUtf8(Common::g_build_date).left(10)));
}

AboutDialog::~AboutDialog() = default;
