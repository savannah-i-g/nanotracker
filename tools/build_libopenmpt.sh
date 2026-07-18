#!/usr/bin/env bash
# Vendored libopenmpt build. libopenmpt ships no CMake package; this
# script produces the static library CMake's Findlibopenmpt.cmake
# resolves against. Rerun after deleting third_party/ or on version
# bumps (update VERSION + Docs/DEPENDENCIES.md together).
set -euo pipefail

VERSION="0.8.3"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$script_dir")"
third_party="$root/third_party"
src="$third_party/libopenmpt-src"
install="$third_party/libopenmpt-install"

mkdir -p "$third_party"
if [[ ! -d "$src" ]]; then
    tarball="$third_party/libopenmpt-$VERSION.tar.gz"
    curl -sfL "https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-$VERSION+release.autotools.tar.gz" \
        -o "$tarball"
    tar -xzf "$tarball" -C "$third_party"
    mv "$third_party/libopenmpt-$VERSION+release.autotools" "$src"
    rm "$tarball"
fi

cd "$src"
# Static lib, no tools/examples, no optional codec/audio-backend deps —
# module decoding for the classic formats needs none of them.
./configure --prefix="$install" --disable-shared --enable-static \
    --disable-openmpt123 --disable-examples --disable-tests \
    --without-mpg123 --without-ogg --without-vorbis --without-vorbisfile \
    --without-pulseaudio --without-portaudio --without-portaudiocpp \
    --without-sndfile --without-flac CXXFLAGS="-O2"
make -j"$(nproc)"
make install
echo "libopenmpt $VERSION installed to $install"
