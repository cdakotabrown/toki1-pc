#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# setup_linux_sdl.sh -- a static SDL2 for the Linux player package
# (make -f pc/Makefile linux-package; docs/pc-port/WINDOWS-BUILD.md
# "Linux player package").
#
# The official SDL 2.32.10 source release, pinned and checked against the
# SHA-256 GitHub publishes for the asset, built with CMake into
# build/sdl2-linux as libSDL2.a.  SDL's own defaults load X11, Wayland,
# libdecor, PulseAudio, PipeWire and ALSA with dlopen at run time
# (SDL_*_SHARED), so the game binary links none of them: it needs only
# glibc, and runs on desktops with any of those stacks.  Building needs
# cmake and the development headers of those libraries (Debian/Ubuntu:
# libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev
# libxkbcommon-dev libwayland-dev libdecor-0-dev libpulse-dev libasound2-dev);
# a missing one only drops that back end.  Nothing here is committed.
set -eu
ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build"
SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
PREFIX="$BUILD/sdl2-linux"

if [ -f "$PREFIX/lib/libSDL2.a" ]; then
    echo "SDL2 (static, Linux): $PREFIX (already built)"
    exit 0
fi
mkdir -p "$BUILD/sdl2-dl" "$BUILD/sdl2-src"
tarball="$BUILD/sdl2-dl/SDL2-$SDL_VERSION.tar.gz"
if [ ! -f "$tarball" ]; then
    echo "SDL2: fetching SDL2-$SDL_VERSION.tar.gz"
    curl -fsSL -o "$tarball.part" "$SDL_URL"
    mv "$tarball.part" "$tarball"
fi
echo "$SDL_SHA256  $tarball" | sha256sum -c -
rm -rf "$BUILD/sdl2-src/SDL2-$SDL_VERSION"
tar -xzf "$tarball" -C "$BUILD/sdl2-src"
cmake -S "$BUILD/sdl2-src/SDL2-$SDL_VERSION" -B "$BUILD/sdl2-src/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_DEPS_SHARED=ON \
    -DSDL_X11_SHARED=ON -DSDL_WAYLAND_SHARED=ON -DSDL_PULSEAUDIO_SHARED=ON \
    -DSDL_ALSA_SHARED=ON -DSDL_PIPEWIRE_SHARED=ON -DSDL_LIBSAMPLERATE=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_NAS=OFF -DSDL_ARTS=OFF
cmake --build "$BUILD/sdl2-src/build" -j "$(nproc 2>/dev/null || echo 4)"
cmake --install "$BUILD/sdl2-src/build"
echo "Ready: make -f pc/Makefile linux-package -j4"
