#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# setup_linux_pkg_root.sh -- the old userland the Linux player package is
# built in, and a static SDL2 built inside it.
#
#     pc/tools/setup_linux_pkg_root.sh
#
# A binary runs on any glibc at least as new as the one it was linked
# against, so the package is linked against an old one: the official
# Ubuntu 20.04.5 "base" root file system (glibc 2.31, gcc 9.4), pinned by
# the SHA-256 Ubuntu publishes for it, unpacked into build/linux-pkg-root.
# No root and no container daemon: bwrap runs apt inside it in an
# unprivileged user namespace (apt verifies the archive's signatures with
# the keyring the base image ships).  Then SDL 2.32.10 (the same pinned
# source as pc/tools/setup_linux_sdl.sh) is built static inside it into
# build/sdl2-linux-glibc231, with X11, Wayland, PulseAudio and ALSA loaded
# at run time (dlopen), never linked.
#
# Needs: bwrap (bubblewrap), curl, tar, sha256sum, network for the first
# run.  About 600 MB in build/; nothing here is committed.
set -eu
ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build"
ROOTFS=${LINUX_PKG_ROOTFS:-$BUILD/linux-pkg-root}
BASE_VERSION=20.04.5
BASE_SHA256=60e216b60947653dc8989be3821380268315b99b2959c29882781541bfe5a426
BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/20.04/release/ubuntu-base-$BASE_VERSION-base-amd64.tar.gz"
SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
SDL_PREFIX="$BUILD/sdl2-linux-glibc231"
# the compiler, and the headers SDL needs to build each run-time back end
PACKAGES="build-essential cmake pkg-config ca-certificates
 libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev
 libxfixes-dev libxinerama-dev libxkbcommon-dev libwayland-dev
 wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev
 libpulse-dev libasound2-dev libdbus-1-dev libudev-dev"

command -v bwrap >/dev/null || { echo "setup_linux_pkg_root: needs bwrap (bubblewrap)"; exit 1; }

# 1. the root file system
if [ ! -x "$ROOTFS/usr/bin/gcc" ]; then
    mkdir -p "$BUILD/linux-pkg-dl"
    base="$BUILD/linux-pkg-dl/ubuntu-base-$BASE_VERSION-base-amd64.tar.gz"
    if [ ! -f "$base" ]; then
        echo "linux-pkg-root: fetching ubuntu-base-$BASE_VERSION-base-amd64.tar.gz"
        curl -fsSL -o "$base.part" "$BASE_URL"
        mv "$base.part" "$base"
    fi
    echo "$BASE_SHA256  $base" | sha256sum -c -
    rm -rf "$ROOTFS"
    mkdir -p "$ROOTFS"
    # device nodes cannot be created without root; bwrap supplies /dev
    tar -xzf "$base" -C "$ROOTFS" --exclude='./dev/*' --exclude='dev/*'
    cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
    echo "linux-pkg-root: apt-get install (in bwrap)"
    # the archive's own signatures are checked; -o APT::Sandbox::User=root
    # because the namespace maps a single user
    bwrap --unshare-user --uid 0 --gid 0 --die-with-parent \
        --bind "$ROOTFS" / --dev /dev --proc /proc --tmpfs /tmp \
        --clearenv --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --setenv DEBIAN_FRONTEND noninteractive --setenv HOME /root \
        -- sh -c "apt-get -q -o APT::Sandbox::User=root update >/dev/null &&
                  apt-get -q -y -o APT::Sandbox::User=root install --no-install-recommends $(echo $PACKAGES) >/dev/null &&
                  apt-get -q -o APT::Sandbox::User=root clean"
fi
echo "linux-pkg-root: $ROOTFS ($(sh "$ROOT/pc/tools/linux_pkg_exec.sh" ldd --version | head -1))"

# 2. SDL2, static, built inside the root
if [ -f "$SDL_PREFIX/lib/libSDL2.a" ]; then
    echo "SDL2 (static, glibc 2.31 root): $SDL_PREFIX (already built)"
    exit 0
fi
mkdir -p "$BUILD/sdl2-dl" "$BUILD/sdl2-src-glibc231"
tarball="$BUILD/sdl2-dl/SDL2-$SDL_VERSION.tar.gz"
if [ ! -f "$tarball" ]; then
    echo "SDL2: fetching SDL2-$SDL_VERSION.tar.gz"
    curl -fsSL -o "$tarball.part" "$SDL_URL"
    mv "$tarball.part" "$tarball"
fi
echo "$SDL_SHA256  $tarball" | sha256sum -c -
src="$BUILD/sdl2-src-glibc231"
rm -rf "$src/SDL2-$SDL_VERSION" "$src/build"
tar -xzf "$tarball" -C "$src"
X="sh $ROOT/pc/tools/linux_pkg_exec.sh"
cd "$ROOT"
$X cmake -S "$src/SDL2-$SDL_VERSION" -B "$src/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_DEPS_SHARED=ON \
    -DSDL_X11_SHARED=ON -DSDL_WAYLAND_SHARED=ON -DSDL_PULSEAUDIO_SHARED=ON \
    -DSDL_ALSA_SHARED=ON -DSDL_PIPEWIRE=OFF -DSDL_LIBSAMPLERATE=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_NAS=OFF -DSDL_ARTS=OFF \
    -DSDL_KMSDRM=OFF -DSDL_LIBUDEV=ON >/dev/null
$X cmake --build "$src/build" -j "$(nproc 2>/dev/null || echo 4)" >/dev/null
$X cmake --install "$src/build" >/dev/null
echo "SDL2 (static, glibc 2.31 root): $SDL_PREFIX"
echo "Ready: make -f pc/Makefile linux-package"
