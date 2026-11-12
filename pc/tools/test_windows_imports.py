# SPDX-License-Identifier: GPL-3.0-or-later
"""The Windows player download is ONE exe: its import table may name only
DLLs that ship with every Windows 10/11 install (package_windows.SYSTEM_DLLS).

Checked on the built exe (pc/build-win/tokimeki-pc.exe, when present) and on
two small programs linked the same two ways: statically against the SDL2
release's libSDL2.a (passes) and against SDL2.dll (the negative control:
must fail).  Skipped without the MinGW toolchain
(pc/tools/setup_windows_toolchain.sh).
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

import package_windows as pw

ROOT = Path(__file__).resolve().parents[2]
GCC = ROOT / 'build/mingw-root/usr/bin/x86_64-w64-mingw32-gcc-posix'
SDL = sorted((ROOT / 'build/sdl2-mingw').glob('SDL2-*/x86_64-w64-mingw32'))
PROGRAM = '#include <SDL2/SDL.h>\nint main(int c, char **v) { (void)c; (void)v; return SDL_Init(0); }\n'
STATIC_LIBS = ['-ldinput8', '-ldxguid', '-ldxerr8', '-luser32', '-lgdi32', '-lwinmm', '-limm32', '-lole32',
               '-loleaut32', '-lshell32', '-lsetupapi', '-lversion', '-luuid', '-lcfgmgr32']


@unittest.skipUnless(GCC.is_file() and SDL, 'MinGW toolchain / SDL2 release not unpacked')
class WindowsImportTests(unittest.TestCase):
    def link(self, tmp, static):
        src = Path(tmp) / 'p.c'
        src.write_text(PROGRAM)
        exe = Path(tmp) / ('static.exe' if static else 'dynamic.exe')
        sdl = SDL[-1]
        libs = [str(sdl / 'lib/libSDL2.a')] + STATIC_LIBS if static else ['-L', str(sdl / 'lib'), '-lSDL2']
        subprocess.run([str(GCC), '-DSDL_MAIN_HANDLED', '-I', str(sdl / 'include'), str(src), '-o', str(exe)]
                       + libs + ['-static-libgcc', '-mwindows'], check=True, capture_output=True)
        return exe

    def test_static_sdl_program_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            pw.check_imports(self.link(tmp, True))

    def test_dynamic_sdl_program_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.link(tmp, False)
            self.assertIn('SDL2.dll', pw.pe_imports(exe))
            with self.assertRaises(ValueError):
                pw.check_imports(exe)

    @unittest.skipUnless((ROOT / 'pc/build-win/tokimeki-pc.exe').is_file(), 'Windows build not present')
    def test_game_exe_imports_only_system_dlls(self):
        exe = ROOT / 'pc/build-win/tokimeki-pc.exe'
        pw.check_imports(exe)
        self.assertNotIn('SDL2.dll', pw.pe_imports(exe))

    def test_package_whitelist_refuses_game_data_and_dlls(self):
        for bad in ('tokimeki-pc/SDL2.dll', 'tokimeki-pc/assets/raw/PSX.EXE', 'tokimeki-pc/disk/x.bin',
                    'tokimeki-pc/saves/card1.mcr', 'tokimeki-pc/start_game.pyz', 'tokimeki-pc/play.bat'):
            with self.assertRaises(ValueError):
                pw.check_no_game_data([bad])
        pw.check_no_game_data(['tokimeki-pc/tokimeki-pc.exe', 'tokimeki-pc/README.txt'])


if __name__ == '__main__':
    unittest.main()
