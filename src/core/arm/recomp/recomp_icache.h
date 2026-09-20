// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace suyu::recomp {

class RecompICache {
public:
    struct Range {
        std::uint64_t start;
        std::uint64_t end; // half-open; max means the range reaches the end
    };

    void Clear() {
        aot_rejected_.store(true, std::memory_order_release);
    }

    bool AllowsAot() const {
        return !aot_rejected_.load(std::memory_order_acquire);
    }

    // Direct calls emitted by the static pass bypass the dispatcher. Once any
    // range has been invalidated, disable those chains globally; each target
    // then re-enters LookupAot and observes the immutable range snapshot.
    bool AllowsAotChaining() const {
        return AllowsAot() && !has_invalidated_ranges_.load(std::memory_order_acquire);
    }

    // Invalidate the page-conservative AOT ranges intersecting the changed
    // bytes. This keeps unrelated pages usable after a self-modifying page or
    // debugger patch instead of forcing the entire image to JIT.
    void InvalidateRange(std::uint64_t start, std::size_t length) {
        if (length == 0) {
            return;
        }
        constexpr std::uint64_t page_size = 0x1000;
        const std::uint64_t original_start = start;
        const std::uint64_t original_end =
            length > std::numeric_limits<std::uint64_t>::max() - original_start
                ? std::numeric_limits<std::uint64_t>::max()
                : original_start + static_cast<std::uint64_t>(length);
        start = original_start & ~(page_size - 1);
        // Generated blocks are split at page boundaries, but legacy/manual
        // images may still contain a block that crosses into the modified
        // page. Without block extent metadata for those images, conservatively
        // reject the preceding page too so stale cross-page AOT cannot survive.
        if (start >= page_size) {
            start -= page_size;
        }
        const std::uint64_t raw_end = original_end;
        const std::uint64_t end =
            raw_end > std::numeric_limits<std::uint64_t>::max() - (page_size - 1)
                ? std::numeric_limits<std::uint64_t>::max()
                : (raw_end + page_size - 1) & ~(page_size - 1);
        // Readers remain lock-free through the immutable shared_ptr snapshot;
        // serialize copy/merge/publish so concurrent invalidations cannot
        // overwrite one another's ranges.
        std::scoped_lock writer_lock{ranges_write_lock_};
        std::uint64_t merged_start = start;
        std::uint64_t merged_end = end;
        const auto current = std::atomic_load_explicit(&invalidated_ranges_, std::memory_order_acquire);
        std::vector<Range> kept;
        kept.reserve(current->size() + 1);
        for (const Range range : *current) {
            if (range.end < merged_start || merged_end < range.start) {
                kept.push_back(range);
                continue;
            }
            merged_start = std::min(merged_start, range.start);
            merged_end = std::max(merged_end, range.end);
        }
        kept.push_back({merged_start, merged_end});
        std::sort(kept.begin(), kept.end(),
                  [](const Range a, const Range b) { return a.start < b.start; });
        std::atomic_store_explicit(&invalidated_ranges_,
                                   std::make_shared<const std::vector<Range>>(std::move(kept)),
                                   std::memory_order_release);
        has_invalidated_ranges_.store(true, std::memory_order_release);
    }

    bool AllowsAotAt(std::uint64_t pc) const {
        if (!AllowsAot()) {
            return false;
        }
        if (!has_invalidated_ranges_.load(std::memory_order_acquire)) {
            return true;
        }
        const auto ranges = std::atomic_load_explicit(&invalidated_ranges_, std::memory_order_acquire);
        const auto it = std::upper_bound(
            ranges->begin(), ranges->end(), pc,
            [](std::uint64_t value, const Range range) { return value < range.start; });
        return it == ranges->begin() || std::prev(it)->end <= pc;
    }

    std::size_t InvalidatedRangeCount() const {
        const auto ranges = std::atomic_load_explicit(&invalidated_ranges_, std::memory_order_acquire);
        return ranges->size();
    }

private:
    std::atomic<bool> aot_rejected_{false};
    std::atomic<bool> has_invalidated_ranges_{false};
    mutable std::mutex ranges_write_lock_;
    std::shared_ptr<const std::vector<Range>> invalidated_ranges_ =
        std::make_shared<const std::vector<Range>>();
};

}
