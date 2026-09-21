// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;

class NextendoProfileChip : public QWidget {
    Q_OBJECT

public:
    explicit NextendoProfileChip(QWidget* parent = nullptr);

    void SetScale(qreal scale);

signals:
    void Clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void RefreshAvatar();

    QTimer refresh_timer;
    QLabel* name_label = nullptr;
    QPixmap avatar;
    std::string last_avatar_key;
    qreal m_scale = 1.0;
};
