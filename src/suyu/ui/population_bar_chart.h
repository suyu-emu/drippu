// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QWidget>

#include "suyu/nextendo_population_history.h"

class PopulationBarChart : public QWidget {
    Q_OBJECT

public:
    explicit PopulationBarChart(QWidget* parent = nullptr);

    void SetData(const Nextendo::PopulationHistory::DayProfile& profile, int live_count,
                const QColor& primary, const QColor& secondary);
    void Clear();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct PlotMetrics {
        QRectF plot_rect;
        double max_val = 1.0;
        qreal bar_w = 0.0;
        qreal gap = 2.0;
    };
    PlotMetrics ComputeMetrics() const;
    int BarAt(const QPoint& pos) const;

    Nextendo::PopulationHistory::DayProfile m_profile{};
    int m_live_count = 0;
    QColor m_primary;
    QColor m_secondary;
    bool m_has_data = false;
    int m_hover_hour = -1;
};
