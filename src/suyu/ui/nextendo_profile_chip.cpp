// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/ui/nextendo_profile_chip.h"

#include <algorithm>
#include <thread>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "suyu/nextendo_avatar_cache.h"
#include "suyu/uisettings.h"
#include "common/nextendo_account.h"
#include "common/nextendo_avatar.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace {
constexpr int kDiameter = 52;
}

NextendoProfileChip::NextendoProfileChip(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(kDiameter, kDiameter);

    name_label = new QLabel(this);
    name_label->hide();

    refresh_timer.setInterval(15000);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoProfileChip::RefreshAvatar);
    refresh_timer.start();
    RefreshAvatar();
    SetScale(1.0);
}

void NextendoProfileChip::SetScale(qreal scale) {
    m_scale = std::clamp(scale, 1.0, 2.4);
    const int d = static_cast<int>(kDiameter * m_scale);
    setFixedSize(d, d);
    name_label->setStyleSheet(QStringLiteral("QLabel { color: white; font-weight: bold; font-size: %1px; }")
                                   .arg(static_cast<int>(15 * m_scale)));
    name_label->move(d + static_cast<int>(12 * m_scale), d / 2 - static_cast<int>(10 * m_scale));
    if (avatar.isNull() || avatar.width() != d) {
        last_avatar_key.clear();
        RefreshAvatar();
    }
    update();
}

void NextendoProfileChip::RefreshAvatar() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::NextendoAccount::IsLinked()) {
        avatar = QPixmap();
        update();
        return;
    }
    std::thread{[this] {
        auto profile = WebService::NextendoApi::GetProfile();
        if (!profile.ok) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, image = profile.image_base64] {
                Common::NextendoAvatar::SetSelfJPEGBase64(image);
                if (image == last_avatar_key) {
                    return;
                }
                last_avatar_key = image;
                avatar = Nextendo::AvatarCache::Get("self", image, width());
                update();
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoProfileChip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit Clicked();
    }
    QWidget::mousePressEvent(event);
}

void NextendoProfileChip::enterEvent(QEnterEvent*) {
    const bool linked = Common::NextendoAccount::IsLinked();
    name_label->setText(linked ? QString::fromStdString(Common::NextendoAccount::GetUsername())
                               : tr("Sign In to Nextendo"));
    name_label->adjustSize();
    name_label->show();
}

void NextendoProfileChip::leaveEvent(QEvent*) {
    name_label->hide();
}

void NextendoProfileChip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const bool linked = Common::NextendoAccount::IsLinked();
    const QString hex = QString::fromStdString(UISettings::values.accent_color.GetValue());
    const QColor accent = QColor(hex).isValid() ? QColor(hex) : QColor(0, 150, 255);

    const QRectF circle(1, 1, width() - 2, height() - 2);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(12, 12, 18, 220));
    p.drawEllipse(circle);

    if (!avatar.isNull()) {
        QPainterPath clip;
        clip.addEllipse(circle);
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(circle.toRect(), avatar);
        p.restore();
    } else {
        const QString name = linked ? QString::fromStdString(Common::NextendoAccount::GetUsername())
                                    : QString{};
        p.setPen(linked ? Qt::white : QColor(150, 150, 158));
        QFont f = font();
        const qreal size_scale = static_cast<qreal>(width()) / kDiameter;
        f.setPointSizeF(f.pointSizeF() + (linked ? 4 : 8) * size_scale);
        f.setBold(linked);
        p.setFont(f);
        p.drawText(circle, Qt::AlignCenter, linked && !name.isEmpty() ? name.left(1).toUpper() : QStringLiteral("+"));
    }

    p.setPen(QPen(accent, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(circle);
}
