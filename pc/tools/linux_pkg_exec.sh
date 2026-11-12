#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# linux_pkg_exec.sh -- run a command inside the Linux package's old
# userland.
#
#     pc/tools/linux_pkg_exec.sh gcc -c foo.c -o foo.o      (build mode)
#     pc/tools/linux_pkg_exec.sh --run GAME [ARGS...]       (run mode)
#
# The root file system is the pinned Ubuntu 20.04 base that
# pc/tools/setup_linux_pkg_root.sh unpacks into build/linux-pkg-root
# (glibc 2.31, gcc 9).  bwrap (unprivileged user namespaces; no root, no
# container daemon) mounts it read-only as /, mounts the repository
# read-write at its own path, keeps the working directory, and runs the
# command without network.
#
# Build mode (the Makefile's CC / CXX / AR for `make -f pc/Makefile
# linux-package`) runs with a plain environment, so make and the Python
# generators stay on the host and only the compiler, assembler, archiver
# and linker see the old glibc: the binary links against the old root's
# libc, never the host's.
#
# Run mode (pc/tools/fresh_install.py --userland) keeps the caller's
# environment (SDL_VIDEODRIVER=dummy, PC_TRACE, HOME, ...) and also mounts,
# read-only, every directory listed in LINUX_PKG_RUN_BINDS (colon
# separated; the disc's folder): the player package running on an old
# distribution's libraries, with nothing of the host's /usr visible.
#
# LINUX_PKG_ROOTFS overrides the root file system.
set -eu
ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
ROOTFS=${LINUX_PKG_ROOTFS:-$ROOT/build/linux-pkg-root}
MODE=build
if [ "${1:-}" = --run ]; then
    MODE=run
    shift
fi
if [ ! -x "$ROOTFS/usr/bin/gcc" ]; then
    echo "linux_pkg_exec: no build root at $ROOTFS (run pc/tools/setup_linux_pkg_root.sh)" >&2
    exit 2
fi
if [ $# -eq 0 ]; then
    echo "usage: linux_pkg_exec.sh [--run] COMMAND [ARGS...]" >&2
    exit 2
fi
# mount points (the root itself is mounted read-only)
[ -d "$ROOTFS$ROOT" ] || mkdir -p "$ROOTFS$ROOT"
if [ "$MODE" = build ]; then
    exec bwrap --unshare-all --die-with-parent \
        --ro-bind "$ROOTFS" / \
        --dev /dev --proc /proc --tmpfs /tmp \
        --bind "$ROOT" "$ROOT" \
        --chdir "$(pwd)" \
        --clearenv --setenv PATH /usr/bin:/bin --setenv LANG C.UTF-8 \
        --setenv HOME /tmp --setenv TMPDIR /tmp \
        -- "$@"
fi
# run mode: one --ro-bind per extra directory, built into "$@" in front
# of the command (POSIX sh has no arrays)
n=$#
old_ifs=$IFS
IFS=:
for d in ${LINUX_PKG_RUN_BINDS:-}; do
    [ -d "$d" ] || { echo "linux_pkg_exec: no directory $d (LINUX_PKG_RUN_BINDS)" >&2; exit 2; }
    [ -d "$ROOTFS$d" ] || mkdir -p "$ROOTFS$d"
    set -- "$@" --ro-bind "$d" "$d"
done
IFS=$old_ifs
set -- "$@" --
# rotate the binds and the "--" (the words after the first n) in front of
# the command
i=0
while [ "$i" -lt "$n" ]; do
    a=$1
    shift
    set -- "$@" "$a"
    i=$((i + 1))
done
exec bwrap --unshare-all --die-with-parent \
    --ro-bind "$ROOTFS" / \
    --dev /dev --proc /proc --tmpfs /tmp \
    --bind "$ROOT" "$ROOT" \
    --chdir "$(pwd)" \
    --unsetenv LD_LIBRARY_PATH --unsetenv LD_PRELOAD \
    "$@"
