#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# setup_windows_toolchain.sh -- what `make -f pc/Makefile windows` needs, on a
# Debian/Ubuntu host, without root.
#
#  1. MinGW-w64 gcc/g++ (posix threads).  If x86_64-w64-mingw32-gcc-posix is
#     on PATH (sudo apt install gcc-mingw-w64-x86-64-posix
#     g++-mingw-w64-x86-64-posix) nothing is done; otherwise the same signed
#     Ubuntu packages are fetched with `apt-get download` and unpacked with
#     `dpkg -x` into build/mingw-root, which pc/Makefile finds by itself.
#     Their host dependencies (libisl, libmpc, libmpfr, libgmp, binutils-
#     common, zlib, libzstd) are on any machine with a native gcc.
#  2. SDL2: the official MinGW development release, pinned and checked
#     against its SHA-256 (the digest GitHub publishes for the asset),
#     unpacked into build/sdl2-mingw.  The game links its libSDL2.a (one exe, no DLL).
#  3. Wine (to run the build's tests here) is not fetched: sudo apt install wine64.
# Nothing here is committed: build/ is ignored.
set -eu
ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build"
SDL_VERSION=2.32.10
SDL_SHA256=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2
SDL_HOST=github.com
SDL_URL="https://$SDL_HOST/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
# Pinned: package=version, and the SHA-256 of each .deb as fetched
# (2026-11-02, Ubuntu).  A version the archive no longer carries fails the
# download; update both lists together, never one alone.
PACKAGES="gcc-mingw-w64-x86-64-posix=13.2.0-6ubuntu1+26.1
          g++-mingw-w64-x86-64-posix=13.2.0-6ubuntu1+26.1
          gcc-mingw-w64-x86-64-posix-runtime=13.2.0-6ubuntu1+26.1
          gcc-mingw-w64-base=13.2.0-6ubuntu1+26.1
          binutils-mingw-w64-x86-64=2.45.90.20260125-1ubuntu1+13.3
          binutils-mingw-w64-base=2.45.90.20260125-1ubuntu1+13.3
          mingw-w64-x86-64-dev=13.0.0-2ubuntu1 mingw-w64-common=13.0.0-2ubuntu1"
DEB_SHA256="f3dbbe4aec206d7078987c9ba3286df20a5fab4ed8c63b0f0d31969c26bdfcbf  binutils-mingw-w64-base_2.45.90.20260125-1ubuntu1+13.3_amd64.deb
f835050ed5b7fedf8f3242610fda1eb55b5472b72624f9a88de90c610b465552  binutils-mingw-w64-x86-64_2.45.90.20260125-1ubuntu1+13.3_amd64.deb
52c3613f386e462ae71105d1a0b2a48d866ccb048ad253acff629c5a2291252d  g++-mingw-w64-x86-64-posix_13.2.0-6ubuntu1+26.1_amd64.deb
a30c07e807e9874ea93909448c5d6d37f9569ebe60ad32de8e490aad198538a3  gcc-mingw-w64-base_13.2.0-6ubuntu1+26.1_amd64.deb
aca6b438e0b55b11cedcf1170a1ee26e7979e34192e34f6e6389e560908a5bfc  gcc-mingw-w64-x86-64-posix-runtime_13.2.0-6ubuntu1+26.1_amd64.deb
0855fa1d4a618550faac3a3e4352aa3226833b28184ad3c3b496a68a22c4eda2  gcc-mingw-w64-x86-64-posix_13.2.0-6ubuntu1+26.1_amd64.deb
da9b2cb14134ca0ef3a31302df50295a2dd6fee7aa4fd888b11c6f72f8ab939e  mingw-w64-common_13.0.0-2ubuntu1_all.deb
da40b9e2ae89d98d44b547a9e4fd01862858edb176bade9f12ad143e7ae7efe4  mingw-w64-x86-64-dev_13.0.0-2ubuntu1_all.deb"

mkdir -p "$BUILD"
if command -v x86_64-w64-mingw32-gcc-posix >/dev/null 2>&1; then
    echo "MinGW-w64: system $(command -v x86_64-w64-mingw32-gcc-posix)"
elif [ -x "$BUILD/mingw-root/usr/bin/x86_64-w64-mingw32-gcc-posix" ]; then
    echo "MinGW-w64: $BUILD/mingw-root (already unpacked)"
else
    echo "MinGW-w64: fetching the Ubuntu packages (apt-get download, no root)"
    mkdir -p "$BUILD/mingw-debs" "$BUILD/mingw-root"
    (cd "$BUILD/mingw-debs" && apt-get download $PACKAGES)
    (cd "$BUILD/mingw-debs" && printf '%s\n' "$DEB_SHA256" | sha256sum -c -)
    for deb in "$BUILD"/mingw-debs/*.deb; do
        dpkg -x "$deb" "$BUILD/mingw-root"
    done
    "$BUILD/mingw-root/usr/bin/x86_64-w64-mingw32-gcc-posix" --version | head -1
fi

if [ -d "$BUILD/sdl2-mingw/SDL2-$SDL_VERSION/x86_64-w64-mingw32" ]; then
    echo "SDL2: $BUILD/sdl2-mingw/SDL2-$SDL_VERSION (already unpacked)"
else
    echo "SDL2: fetching SDL2-devel-$SDL_VERSION-mingw.tar.gz"
    mkdir -p "$BUILD/sdl2-dl" "$BUILD/sdl2-mingw"
    tarball="$BUILD/sdl2-dl/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
    curl -fsSL -o "$tarball" "$SDL_URL"
    echo "$SDL_SHA256  $tarball" | sha256sum -c -
    tar -xzf "$tarball" -C "$BUILD/sdl2-mingw"
fi

if ! command -v wine >/dev/null 2>&1; then
    echo "Wine is not installed (the Windows tests need it): sudo apt install wine64"
fi
echo "Ready: make -f pc/Makefile windows -j4"
