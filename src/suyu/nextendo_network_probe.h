// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <set>
#include <vector>

#include <QHostAddress>
#include <QObject>
#include <QString>

class QUdpSocket;
class QTimer;

// One-shot NAT-check probe (the Friends dialog's "NAT" indicator) and backend latency check (its
// "Ping" indicator). Both are fire-and-forget; results come back via signals.
//
// The NAT probe speaks the same nncs (Nintendo NAT-Check) UDP protocol the console itself uses,
// against the same nncs1/nncs2 responders games get DNS-redirected to (nextendo_server_ip /
// nextendo_nat_ip) -- rather than a UPnP IGD query, which only ever reports something for players
// whose router has UPnP enabled and says nothing about whether the NAT actually lets P2P through.
class NextendoNetworkProbe : public QObject {
    Q_OBJECT

public:
    enum class NatStatus { Checking, Open, Strict, Unknown };

    explicit NextendoNetworkProbe(QObject* parent = nullptr);
    ~NextendoNetworkProbe() override;

    void ProbeNat();
    void PingBackend();

signals:
    void NatStatusChanged(NextendoNetworkProbe::NatStatus status);
    void PingResult(int ms); // -1 on failure

private:
    struct NatTarget {
        QHostAddress host;
        quint16 port;
        bool answered = false;
    };

    void OnNatReadyRead();
    void Finish(NatStatus status);

    QUdpSocket* nat_socket = nullptr;
    QTimer* nat_timeout = nullptr;
    std::vector<NatTarget> nat_targets;
    std::set<quint32> nat_external_ports;
    bool nat_resolved = false;
};
