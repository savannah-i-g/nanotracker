#!/usr/bin/env bash
# GA packaging: builds a Release tree and wraps it as an AppImage.
# Usage: tools/package_appimage.sh <appimagetool> [build-dir]
#   <appimagetool>  path to appimagetool (AppImage or extracted AppRun)
#   [build-dir]     existing Release build tree (default: build)
#
# Bundled beside the binary: assets/, LICENSE + LICENSES/, and
# libopenal (the one dynamic non-system dependency — every other
# third-party library links statically on Linux). libmp3lame is
# deliberately NOT bundled: MP3 export is optional by design and
# reports cleanly when the system library is absent.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <appimagetool> [build-dir]" >&2
    exit 2
fi

tool="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$script_dir")"
build="${2:-$root/build}"
appdir="$(mktemp -d)/NanoTracker.AppDir"

[[ -x "$build/nanotracker" ]] || {
    echo "no nanotracker binary in $build — build Release first" >&2
    exit 1
}

mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" \
    "$appdir/usr/share/applications" \
    "$appdir/usr/share/icons/hicolor/256x256/apps"

cp "$build/nanotracker" "$appdir/usr/bin/"
cp -r "$root/assets" "$appdir/usr/bin/assets"
cp "$root/LICENSE" "$appdir/usr/bin/"
cp -r "$root/LICENSES" "$appdir/usr/bin/LICENSES"

# The one dynamic third-party dependency.
openal="$(ldd "$build/nanotracker" | awk '/libopenal/ {print $3}')"
[[ -n "$openal" ]] && cp "$openal" "$appdir/usr/lib/"

cat > "$appdir/nanotracker.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=NanoTracker
Comment=Native music tracker
Exec=nanotracker
Icon=nanotracker
Categories=AudioVideo;Audio;Music;
Terminal=false
DESKTOP
cp "$appdir/nanotracker.desktop" "$appdir/usr/share/applications/"
cp "$root/assets/nanotracker.png" "$appdir/nanotracker.png"
cp "$root/assets/nanotracker.png" \
    "$appdir/usr/share/icons/hicolor/256x256/apps/"

cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$here/usr/bin"
exec ./nanotracker "$@"
APPRUN
chmod +x "$appdir/AppRun"

out="$root/NanoTracker-x86_64.AppImage"
ARCH=x86_64 "$tool" "$appdir" "$out"
echo "wrote $out"
