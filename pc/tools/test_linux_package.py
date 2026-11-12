#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_linux_package.py -- the Linux player package's glibc floor.

    python3 pc/tools/test_linux_package.py

- The gate itself (package_linux.glibc_requirements / check_glibc) on
  synthetic `objdump -T` text: versions are read from either column form,
  compared numerically (2.4 < 2.31), libstdc++ versions are refused.
- The package binary (pc/build-linux-pkg/tokimeki-pc) asks for no glibc
  newer than 2.31 and links glibc only.  Negative controls, refused by the
  gate: the dev build (pc/build/tokimeki-pc) and the package built the
  previous way, on the host (`make -f pc/Makefile linux-package
  LINUX_PKG_ROOT=host` -> pc/build-linux-pkg-host/tokimeki-pc: the same
  static SDL and flags, the build machine's glibc).
- Both packages run inside the Ubuntu 20.04 root (build/linux-pkg-root,
  glibc 2.31) through pc/tools/linux_pkg_exec.sh --run with `--licenses`
  (a console mode: no window, no game data): the package prints its
  licences; the host-built one is stopped by the dynamic loader
  ("GLIBC_2.xx not found").
Cases that need a binary or the root skip when it is not built.
"""
import os
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package_linux as P  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / 'pc/build-linux-pkg/tokimeki-pc'
DEV = ROOT / 'pc/build/tokimeki-pc'
HOST_PKG = ROOT / 'pc/build-linux-pkg-host/tokimeki-pc'
ROOTFS = ROOT / 'build/linux-pkg-root'
EXEC = ROOT / 'pc/tools/linux_pkg_exec.sh'

SAMPLE = """
pc/build/tokimeki-pc:     file format elf64-x86-64

DYNAMIC SYMBOL TABLE:
0000000000000000      DF *UND*	0000000000000000 (GLIBC_2.2.5) memcpy
0000000000000000      DF *UND*	0000000000000000  GLIBC_2.4   __stack_chk_fail
0000000000000000      DF *UND*	0000000000000000 (GLIBC_2.29) exp
0000000000000000      DF *UND*	0000000000000000 (GLIBC_2.34) __libc_start_main
0000000000000000  w   D  *UND*	0000000000000000  Base        __gmon_start__
0000000000000000      DF *UND*	0000000000000000 (CXXABI_1.3) __cxa_guard_acquire
"""


def in_root(binary, cwd):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SDL_', 'PC_', 'TOKI_', 'DISPLAY', 'WAYLAND', 'XDG_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', HOME=str(cwd),
               LINUX_PKG_ROOTFS=str(ROOTFS))
    return subprocess.run(['sh', str(EXEC), '--run', str(binary), '--licenses'], cwd=str(cwd),
                          env=env, capture_output=True, text=True, timeout=120)


class Gate(unittest.TestCase):
    def test_parse(self):
        req = P.glibc_requirements(SAMPLE)
        self.assertEqual(sorted(req), ['CXXABI_1.3', 'GLIBC_2.2.5', 'GLIBC_2.29', 'GLIBC_2.34',
                                       'GLIBC_2.4'])
        self.assertEqual(req['GLIBC_2.34'], ['__libc_start_main'])
        self.assertEqual(req['GLIBC_2.4'], ['__stack_chk_fail'])

    def test_numeric_order(self):
        self.assertLess(P.version_tuple('GLIBC_2.4'), P.version_tuple('GLIBC_2.31'))
        self.assertLess(P.version_tuple('GLIBC_2.2.5'), P.version_tuple('GLIBC_2.3'))
        self.assertGreater(P.version_tuple('GLIBC_2.34'), P.GLIBC_FLOOR)
        self.assertEqual(P.GLIBC_FLOOR, (2, 31))


@unittest.skipUnless(PKG.exists(), 'no package binary (make -f pc/Makefile linux-package)')
class PackageBinary(unittest.TestCase):
    def test_floor(self):
        newest = P.check_glibc(PKG)
        self.assertLessEqual(P.version_tuple(newest), P.GLIBC_FLOOR, newest)

    def test_needed_glibc_only(self):
        P.check_needed(PKG)
        self.assertTrue(set(P.needed(PKG)) <= P.GLIBC_LIBS)


class Controls(unittest.TestCase):
    def refused(self, binary):
        if not binary.exists():
            self.skipTest(f'no {binary}')
        with self.assertRaises(ValueError) as cm:
            P.check_glibc(binary)
        self.assertIn('newer glibc than 2.31', str(cm.exception))

    def test_dev_build_refused(self):
        self.refused(DEV)

    def test_host_package_refused(self):
        self.refused(HOST_PKG)


@unittest.skipUnless((ROOTFS / 'usr/bin/gcc').exists() and PKG.exists() and HOST_PKG.exists(),
                     'no build root / binaries (pc/tools/setup_linux_pkg_root.sh)')
class OldUserland(unittest.TestCase):
    def setUp(self):
        self.cwd = ROOT / 'build/test-linux-package'
        self.cwd.mkdir(parents=True, exist_ok=True)

    def test_userland_is_old(self):
        r = subprocess.run(['sh', str(EXEC), '--run', 'ldd', '--version'], cwd=str(ROOT),
                           env=dict(os.environ, LINUX_PKG_ROOTFS=str(ROOTFS)),
                           capture_output=True, text=True, timeout=60)
        self.assertIn('2.31', r.stdout.splitlines()[0])

    def test_package_runs(self):
        r = in_root(PKG, self.cwd)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        self.assertIn('Simple DirectMedia Layer', r.stdout + r.stderr)

    def test_host_package_does_not(self):
        r = in_root(HOST_PKG, self.cwd)
        self.assertNotEqual(r.returncode, 0)
        self.assertRegex(r.stderr, r"GLIBC_2\.(3[2-9]|[4-9][0-9])' not found")


if __name__ == '__main__':
    unittest.main()
