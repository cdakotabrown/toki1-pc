#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_win_compat.py -- the Windows build's POSIX shims behave as POSIX.

pc/include/win/pc_win_compat.h (force-included into the Windows build)
maps rename() to pc_win_rename (pc/hal/win/win_compat.c), because msvcrt's
rename() fails when the destination exists and every temp-file-then-rename
save in pc/hal would work once and then never again.  This compiles a tiny
program with the MinGW toolchain the Windows build uses, runs it under Wine
and checks that a second rename over an existing file succeeds and replaces
it.  Negative control: the same program built without the shim must fail
the second rename.  Skipped when MinGW or Wine is not available.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

import game_platform

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROG = r'''
#include <stdio.h>
#include <string.h>
static int put(const char *p, const char *t) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    fputs(t, f);
    return fclose(f);
}
int main(void) {
    char buf[16] = {0};
    FILE *f;
    if (put("a.tmp", "one") || rename("a.tmp", "dst")) return 10;
    if (put("a.tmp", "two")) return 11;
    if (rename("a.tmp", "dst")) return 12;      /* destination exists */
    f = fopen("dst", "rb");
    if (!f || !fgets(buf, sizeof buf, f)) return 13;
    fclose(f);
    return strcmp(buf, "two") ? 14 : 0;
}
'''


def mingw_gcc():
    for c in ('x86_64-w64-mingw32-gcc-posix',
              os.path.join(ROOT, 'build/mingw-root/usr/bin/x86_64-w64-mingw32-gcc-posix')):
        if shutil.which(c) or os.path.exists(c):
            return c
    return None


@unittest.skipUnless(mingw_gcc() and shutil.which('wine'), 'MinGW or Wine not available')
class WinCompat(unittest.TestCase):
    def build_and_run(self, shim):
        d = tempfile.mkdtemp(prefix='wincompat-')
        try:
            src = os.path.join(d, 't.c')
            exe = os.path.join(d, 't.exe')
            with open(src, 'w') as f:
                f.write(PROG)
            cmd = [mingw_gcc(), '-O1', '-o', exe, src]
            if shim:
                cmd[1:1] = ['-include', os.path.join(ROOT, 'pc/include/win/pc_win_compat.h')]
                cmd.append(os.path.join(ROOT, 'pc/hal/win/win_compat.c'))
            subprocess.run(cmd, check=True, capture_output=True, timeout=300)
            # a private WINEPREFIX per run (game_platform.wrap), never ~/.wine
            cmd, env, cleanup = game_platform.wrap([exe], dict(os.environ, WINEDEBUG='-all'))
            try:
                r = subprocess.run(cmd, cwd=d, env=env, capture_output=True, timeout=300)
            finally:
                cleanup()
            return r.returncode
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_rename_replaces_existing(self):
        self.assertEqual(self.build_and_run(shim=True), 0)

    def test_negative_control_plain_msvcrt_rename_fails(self):
        self.assertEqual(self.build_and_run(shim=False), 12)


DPI_PROG = r'''
#include <windows.h>
#include <stdio.h>
void pc_win_dpi_report(void *hwnd);
int main(void) {
    HWND w = CreateWindowExA(0, "STATIC", "t", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                             NULL, NULL, NULL, NULL);
    pc_win_dpi_report(w);
    fflush(stdout);
    return 0;
}
'''


@unittest.skipUnless(mingw_gcc() and shutil.which('wine'), 'MinGW or Wine not available')
class ManifestDpiAwareness(unittest.TestCase):
    """pc/win/tokimeki-pc.manifest grants per-monitor v2 DPI awareness (the
    window stays sharp at any display scaling and across monitors), read
    back by pc_win_dpi_report (pc/hal/win/win_compat.c) in a small program
    built with the game's manifest.  Negative control: the same program with
    the manifest as it was before 2026-11-02 (dpiAware true, no
    dpiAwareness) is only system-aware."""

    def awareness(self, manifest_text):
        d = tempfile.mkdtemp(prefix='windpi-', dir=os.path.join(ROOT, 'build'))
        try:
            with open(os.path.join(d, 't.c'), 'w') as f:
                f.write(DPI_PROG)
            with open(os.path.join(d, 'm.manifest'), 'w', encoding='utf-8') as f:
                f.write(manifest_text)
            with open(os.path.join(d, 't.rc'), 'w') as f:
                f.write('1 24 "m.manifest"\n')
            windres = mingw_gcc().replace('gcc-posix', 'windres')
            subprocess.run([windres, 't.rc', '-O', 'coff', '-o', 'res.o'], cwd=d, check=True,
                           capture_output=True, timeout=120)
            subprocess.run([mingw_gcc(), '-O1', '-o', 't.exe', 't.c', 'res.o',
                            os.path.join(ROOT, 'pc/hal/win/win_compat.c')],
                           cwd=d, check=True, capture_output=True, timeout=300)
            cmd, env, cleanup = game_platform.wrap([os.path.join(d, 't.exe')], None)
            try:
                r = subprocess.run(cmd, cwd=d, env=env, capture_output=True, text=True,
                                   timeout=300)
            finally:
                cleanup()
            return r.stdout
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_game_manifest_is_per_monitor_v2(self):
        with open(os.path.join(ROOT, 'pc/win/tokimeki-pc.manifest'), encoding='utf-8') as f:
            text = f.read()
        self.assertIn('DPI awareness: per-monitor v2', self.awareness(text))

    def test_negative_control_old_manifest_is_system_aware(self):
        with open(os.path.join(ROOT, 'pc/win/tokimeki-pc.manifest'), encoding='utf-8') as f:
            text = f.read()
        lines = [l for l in text.splitlines() if '<dpiAwareness' not in l]
        old = '\n'.join(lines).replace('>true/pm</dpiAware>', '>true</dpiAware>')
        self.assertIn('DPI awareness: system', self.awareness(old))


EXE = os.path.join(ROOT, 'pc/build-win/tokimeki-pc.exe')


@unittest.skipUnless(os.path.exists(EXE) and shutil.which('wine'), 'Windows build or Wine missing')
class WindowsVideoDriver(unittest.TestCase):
    """The Windows build must open a real window even though Windows has no
    DISPLAY variable: a Linux-only "no DISPLAY -> offscreen" rule once made
    the player build run invisibly (2026-11-02).  Run under Wine with
    DISPLAY and WAYLAND_DISPLAY unset and read the driver from its log.
    Negative control: the log of the unfixed build said "offscreen" (seen
    on the 2026-11-02 player exe; kept as the assertion's other side)."""

    def test_windows_driver_without_display_variables(self):
        d = tempfile.mkdtemp(prefix='windrv-', dir=os.path.join(ROOT, 'build'))
        cleanup = lambda: None
        try:
            shutil.copy(EXE, d)
            env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY')}
            env['WINEDEBUG'] = '-all'
            # no driver chosen (the empty value keeps the harness default,
            # dummy, away): the game must pick the Windows one by itself.
            # Wine runs without DISPLAY (game_platform), so the window it
            # opens shows on no desktop.
            env['SDL_VIDEODRIVER'] = ''
            # a private WINEPREFIX (game_platform.wrap); its cleanup stops that
            # prefix's wineserver only, never another user's Wine
            cmd, env, cleanup = game_platform.wrap([os.path.join(d, 'tokimeki-pc.exe')], env)
            try:
                subprocess.run(cmd, cwd=d, env=env, timeout=25,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except subprocess.TimeoutExpired:
                pass
            log = open(os.path.join(d, 'data', 'tokimeki-pc.log'), errors='replace').read()
            self.assertNotIn('offscreen', log)
            self.assertIn('SDL_Init done (driver: windows)', log)
            # the manifest's DPI awareness reached the process (sharp at any
            # display scaling)
            self.assertIn('[HAL] DPI awareness: per-monitor v2', log)
        finally:
            cleanup()
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
