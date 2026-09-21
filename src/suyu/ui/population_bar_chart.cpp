// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/ui/population_bar_chart.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include "suyu/theme.h"
#include "suyu/uisettings.h"

namespace {

constexpr qreal kLeftAxisW = 34.0;
constexpr qreal kBottomAxisH = 18.0;
constexpr qreal kTopMargin = 4.0;
constexpr qreal kRightMargin = 6.0;

double NiceAxisMax(double raw_max) {
    if (raw_max <= 0.0) {
        return 10.0;
    }
    const double target = raw_max * 1.2;
    const double magnitude = std::pow(10.0, std::floor(std::log10(target)));
    const double normalized = target / magnitude;
    double nice;
    if (normalized <= 1.0) {
        nice = 1.0;
    } else if (normalized <= 2.0) {
        nice = 2.0;
    } else if (normalized <= 5.0) {
        nice = 5.0;
    } else {
        nice = 10.0;
    }
    return nice * magnitude;
}

QString HourLabel(int hour) {
    const int h12 = hour % 12 == 0 ? 12 : hour % 12;
    return QStringLiteral("%1 %2").arg(h12).arg(hour < 12 ? QStringLiteral("AM") : QStringLiteral("PM"));
}

} // namespace

PopulationBarChart::PopulationBarChart(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
}

void PopulationBarChart::SetData(const Nextendo::PopulationHistory::DayProfile& profile,
                                 int live_count, const QColor& primary, const QColor& secondary) {
    m_profile = profile;
    m_live_count = live_count;
    m_primary = primary;
    m_secondary = secondary;
    m_has_data = true;
    update();
}

void PopulationBarChart::Clear() {
    m_has_data = false;
    update();
}

QSize PopulationBarChart::sizeHint() const {
    return QSize(200, 46);
}

PopulationBarChart::PlotMetrics PopulationBarChart::ComputeMetrics() const {
    PlotMetrics m;
    m.plot_rect = QRectF(kLeftAxisW, kTopMargin, width() - kLeftAxisW - kRightMargin,
                         height() - kTopMargin - kBottomAxisH);

    double raw_max = static_cast<double>(m_live_count);
    for (const auto& bucket : m_profile) {
        raw_max = std::max(raw_max, bucket.avg);
    }
    m.max_val = NiceAxisMax(raw_max);

    const int n = static_cast<int>(m_profile.size());
    m.bar_w = (m.plot_rect.width() - m.gap * (n - 1)) / n;
    return m;
}

int PopulationBarChart::BarAt(const QPoint& pos) const {
    const auto m = ComputeMetrics();
    if (!m.plot_rect.contains(pos)) {
        return -1;
    }
    const qreal x = pos.x() - m.plot_rect.left();
    const int hour = static_cast<int>(x / (m.bar_w + m.gap));
    return (hour >= 0 && hour < static_cast<int>(m_profile.size())) ? hour : -1;
}

void PopulationBarChart::mouseMoveEvent(QMouseEvent* event) {
    const int hour = m_has_data ? BarAt(event->pos()) : -1;
    if (hour != m_hover_hour) {
        m_hover_hour = hour;
        update();
    }
    if (hour >= 0) {
        const bool is_now = hour == QDateTime::currentDateTime().time().hour();
        const double value = is_now ? std::max(m_profile[hour].avg, static_cast<double>(m_live_count))
                                    : m_profile[hour].avg;
        QToolTip::showText(event->globalPosition().toPoint(),
                           tr("%1: %2 players%3")
                               .arg(HourLabel(hour))
                               .arg(static_cast<int>(std::lround(value)))
                               .arg(is_now ? tr(" (live)") : QString{}),
                           this);
    } else {
        QToolTip::hideText();
    }
}

void PopulationBarChart::leaveEvent(QEvent*) {
    if (m_hover_hour != -1) {
        m_hover_hour = -1;
        update();
    }
    QToolTip::hideText();
}

void PopulationBarChart::paintEvent(QPaintEvent*) {
    if (!m_has_data) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const bool is_dark = Theme::IsDarkMode();
    const QColor track = is_dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 22);
    const QColor grid = is_dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 35);
    const QColor text_color = is_dark ? QColor(200, 200, 205) : QColor(70, 70, 75);
    const QColor hover_color = is_dark ? QColor(255, 255, 255, 60) : QColor(0, 0, 0, 50);

    const auto metrics = ComputeMetrics();
    const QRectF& plot = metrics.plot_rect;
    const int n = static_cast<int>(m_profile.size());
    const int local_hour = QDateTime::currentDateTime().time().hour();

    QFont axis_font = p.font();
    axis_font.setPointSizeF(std::max(7.0, axis_font.pointSizeF() - 2.0));
    p.setFont(axis_font);
    const QFontMetrics fm(axis_font);

    constexpr int kGridLines = 4;
    for (int i = 0; i <= kGridLines; ++i) {
        const double value = metrics.max_val * i / kGridLines;
        const qreal y = plot.bottom() - (value / metrics.max_val) * plot.height();
        p.setPen(grid);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(text_color);
        const QString label = QString::number(static_cast<int>(std::lround(value)));
        p.drawText(QRectF(0, y - fm.height() / 2.0, kLeftAxisW - 4, fm.height()),
                  Qt::AlignRight | Qt::AlignVCenter, label);
    }

    for (int i = 0; i < n; ++i) {
        const qreal x = plot.left() + i * (metrics.bar_w + metrics.gap);
        const bool is_now = i == local_hour;
        const double value = is_now ? std::max(m_profile[i].avg, static_cast<double>(m_live_count))
                                    : m_profile[i].avg;
        const qreal h = std::clamp(value / metrics.max_val, 0.0, 1.0) * plot.height();

        p.fillRect(QRectF(x, plot.top(), metrics.bar_w, plot.height()), track);
        if (h > 0.0) {
            QColor color = is_now ? m_primary : m_secondary;
            color.setAlphaF(is_now ? 1.0f : 0.55f);
            const QRectF bar_rect(x, plot.bottom() - h, metrics.bar_w, h);
            p.fillRect(bar_rect, color);
            p.setPen(QPen(color.darker(140), 1.0));
            p.drawRect(bar_rect);
        }
        if (i == m_hover_hour) {
            p.fillRect(QRectF(x, plot.top(), metrics.bar_w, plot.height()), hover_color);
        }

        if (i % 4 == 0) {
            p.setPen(text_color);
            const QRectF label_rect(x - metrics.bar_w, plot.bottom() + 2,
                                    metrics.bar_w * 3 + metrics.gap * 2, kBottomAxisH - 2);
            p.drawText(label_rect, Qt::AlignHCenter | Qt::AlignTop, HourLabel(i));
        }
    }
}
