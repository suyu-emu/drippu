// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>
#include <utility>

#include <QByteArray>

#include "suyu/nextendo_avatar_cache.h"

namespace Nextendo::AvatarCache {

namespace {

// Cached once at a fixed, generous size regardless of what a given caller asked for -- the
// header avatar (72px) and a game icon (48-64px) share the same "self" entry, so caching at
// whichever size asked first meant the other one got that size force-upscaled back out.
constexpr int kCacheSize = 256;

struct CacheEntry {
    std::string source_hash; // the base64 string itself; cheap enough and self-invalidating
    QPixmap pixmap;
};

std::unordered_map<std::string, CacheEntry>& Cache() {
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

} // Anonymous namespace

QPixmap Get(const std::string& key, const std::string& image_base64, int size) {
    if (image_base64.empty()) {
        return {};
    }

    auto& cache = Cache();
    if (const auto it = cache.find(key); it != cache.end() && it->second.source_hash == image_base64) {
        return it->second.pixmap;
    }

    const QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(image_base64));
    QPixmap pixmap;
    if (raw.isEmpty() || !pixmap.loadFromData(raw)) {
        cache.erase(key);
        return {};
    }

    const int target = qMax(size, kCacheSize);
    pixmap = pixmap.scaled(target, target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    cache[key] = CacheEntry{image_base64, pixmap};
    return pixmap;
}

void Clear() {
    Cache().clear();
}

} // namespace Nextendo::AvatarCache
