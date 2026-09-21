// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/nextendo_population_dialog.h"

#include <utility>
#include <vector>

#include <QButtonGroup>
#include <QColor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimeZone>
#include <QVBoxLayout>

#include "suyu/nextendo_online_counts.h"
#include "suyu/nextendo_population_history.h"
#include "suyu/theme.h"
#include "suyu/ui/population_bar_chart.h"
#include "suyu/uisettings.h"
#include "common/common_types.h"

namespace {

// Splatoon 2's 3 regional title IDs mirror the same server-side total; only count one.
constexpr u64 kMarioKart8 = 0x0100152000022000;
constexpr u64 kSuperSmashBros = 0x01006a800016e000;
constexpr u64 kSplatoon2 = 0x0100f8f0000a2000;
constexpr u64 kAnimalCrossing = 0x01006f8002326000;

std::vector<u64> TitlesForTab(int tab) {
    switch (tab) {
    case 1:
        return {kSplatoon2};
    case 2:
        return {kSuperSmashBros};
    case 3:
        return {kMarioKart8};
    default:
        return {kMarioKart8, kSuperSmashBros, kSplatoon2, kAnimalCrossing};
    }
}

std::pair<QColor, QColor> ColorsForTab(int tab) {
    switch (tab) {
    case 1:
        return {QColor(60, 200, 90), QColor(40, 140, 220)}; // Splatoon 2: green / blue
    case 2:
        return {QColor(255, 140, 0), QColor(120, 72, 40)}; // Super Smash Bros.: orange / brown
    case 3:
        return {QColor(220, 40, 40), QColor(240, 240, 240)}; // Mario Kart 8: red / white
    default: {
        const QString accent_hex =
            QString::fromStdString(UISettings::values.accent_color.GetValue());
        const QColor accent = QColor(accent_hex).isValid() ? QColor(accent_hex) : QColor(0, 150, 255);
        return {accent, accent};
    }
    }
}

} // namespace

NextendoPopulationDialog::NextendoPopulationDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Nextendo Population"));
    setFixedSize(561, 367);

    auto* layout = new QVBoxLayout(this);

    auto* description = new QLabel(
        tr("Average Nextendo online population by hour, sourced from a community-run scraper of "
           "the public online-count endpoint. Updated every hour (with live updates)."));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* tab_row = new QHBoxLayout;
    m_tabs = new QButtonGroup(this);
    m_tabs->setExclusive(true);
    const QStringList labels{tr("Total"), tr("Splatoon 2"), tr("Super Smash Bros. Ultimate"),
                             tr("Mario Kart 8 Deluxe")};
    for (int i = 0; i < labels.size(); ++i) {
        auto* btn = new QPushButton(labels[i]);
        btn->setCheckable(true);
        btn->setChecked(i == 0);
        btn->setFixedHeight(26);
        btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_tabs->addButton(btn, i);
        tab_row->addWidget(btn);
    }
    layout->addLayout(tab_row);

    m_live_label = new QLabel;
    QFont live_font = m_live_label->font();
    live_font.setBold(true);
    live_font.setPointSizeF(live_font.pointSizeF() + 1.0);
    m_live_label->setFont(live_font);
    layout->addWidget(m_live_label);

    m_chart = new PopulationBarChart(this);
    m_chart->setMinimumHeight(200);
    layout->addWidget(m_chart, 1);

    m_empty_label = new QLabel(tr("No population data yet -- it fills in as the scraper runs."));
    m_empty_label->setAlignment(Qt::AlignCenter);
    m_empty_label->hide();
    layout->addWidget(m_empty_label);

    m_updated_label = new QLabel;
    m_updated_label->setAlignment(Qt::AlignCenter);
    m_updated_label->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
    layout->addWidget(m_updated_label);

    connect(m_tabs, &QButtonGroup::idToggled, this, [this](int id, bool checked) {
        if (checked) {
            RefreshChart(id);
        }
    });

    RefreshChart(0);
}

void NextendoPopulationDialog::RefreshChart(int tab) {
    const auto title_ids = TitlesForTab(tab);

    Nextendo::PopulationHistory::DayProfile summed{};
    int live_total = 0;
    bool any_data = false;

    for (u64 id : title_ids) {
        live_total += Nextendo::OnlineCounts::For(id);
        const auto profile = Nextendo::PopulationHistory::For(id);
        if (!profile) {
            continue;
        }
        any_data = true;
        for (int h = 0; h < 24; ++h) {
            summed[h].avg += (*profile)[h].avg;
            summed[h].samples += (*profile)[h].samples;
        }
    }

    if (any_data) {
        const auto [primary, secondary] = ColorsForTab(tab);
        m_chart->SetData(summed, live_total, primary, secondary);
        m_chart->show();
        m_empty_label->hide();
    } else {
        m_chart->hide();
        m_empty_label->show();
    }

    m_live_label->setText(tr("\xF0\x9F\x8E\xAE Live: %1 players \xE2\x80\x94 %2")
                              .arg(live_total)
                              .arg(QLocale::system().toString(QDateTime::currentDateTime(),
                                                              QStringLiteral("dddd, MMM d, h:mm AP"))));

    const std::string updated = Nextendo::PopulationHistory::LastUpdatedUtc();
    QString updated_text;
    if (!updated.empty()) {
        auto dt = QDateTime::fromString(QString::fromStdString(updated), Qt::ISODate);
        if (dt.isValid()) {
            dt.setTimeZone(QTimeZone::UTC);
            const QDateTime local = dt.toLocalTime();
            const qint64 minutes_ago = std::max<qint64>(0, dt.secsTo(QDateTime::currentDateTimeUtc())) / 60;
            const QString when = QLocale::system().toString(local, QStringLiteral("MMM d, h:mm AP"));
            updated_text = minutes_ago <= 1
                              ? tr("Updated just now (%1)").arg(when)
                              : tr("Updated %1 minutes ago (%2)").arg(minutes_ago).arg(when);
        }
    }
    m_updated_label->setText(updated_text);
}
