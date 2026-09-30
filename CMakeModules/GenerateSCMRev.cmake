# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# generate git/build information
include(GetSCMRev)

function(get_timestamp _var)
    string(TIMESTAMP timestamp UTC)
    set(${_var} "${timestamp}" PARENT_SCOPE)
endfunction()

get_timestamp(BUILD_DATE)

if (DEFINED GIT_RELEASE)
    set(BUILD_VERSION "${GIT_TAG}")
    set(GIT_REFSPEC "${GIT_RELEASE}")
    set(IS_DEV_BUILD false)
else()
    string(SUBSTRING ${GIT_COMMIT} 0 10 BUILD_VERSION)
    set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
    set(IS_DEV_BUILD true)
endif()

if (NIGHTLY_BUILD)
    set(IS_NIGHTLY_BUILD true)
else()
    set(IS_NIGHTLY_BUILD false)
endif()

set(GIT_DESC ${BUILD_VERSION})

# Generate cpp with Git revision from template

# Default update feed: the project's own GitHub releases. These are only the
# compiled-in defaults -- Settings::values.update_* (see common/settings.h) let
# the user point the updater at a fork or a new home if drippu ever moves, and
# update_extra_sources adds further repos to check alongside this one.
#
# Layout notes for the values below, matching how Common::Net uses them:
# - API is the bare host: requests go to https://<API><API_PATH>.
# - API_PATH is the "latest release" endpoint for the channel.
# - STABLE_API_PATH is the path segment GetReleasesBody() prefixes with "/" and
#   suffixes with "/<repo>/releases", i.e. the release-list endpoint.
# - WEBSITE carries its scheme: Release::FromJson falls back to
#   <WEBSITE>/<repo>/releases/tag/<tag> when the API omits html_url.
set(BUILD_AUTO_UPDATE_WEBSITE "https://github.com")
set(BUILD_AUTO_UPDATE_API "api.github.com")
set(BUILD_AUTO_UPDATE_API_PATH "/repos/suyu-emu/drippu/releases/latest")
set(BUILD_AUTO_UPDATE_REPO "suyu-emu/drippu")
set(BUILD_AUTO_UPDATE_STABLE_API "api.github.com")
set(BUILD_AUTO_UPDATE_STABLE_API_PATH "repos")
set(BUILD_AUTO_UPDATE_STABLE_REPO "suyu-emu/drippu")

if (NIGHTLY_BUILD)
    set(REPO_NAME "drippu Nightly")
else()
    set(REPO_NAME "drippu")
endif()

set(BUILD_ID ${GIT_REFSPEC})
set(BUILD_FULLNAME "${REPO_NAME} v0.04 (early access)")
set(CXX_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(scm_rev.cpp.in scm_rev.cpp @ONLY)
