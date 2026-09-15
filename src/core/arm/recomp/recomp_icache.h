// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace suyu::recomp {

class RecompICache {
public:
    void Clear() {
        aot_rejected_.store(true, std::memory_order_release);
    }

    bool AllowsAot() const {
        return !aot_rejected_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> aot_rejected_{false};
};

}
