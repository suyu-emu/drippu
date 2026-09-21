// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/ui/nextendo_status_cluster.h"

#include <algorithm>

#include <QDateTime>
#include <QFontMetrics>
#include <QNetworkInterface>
#include <QPainter>
#include <QPainterPath>

namespace {
void ShadowedText(QPainter& p, const QPointF& pos, Qt::Alignment align, const QString& text,
                  const QFont& font, const QColor& color) {
    p.setFont(font);
    const QFontMetrics fm(font);
    const QRectF box(pos.x() - (align & Qt::AlignRight ? fm.horizontalAdvance(text) : 0),
                     pos.y(), fm.horizontalAdvance(text) + 4, fm.height());
    p.setPen(QColor(0, 0, 0, 160));
    p.drawText(box.translated(1, 1.5), align | Qt::AlignTop, text);
    p.setPen(color);
    p.drawText(box, align | Qt::AlignTop, text);
}
}

NextendoStatusCluster::NextendoStatusCluster(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(&timer, &QTimer::timeout, this, &NextendoStatusCluster::Tick);
    timer.start(1000);
    RefreshNetwork();
    SetScale(1.0);
}

void NextendoStatusCluster::SetScale(qreal scale) {
    m_scale = std::clamp(scale, 1.0, 2.0);

    QFont time_font;
    time_font.setPointSizeF(time_font.pointSizeF() + 8 * m_scale);
    time_font.setBold(true);
    const QFontMetrics time_fm(time_font);
    const int time_w = time_fm.horizontalAdvance(QStringLiteral("88:88 PM"));

    QFont date_font;
    date_font.setPointSizeF(date_font.pointSizeF() + 1 * m_scale);
    const QFontMetrics date_fm(date_font);
    const int date_w = date_fm.horizontalAdvance(QStringLiteral("Sat, Aug 30"));

    const int icon_w = static_cast<int>(30 * m_scale);
    const int gap = static_cast<int>(12 * m_scale);
    setFixedSize(date_w + gap + time_w + gap + icon_w, static_cast<int>(34 * m_scale));
    update();
}

void NextendoStatusCluster::RefreshNetwork() {
    net_kind = NetKind::Offline;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        if (iface.addressEntries().isEmpty()) {
            continue;
        }
        if (iface.type() == QNetworkInterface::Wifi) {
            net_kind = NetKind::Wireless;
            break;
        }
        if (iface.type() == QNetworkInterface::Ethernet) {
            net_kind = NetKind::Ethernet;
            break;
        }
    }
}

void NextendoStatusCluster::Tick() {
    if (++net_refresh_counter >= 5) {
        net_refresh_counter = 0;
        RefreshNetwork();
    }
    update();
}

void NextendoStatusCluster::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const int icon_w = static_cast<int>(30 * m_scale);
    const qreal icon_cx = width() - icon_w / 2.0;
    const qreal row_y = height() / 2.0;

    QFont time_font;
    time_font.setPointSizeF(time_font.pointSizeF() + 8 * m_scale);
    time_font.setBold(true);
    const QFontMetrics time_fm(time_font);
    const QString time_str = QDateTime::currentDateTime().toString(QStringLiteral("h:mm AP"));
    const qreal time_right = width() - icon_w - 12 * m_scale;
    ShadowedText(p, QPointF(time_right, row_y - time_fm.height() / 2.0), Qt::AlignRight, time_str,
                time_font, Qt::white);

    QFont date_font;
    date_font.setPointSizeF(date_font.pointSizeF() + 1 * m_scale);
    const QFontMetrics date_fm(date_font);
    const QString date_str = QDateTime::currentDateTime().toString(QStringLiteral("ddd, MMM d"));
    const qreal date_right = time_right - time_fm.horizontalAdvance(time_str) - 10 * m_scale;
    ShadowedText(p, QPointF(date_right, row_y - date_fm.height() / 2.0), Qt::AlignRight, date_str,
                date_font, QColor(255, 255, 255, 210));

    QColor net_col;
    switch (net_kind) {
    case NetKind::Ethernet: net_col = QColor(120, 210, 255); break;
    case NetKind::Wireless: net_col = QColor(140, 230, 170); break;
    default: net_col = QColor(255, 255, 255, 110); break;
    }

    p.setPen(Qt::NoPen);
    p.setBrush(net_col);
    if (net_kind == NetKind::Offline) {
        p.setPen(QPen(net_col, 1.6 * m_scale));
        p.drawLine(QPointF(icon_cx - 6 * m_scale, row_y - 6 * m_scale),
                  QPointF(icon_cx + 6 * m_scale, row_y + 6 * m_scale));
    }
    for (int i = 0; i < 3; ++i) {
        const qreal bar_h = (i + 1) * 4.5 * m_scale;
        const qreal bar_w = 4.0 * m_scale;
        const qreal x = icon_cx - 9 * m_scale + i * (bar_w + 2.5 * m_scale);
        QRectF bar(x, row_y + 8 * m_scale - bar_h, bar_w, bar_h);
        p.drawRoundedRect(bar, bar_w * 0.3, bar_w * 0.3);
    }
}
