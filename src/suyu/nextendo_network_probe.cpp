// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/nextendo_network_probe.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <thread>

#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#include "common/settings.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace {
constexpr int kNatProbeTimeoutMs = 3000;
constexpr quint32 kNatTestId = 101;
constexpr std::array<quint16, 2> kNatPorts{10025, 10125};

// Same fallback chain sfdnsres.cpp uses for the in-game DNS redirect: the two nncs responders
// share the account/game server's IP by default, with their own env var to split them off.
QString GetConfiguredHost(const std::string& setting, const char* env_var) {
    if (!setting.empty()) {
        return QString::fromStdString(setting);
    }
    if (const char* env = std::getenv(env_var); env && *env) {
        return QString::fromLatin1(env);
    }
    return QStringLiteral("127.0.0.1");
}
} // namespace

NextendoNetworkProbe::NextendoNetworkProbe(QObject* parent) : QObject(parent) {}

NextendoNetworkProbe::~NextendoNetworkProbe() = default;

void NextendoNetworkProbe::ProbeNat() {
    nat_resolved = false;
    emit NatStatusChanged(NatStatus::Checking);

    delete nat_socket;
    nat_socket = new QUdpSocket(this);
    nat_socket->bind(QHostAddress::AnyIPv4, 0);
    connect(nat_socket, &QUdpSocket::readyRead, this, &NextendoNetworkProbe::OnNatReadyRead);

    const QHostAddress host1(
        GetConfiguredHost(Settings::values.nextendo_server_ip.GetValue(), "NEXTENDO_SERVER_IP"));
    const QHostAddress host2(
        GetConfiguredHost(Settings::values.nextendo_nat_ip.GetValue(), "NEXTENDO_NAT_IP"));

    nat_targets.clear();
    nat_external_ports.clear();
    for (const QHostAddress& host : {host1, host2}) {
        for (quint16 port : kNatPorts) {
            nat_targets.push_back(NatTarget{host, port, false});
        }
    }

    QByteArray probe(16, '\0');
    probe[3] = static_cast<char>(kNatTestId); // big-endian test id at offset 0
    for (const NatTarget& target : nat_targets) {
        nat_socket->writeDatagram(probe, target.host, target.port);
    }

    delete nat_timeout;
    nat_timeout = new QTimer(this);
    nat_timeout->setSingleShot(true);
    connect(nat_timeout, &QTimer::timeout, this, [this] {
        const auto answered = std::count_if(nat_targets.begin(), nat_targets.end(),
                                            [](const NatTarget& t) { return t.answered; });
        // At least two distinct destinations have to answer to compare mapping at all.
        if (answered < 2) {
            Finish(NatStatus::Unknown);
        } else {
            Finish(nat_external_ports.size() == 1 ? NatStatus::Open : NatStatus::Strict);
        }
    });
    nat_timeout->start(kNatProbeTimeoutMs);
}

void NextendoNetworkProbe::OnNatReadyRead() {
    while (nat_socket && nat_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = nat_socket->receiveDatagram();
        if (nat_resolved || !datagram.isValid()) {
            continue;
        }
        const QByteArray data = datagram.data();
        if (data.size() < 16) {
            continue;
        }
        const auto read_u32_be = [&data](int offset) {
            return (static_cast<quint32>(static_cast<quint8>(data[offset])) << 24) |
                  (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 16) |
                  (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 8) |
                  static_cast<quint32>(static_cast<quint8>(data[offset + 3]));
        };
        if (read_u32_be(0) != kNatTestId) {
            continue;
        }

        // Only a reply from a target we actually probed counts, and only once per target --
        // otherwise a stray or duplicate datagram could skew the mapping comparison.
        for (NatTarget& target : nat_targets) {
            if (target.answered || target.host != datagram.senderAddress() ||
                target.port != datagram.senderPort()) {
                continue;
            }
            target.answered = true;
            nat_external_ports.insert(read_u32_be(4));
            break;
        }
    }
}

void NextendoNetworkProbe::Finish(NatStatus status) {
    if (nat_resolved) {
        return;
    }
    nat_resolved = true;
    if (nat_timeout) {
        nat_timeout->stop();
    }
    if (nat_socket) {
        nat_socket->deleteLater();
        nat_socket = nullptr;
    }
    emit NatStatusChanged(status);
}

void NextendoNetworkProbe::PingBackend() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this] {
        const auto ms = WebService::NextendoApi::PingBackend();
        QMetaObject::invokeMethod(
            this, [this, ms] { emit PingResult(ms.value_or(-1)); }, Qt::QueuedConnection);
    }}.detach();
#else
    emit PingResult(-1);
#endif
}
