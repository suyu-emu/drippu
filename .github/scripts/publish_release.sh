#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

artifacts_dir="${ARTIFACTS_DIR:-artifacts}"
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
sha="${GITHUB_SHA:?GITHUB_SHA is required}"
version="${RELEASE_VERSION:-v0.04}"
short="${sha:0:12}"
date_utc="$(date -u +%Y%m%d)"
display_date="$(date -u +%Y-%m-%d)"
tag="${version}-${date_utc}-${short}"
server="${GITHUB_SERVER_URL:-https://github.com}"
run_id="${GITHUB_RUN_ID:-}"

if [ ! -d "$artifacts_dir" ]; then
  echo "No artifacts directory at $artifacts_dir"
  exit 1
fi

files=()
while IFS= read -r file; do
  files+=("$file")
done < <(find "$artifacts_dir" -type f \( -name '*.zip' -o -name '*.tar.gz' -o -name '*.apk' \) | sort)
if [ "${#files[@]}" -eq 0 ]; then
  echo "No artifacts to release"
  exit 1
fi

required=(
  drippu-windows-x64.zip
  drippu-linux-x64.tar.gz
  drippu-macos-arm64.tar.gz
  drippu-freebsd-x64.tar.gz
  drippu-libretro-core-linux-x64.tar.gz
  drippu-libretro-core-windows-x64.zip
  drippu-libretro-core-android-arm64.tar.gz
  app-legacy-release.apk
  app-mainline-release.apk
  app-chromeOS-release.apk
  app-genshinSpoof-release.apk
)

artifact_names=()
for file in "${files[@]}"; do
  name="$(basename "$file")"
  if [ "${#artifact_names[@]}" -gt 0 ]; then
    for existing in "${artifact_names[@]}"; do
      if [ "$existing" = "$name" ]; then
        echo "duplicate release artifact name: $name"
        exit 1
      fi
    done
  fi
  artifact_names+=("$name")
done

missing=0
for name in "${required[@]}"; do
  found=0
  for existing in "${artifact_names[@]}"; do
    if [ "$existing" = "$name" ]; then
      found=1
      break
    fi
  done
  if [ "$found" -eq 0 ]; then
    echo "missing required artifact: $name"
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  echo "found:"
  printf '  %s\n' "${artifact_names[@]}" | sort
  exit 1
fi

friendly_platform() {
  case "$1" in
    drippu-windows-x64.zip) echo 'Windows x64' ;;
    drippu-linux-x64.tar.gz) echo 'Linux x64' ;;
    drippu-macos-arm64.tar.gz) echo 'macOS Apple Silicon' ;;
    drippu-freebsd-x64.tar.gz) echo 'FreeBSD x64' ;;
    drippu-libretro-core-linux-x64.tar.gz) echo 'Libretro core (Linux x64)' ;;
    drippu-libretro-core-windows-x64.zip) echo 'Libretro core (Windows x64)' ;;
    drippu-libretro-core-android-arm64.tar.gz) echo 'Libretro core (Android arm64)' ;;
    app-legacy-release.apk) echo 'Android (legacy)' ;;
    app-mainline-release.apk) echo 'Android (mainline)' ;;
    app-chromeOS-release.apk) echo 'Android (ChromeOS)' ;;
    app-genshinSpoof-release.apk) echo 'Android (Genshin spoof)' ;;
    *) echo 'Other' ;;
  esac
}

notes="$(mktemp)"
sums="$(mktemp)"
trap 'rm -f "$notes" "$sums"' EXIT

for file in "${files[@]}"; do
  if command -v sha256sum >/dev/null 2>&1; then
    digest="$(sha256sum "$file" | awk '{print $1}')"
  else
    digest="$(shasum -a 256 "$file" | awk '{print $1}')"
  fi
  printf '%s  %s\n' "$digest" "$(basename "$file")"
done | sort -k2 >"$sums"

previous_tag=""
if git rev-parse --git-dir >/dev/null 2>&1; then
  previous_tag="$(
    git tag --merged "$sha" --list "${version}-*" --sort=-creatordate |
      grep -Fvx "$tag" | head -n 1 || true
  )"
fi

{
  echo "Automated prerelease built from [\`${short}\`](${server}/${repo}/commit/${sha})."
  if [ -n "$run_id" ]; then
    echo "The complete build and verification results are available in [GitHub Actions](${server}/${repo}/actions/runs/${run_id})."
  fi
  echo
  echo "## Downloads"
  echo
  echo '| Platform | Artifact |'
  echo '| --- | --- |'
  for name in "${required[@]}"; do
    printf '| %s | `%s` |\n' "$(friendly_platform "$name")" "$name"
  done
  echo
  echo "## What's changed"
  echo
  if [ -n "$previous_tag" ]; then
    echo "Changes since [\`${previous_tag}\`](${server}/${repo}/compare/${previous_tag}...${sha}):"
    echo
    changes="$(git log --first-parent --format='%H%x09%h%x09%s' "${previous_tag}..${sha}")"
    if [ -n "$changes" ]; then
      while IFS=$'\t' read -r commit short_commit subject; do
        printf -- '- %s ([`%s`](%s/%s/commit/%s))\n' \
          "$subject" "$short_commit" "$server" "$repo" "$commit"
      done <<<"$changes"
    else
      echo 'No source changes since the previous automated release.'
    fi
  else
    echo 'Initial automated build for this release channel.'
  fi
  echo
  echo '## Verification'
  echo
  echo 'All listed platforms built successfully, and every attached archive passed the release artifact checks.'
  echo 'Download `SHA256SUMS` alongside the artifacts and run `sha256sum -c SHA256SUMS` to verify them.'
  echo
  echo '<details>'
  echo '<summary>SHA-256 checksums</summary>'
  echo
  echo '```text'
  cat "$sums"
  echo '```'
  echo '</details>'
  echo
  echo '<details>'
  echo '<summary>Build toolchains</summary>'
  echo
  echo '```text'
  toolchains=()
  while IFS= read -r toolchain; do
    toolchains+=("$toolchain")
  done < <(find "$artifacts_dir" -type f \( -name 'toolchain.txt' -o -name 'toolchain-*.txt' \) | sort)
  if [ "${#toolchains[@]}" -eq 0 ]; then
    echo 'No per-job toolchain metadata was uploaded.'
  else
    for toolchain in "${toolchains[@]}"; do
      printf '== %s ==\n' "${toolchain#${artifacts_dir}/}"
      cat "$toolchain"
      echo
    done
  fi
  echo '```'
  echo '</details>'
} >"$notes"

echo "Releasing ${tag}"
cat "$notes"

cp "$sums" SHA256SUMS
assets=("${files[@]}" SHA256SUMS)

gh release create "$tag" \
  --repo "$repo" \
  --target "$sha" \
  --title "drippu ${version} automated build - ${display_date}" \
  --notes-file "$notes" \
  --prerelease \
  "${assets[@]}"
