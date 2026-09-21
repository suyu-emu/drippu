// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <QPixmap>

#include "common/common_types.h"

// Decodes base64 JPEG avatars into cached QPixmaps. UI thread only.
namespace Nextendo::AvatarCache {

// Null QPixmap if image_base64 is empty/undecodable; callers fall back to an initials avatar.
QPixmap Get(const std::string& key, const std::string& image_base64, int size);

void Clear();

} // namespace Nextendo::AvatarCache
