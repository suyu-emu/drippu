#!/bin/bash -ex

# SPDX-FileCopyrightText: 2023 yuzu Emulator Project
# SPDX-FileCopyrightText: 2024 suyu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Compatibility wrapper: the canonical Android build script lives in
# .ci/android/build.sh. This wrapper is kept so existing callers
# (.gitlab-ci.yml, .forgejo/workflows/*.yml) keep working.
# It builds the mainline flavor, matching this script's historic behavior.

CANONICAL_SCRIPT="$(cd "$(dirname "$0")/../../android" && pwd)/build.sh"

# Map this script's historic build-type values onto the canonical script's
# -b/--build-type values (Release, RelWithDebInfo, Debug).
BUILD_TYPE="Release"
if [ "${GITHUB_REPOSITORY}" = "suyu/suyu" ]; then
    BUILD_TYPE="RelWithDebInfo"
fi

exec "${CANONICAL_SCRIPT}" -t standard -b "${BUILD_TYPE}"
