// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/nextendo_online_counts.h"

#include <map>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <QObject>
#include <QTimer>

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Nextendo::OnlineCounts {

namespace {

std::mutex g_mutex;
std::map<std::string, int> g_counts; // lowercase-hex title id -> player count

class Poller : public QObject {
public:
    using QObject::QObject;

    void Poll() {
#ifdef ENABLE_WEB_SERVICE
        std::thread{[] {
            auto counts = WebService::NextendoApi::GetOnlineCounts();
            std::scoped_lock lock{g_mutex};
            g_counts = std::move(counts);
        }}.detach();
#endif
    }
};

Poller*& PollerInstance() {
    static Poller* instance = nullptr;
    return instance;
}

} // Anonymous namespace

void Start(QObject* parent) {
    Poller*& instance = PollerInstance();
    if (instance) {
        return;
    }

    instance = new Poller(parent);
    auto* timer = new QTimer(instance);
    timer->setInterval(5000);
    QObject::connect(timer, &QTimer::timeout, instance, &Poller::Poll);
    timer->start();
    instance->Poll();
}

int For(u64 program_id) {
    const std::string key = fmt::format("{:016x}", program_id);
    std::scoped_lock lock{g_mutex};
    const auto it = g_counts.find(key);
    return it != g_counts.end() ? it->second : 0;
}

} // namespace Nextendo::OnlineCounts
