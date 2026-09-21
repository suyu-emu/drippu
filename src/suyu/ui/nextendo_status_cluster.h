// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QTimer>
#include <QWidget>

class QPaintEvent;

class NextendoStatusCluster : public QWidget {
    Q_OBJECT

public:
    explicit NextendoStatusCluster(QWidget* parent = nullptr);

    void SetScale(qreal scale);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    enum class NetKind { Offline, Ethernet, Wireless };

    void Tick();
    void RefreshNetwork();

    QTimer timer;
    NetKind net_kind = NetKind::Offline;
    int net_refresh_counter = 0;
    qreal m_scale = 1.0;
};
