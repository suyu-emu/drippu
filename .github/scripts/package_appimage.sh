#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a self-contained AppImage from a completed build.
#
# Usage: package_appimage.sh [build_dir] [output]
#   build_dir  CMake build directory (default: build)
#   output     AppImage to write (default: drippu-linux-x86_64.AppImage)
set -euo pipefail

build_dir="${1:-build}"
output="${2:-drippu-linux-x86_64.AppImage}"

appimagetool_version="1.9.1"
appimagetool_url="https://github.com/AppImage/appimagetool/releases/download/${appimagetool_version}/appimagetool-x86_64.AppImage"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
appdir="$repo_root/AppDir"

rm -rf "$appdir"
mkdir -p "$appdir/lib"
cp "$build_dir/bin/suyu" "$appdir/drippu"
cp "$build_dir/bin/suyu-cmd" "$appdir/drippu-cmd" 2>/dev/null || true

EXCLUDE_REGEX='^(linux-vdso|ld-linux|libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|libresolv\.so|libnsl\.so|libutil\.so|libgcc_s\.so|libstdc\+\+\.so|libGL\.so|libGLX\.so|libGLdispatch\.so|libEGL\.so|libvulkan\.so|libX11\.so|libXext\.so|libXrandr\.so|libXcursor\.so|libXi\.so|libXfixes\.so|libxcb\.|libwayland-|libdrm\.so|libgbm\.so)'
: "${EXCLUDE_REGEX:?EXCLUDE_REGEX must be set}"

declare -A seen
collect_deps() {
  ldd "$1" 2>/dev/null | awk '{print $3}' | grep -E '^/' || true
}

bundle_recursive() {
  local queue
  queue=$(collect_deps "$1")
  while [ -n "$queue" ]; do
    local next=""
    for lib in $queue; do
      local name
      name=$(basename "$lib")
      if [[ -z "${seen[$name]:-}" ]] && ! echo "$name" | grep -qE "$EXCLUDE_REGEX"; then
        seen[$name]=1
        cp -v "$lib" "$appdir/lib/"
        next="$next $(collect_deps "$lib")"
      fi
    done
    queue="$next"
  done
}

bundle_recursive "$appdir/drippu"
[ -f "$appdir/drippu-cmd" ] && bundle_recursive "$appdir/drippu-cmd"

echo "Bundled libraries:"
ls -la "$appdir/lib"

# qt.conf below replaces Qt's compiled-in plugin path, so every plugin the app
# may load (TLS, Wayland shell/buffer integrations, imageformats, ...) must be
# bundled. Discover the plugin directory at runtime: on Ubuntu 24.04 libqxcb.so
# ships in libqt6gui6t64 while the remaining QPA plugins live in qt6-qpa-plugins.
QT_PLUGINS_DIR="$(qtpaths6 --plugin-dir 2>/dev/null || true)"
if [ -z "${QT_PLUGINS_DIR:-}" ] || [ ! -d "$QT_PLUGINS_DIR/platforms" ]; then
  QT_PLUGINS_DIR="$(dirname "$(find /usr/lib -type f -path '*/qt6/plugins/platforms/libqxcb.so' -print -quit)")/.."
fi
: "${QT_PLUGINS_DIR:?Qt plugin directory not found}"
test -d "$QT_PLUGINS_DIR/platforms"

rm -rf "$appdir/plugins"
mkdir -p "$appdir/plugins"
cp -a "$QT_PLUGINS_DIR"/. "$appdir/plugins/"
rm -rf "$appdir/plugins/wayland-graphics-integration-server" \
       "$appdir/plugins/wayland-decoration-server"

while IFS= read -r p; do
  bundle_recursive "$p"
done < <(find "$appdir/plugins" -type f -name '*.so')

for required in \
  plugins/platforms/libqxcb.so \
  plugins/tls/libqopensslbackend.so \
  plugins/wayland-shell-integration/libxdg-shell.so \
  plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so; do
  test -f "$appdir/$required" || { echo "missing Qt plugin: $required"; exit 1; }
done

cat > "$appdir/qt.conf" << 'EOF'
[Paths]
Prefix = .
Plugins = plugins
EOF

# RPATH does not cascade to a library's own dependencies, so every file with
# NEEDED entries is patched individually.
patchelf --set-rpath '$ORIGIN/lib' "$appdir/drippu"
[ -f "$appdir/drippu-cmd" ] && patchelf --set-rpath '$ORIGIN/lib' "$appdir/drippu-cmd"

for f in "$appdir"/lib/*.so*; do
  patchelf --set-rpath '$ORIGIN' "$f" 2>/dev/null || true
done

while IFS= read -r p; do
  patchelf --set-rpath '$ORIGIN/../../lib' "$p" 2>/dev/null || true
done < <(find "$appdir/plugins" -type f -name '*.so')

sed -e 's/^TryExec=suyu/TryExec=drippu/' -e 's/^Exec=suyu/Exec=drippu/' \
  "$repo_root/dist/dev.suyu_emu.suyu.desktop" > "$appdir/drippu.desktop"
cp "$repo_root/dist/drippu.svg" "$appdir/drippu.svg"
cp "$repo_root/dist/qt_themes/default/icons/256x256/drippu.png" "$appdir/drippu.png"
cp "$appdir/drippu.png" "$appdir/.DirIcon"

cat > "$appdir/AppRun" << 'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/drippu" "$@"
EOF
chmod +x "$appdir/AppRun"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

wget -q -O "$workdir/appimagetool" "$appimagetool_url"
chmod +x "$workdir/appimagetool"

ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 \
  "$workdir/appimagetool" -n "$appdir" "$output"
