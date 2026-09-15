// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

namespace suyu::recomp {

class RecompSession {
public:
    // Binds this session to `process`. Returns true when the bound process
    // changed, including the first bind.
    bool AttachProcess(const void* process) {
        std::lock_guard<std::mutex> lock{mu_};
        if (process_ == process) {
            ++owners_;
            return false;
        }
        process_ = process;
        owners_ = 1;
        ResetLocked();
        return true;
    }

    void DetachProcess(const void* process) {
        std::lock_guard<std::mutex> lock{mu_};
        if (process_ != process || owners_ == 0) {
            return;
        }
        if (--owners_ == 0) {
            process_ = nullptr;
            ResetLocked();
        }
    }

    template <typename Fn>
    void EnsureModuleBasesRegistered(Fn&& register_all) {
        std::lock_guard<std::mutex> lock{mu_};
        if (bases_registered_) {
            return;
        }
        std::forward<Fn>(register_all)();
        bases_registered_ = true;
    }

    void NoteStaticBlock() {
        static_blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t static_blocks() const {
        return static_blocks_.load(std::memory_order_relaxed);
    }

private:
    void ResetLocked() {
        bases_registered_ = false;
        static_blocks_.store(0, std::memory_order_relaxed);
    }

    std::mutex mu_;
    const void* process_{nullptr};
    unsigned owners_{0};
    bool bases_registered_{false};
    std::atomic<std::uint64_t> static_blocks_{0};
};

}
