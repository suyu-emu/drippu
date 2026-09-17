#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

artifacts_dir="${ARTIFACTS_DIR:-artifacts}"
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
sha="${GITHUB_SHA:?GITHUB_SHA is required}"
short="${sha:0:12}"
date_utc="$(date -u +%Y%m%d)"
tag="v0.04-${date_utc}-${short}"
server="${GITHUB_SERVER_URL:-https://github.com}"
run_id="${GITHUB_RUN_ID:-}"

if [ ! -d "$artifacts_dir" ]; then
  echo "No artifacts directory at $artifacts_dir"
  exit 1
fi

mapfile -t files < <(find "$artifacts_dir" -type f \( -name '*.zip' -o -name '*.tar.gz' -o -name '*.apk' \) | sort)
if [ "${#files[@]}" -eq 0 ]; then
  echo "No artifacts to release"
  exit 1
fi

required=(
  drippu-windows-x64.zip
  drippu-linux-x64.tar.gz
  drippu-macos-arm64.tar.gz
  drippu-libretro-core-linux-x64.tar.gz
  drippu-libretro-core-windows-x64.zip
)

found_names=()
for f in "${files[@]}"; do
  found_names+=("$(basename "$f")")
done

missing=0
for name in "${required[@]}"; do
  if ! printf '%s\n' "${found_names[@]}" | grep -Fxq "$name"; then
    echo "missing required artifact: $name"
    missing=1
  fi
done
if ! printf '%s\n' "${found_names[@]}" | grep -Eq '\.apk$'; then
  echo "missing required artifact: at least one .apk"
  missing=1
fi
if [ "$missing" -ne 0 ]; then
  echo "found:"
  printf '  %s\n' "${found_names[@]}"
  exit 1
fi

notes="$(mktemp)"
sums="$(mktemp)"
trap 'rm -f "$notes" "$sums"' EXIT

{
  for f in "${files[@]}"; do
    sha256sum "$f"
  done
} >"$sums"

{
  echo "drippu v0.04 ${short}"
  echo
  echo "commit: ${sha}"
  if [ -n "$run_id" ]; then
    echo "run: ${server}/${repo}/actions/runs/${run_id}"
  fi
  echo
  echo "## Checksums"
  echo
  cat "$sums"
  echo
  echo "## Toolchain"
  echo
  mapfile -t toolchains < <(find "$artifacts_dir" -type f \( -name 'toolchain.txt' -o -name 'toolchain-*.txt' \) | sort)
  if [ "${#toolchains[@]}" -eq 0 ]; then
    echo "No per-job toolchain.txt files were uploaded."
    if command -v cmake >/dev/null 2>&1; then
      echo
      echo "Publish runner cmake:"
      cmake --version
    fi
  else
    for t in "${toolchains[@]}"; do
      echo "### ${t#${artifacts_dir}/}"
      echo
      cat "$t"
      echo
    done
  fi
} >"$notes"

echo "Releasing ${tag}"
cat "$notes"

cp "$sums" SHA256SUMS
assets=("${files[@]}" SHA256SUMS)

gh release create "$tag" \
  --repo "$repo" \
  --title "drippu v0.04 ${short}" \
  --notes-file "$notes" \
  --prerelease \
  "${assets[@]}"
