# SPDX-License-Identifier: GPL-3.0-or-later
"""Game controllers in the input HAL (pc/hal/pad.c), with SDL's virtual
joysticks instead of hardware.

Harness (pc/tests/controller_harness.c: the real pad.c, pad_device.c,
pad_map.c and settings.c on the host SDL2, >= 2.24 for
SDL_JoystickAttachVirtualEx; no window, no game data):
  * hints: face buttons by position (SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0)
    and Joy-Cons combined, and the same variable in the environment wins;
  * a joystick SDL has no mapping for (a PlayStation-style USB clone) gets
    the default mapping (pc/hal/pad_map.c): buttons 1-4 triangle / circle /
    cross / square, 7 L2, 9 select, 10 start, the hat and the stick drive
    the d-pad, and the legend calls it generic; the line is printed with
    its platform field;
  * the stick's d-pad presses past stick_threshold and lets go under three
    quarters of it;
  * unplugged with a button down, the button lets go; plugged in again it
    is found by the hot-plug event and is still on the default mapping;
  * an axis resting at one end (a wheel's pedal) holds no direction until
    it has passed the centre;
  * a mapped DualShock 4: PlayStation names, right = circle, the trigger
    is L2, the stick is the d-pad; the left stick as settings-menu presses;
    the hold-off after the menu;
  * TOKI_PORT2=1 ([input] port2_controller): the first controller is port 1
    and the second port 2 (record, player LEDs, the hold-off); when player 1
    leaves, player 2 becomes player 1 and port 2 is empty;
  * the printed line, edited, in gamecontrollerdb.txt in the settings
    folder maps the joystick instead of the default;
  * TOKI_JOYSTICK_FALLBACK=0: the unmapped joystick is ignored, a mapped
    pad still works.
Negative controls: six source mutations of pad.c, each caught.

CONTROLLER_WINDOWS=1 also builds the harness with the Windows toolchain and
the Windows build's static SDL2 (pc/Makefile WINDOWS=1) and runs every mode
under Wine, with no display.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['pc/hal/pad.c', 'pc/hal/pad_device.c', 'pc/hal/pad_map.c', 'pc/hal/settings.c',
           'pc/src/pc_debug.c']
MUTATIONS = [
    ('no default mapping', 'pc/hal/pad.c',
     '        if (!fallback_map(idx)) return;\n', '        return;\n', 'fallback'),
    ('no joystick hot-plug event', 'pc/hal/pad.c',
     '    case SDL_JOYDEVICEADDED:\n        controller_open_index(e->jdevice.which);\n',
     '', 'fallback'),
    ('no hysteresis', 'pc/hal/pad.c',
     '            if (c->axis_held & pb->mask) thr = thr * 3 / 4;\n', '', 'hysteresis'),
    ('resting axis counts', 'pc/hal/pad.c',
     '                if (v > thr / 2 || v < -thr / 2) continue;\n', '', 'rest-extreme'),
    ('port 2 never used', 'pc/hal/pad.c',
     '    return ctl_players(p, 2) == 2 && p[1] == c ? 2 : 1;\n', '    return 1;\n', 'port2'),
    ('buttons by label', 'pc/hal/pad.c',
     'SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0"',
     'SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1"', 'hints'),
]
CASES = {
    'basic': ['hints', 'fallback', 'hysteresis', 'unplug', 'replug', 'rest-extreme',
              'playstation', 'menu-stick', 'hold-off'],
    'port2': ['port2', 'port2-hold-off', 'port2-leave'],
    'db': ['db'],
    'nofallback': ['nofallback'],
}

WIN_SDL = ROOT / 'build/sdl2-mingw'
WIN_LIBS = ['-ldinput8', '-ldxguid', '-ldxerr8', '-luser32', '-lgdi32', '-lwinmm', '-limm32',
            '-lole32', '-loleaut32', '-lshell32', '-lsetupapi', '-lversion', '-luuid',
            '-lcfgmgr32']


def sdl_flags():
    r = run(['pkg-config', '--cflags', '--libs', 'sdl2'], timeout=10)
    if not r.ok:
        return None
    v = run(['pkg-config', '--modversion', 'sdl2'], timeout=10).stdout.strip()
    parts = [int(x) for x in re.findall(r'\d+', v)[:2]]
    if parts < [2, 24]:
        return None
    return shlex.split(r.stdout)


def windows_toolchain():
    """(compiler, SDL2 prefix) of the Windows build, or None."""
    cc = shutil.which('x86_64-w64-mingw32-gcc-posix')
    if not cc:
        local = ROOT / 'build/mingw-root/usr/bin/x86_64-w64-mingw32-gcc-posix'
        cc = str(local) if local.exists() else None
    sdl = sorted(WIN_SDL.glob('SDL2-*/x86_64-w64-mingw32'))
    if not cc or not sdl or not shutil.which('wine'):
        return None
    return cc, sdl[-1]


def build(folder, flags, mutation=None, windows=None):
    folder = Path(folder)
    paths = {s: str(ROOT / s) for s in SOURCES}
    if mutation:
        _, path, old, new, _ = mutation
        text = (ROOT / path).read_text()
        assert text.count(old) == 1, f'mutation anchor not unique in {path}: {old!r}'
        copy = folder / Path(path).name
        copy.write_text(text.replace(old, new))
        paths[path] = str(copy)
    common = ['-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-Ipc/include', '-Ipc/include/game',
              '-Ipc/hal', '-include', 'pc/include/game/types.h',
              str(ROOT / 'pc/tests/controller_harness.c'), *[paths[s] for s in SOURCES]]
    if windows:
        cc, sdl = windows
        binary = folder / 'controller_harness.exe'
        cmd = [cc, '-DSDL_MAIN_HANDLED', '-D__USE_MINGW_ANSI_STDIO=1',
               '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-Ipc/include/win', '-include',
               'pc/include/win/pc_win_compat.h', '-I' + str(sdl / 'include'), *common,
               'pc/hal/win/win_compat.c', str(sdl / 'lib/libSDL2.a'), *WIN_LIBS, '-lm',
               '-static', '-o', str(binary)]
    else:
        binary = folder / 'controller_harness'
        cmd = ['cc', '-fno-pie', '-no-pie', *common, *flags, '-lm', '-o', str(binary)]
    run(cmd, cwd=ROOT, timeout=300).require_ok()
    return binary


def harness(binary, mode, folder, extra=None, db=None):
    """Run one mode with a fresh settings folder; (returncode, lines, stdout)."""
    cfg = Path(folder) / ('cfg-' + mode)
    shutil.rmtree(cfg, ignore_errors=True)
    cfg.mkdir()
    if db:
        (cfg / 'gamecontrollerdb.txt').write_text(db)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('TOKI_', 'PC_', 'SDL_', 'AUTOPLAY', 'SCENE_NAV'))}
    env.update(TOKI_CONFIG_DIR=str(cfg), TOKI_SETTINGS=str(cfg / 'settings.ini'))
    env.update(extra or {})
    cmd = [str(binary), mode]
    if str(binary).endswith('.exe'):
        # no display: Wine has nowhere to show a window or a dialog
        for k in ('DISPLAY', 'WAYLAND_DISPLAY'):
            env.pop(k, None)
        env['WINEDEBUG'] = '-all'
        cmd.insert(0, shutil.which('wine'))
    r = run(cmd, cwd=folder, env=env, timeout=300)
    out = r.stdout.replace('\r\n', '\n')          # the Windows build's text mode
    lines = [l for l in out.splitlines() if not l.startswith('[')]
    return r.returncode, lines, out


class ControllerHarness(unittest.TestCase):
    windows = None

    @classmethod
    def setUpClass(cls):
        cls.flags = sdl_flags()
        if cls.windows is None and cls.flags is None:
            raise unittest.SkipTest('needs SDL2 >= 2.24 development files (pkg-config sdl2)')
        cls.dir = tempfile.TemporaryDirectory(prefix='controller-harness-')
        cls.binary = build(cls.dir.name, cls.flags, windows=cls.windows)
        rc, lines, _ = harness(cls.binary, 'guid', cls.dir.name)
        if any(l.startswith('skip') for l in lines):
            raise unittest.SkipTest(lines[0])
        cls.guid = [l.split()[1] for l in lines if l.startswith('guid ')][0]

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def mode(self, binary, mode):
        extra, db = {}, None
        if mode == 'port2':
            extra['TOKI_PORT2'] = '1'
        elif mode == 'nofallback':
            extra['TOKI_JOYSTICK_FALLBACK'] = '0'
        elif mode == 'db':
            # the line the HAL printed for the clone, edited as the player
            # guide says: circle on button 6, R1 on button 2
            line = self.printed_line()
            self.assertIn(',b:b1,', line)
            self.assertIn(',rightshoulder:b5,', line)
            db = ('# my pad\n' + line.replace(',b:b1,', ',b:b5,')
                  .replace(',rightshoulder:b5,', ',rightshoulder:b1,') + '\n')
        return harness(binary, mode, self.dir.name, extra, db)

    def printed_line(self):
        cls = type(self)
        if 'line' not in cls.__dict__:
            _, _, out = harness(self.binary, 'basic', self.dir.name)
            cls.line = re.search(r'^\[PAD\]   (\S.*)$', out, re.M).group(1)
        return cls.line

    def check_mode(self, mode):
        rc, lines, out = self.mode(self.binary, mode)
        self.assertFalse([l for l in lines if l.startswith('FAIL')], out)
        self.assertEqual(rc, 0, out)
        for case in CASES[mode]:
            self.assertIn('ok ' + case, lines, out)
        return out

    def test_basic(self):
        out = self.check_mode('basic')
        # the default mapping is announced with the line to edit
        self.assertIn('no controller mapping for "Cheap, USB Gamepad"', out)
        self.assertIn(f'[PAD]   {self.guid},Cheap  USB Gamepad,y:b0,b:b1,a:b2,x:b3,', out)
        self.assertRegex(self.printed_line(),
                         r',platform:(Linux|Windows|Mac OS X),$')
        self.assertIn('[PAD] controller connected: Cheap  USB Gamepad (default mapping)', out)
        self.assertIn('[PAD] controller disconnected: Cheap  USB Gamepad', out)
        self.assertIn('[PAD] controller connected: PS4 Controller (mapped)', out)

    def test_hint_environment_wins(self):
        rc, lines, out = harness(self.binary, 'basic', self.dir.name,
                                 {'SDL_GAMECONTROLLER_USE_BUTTON_LABELS': '1'})
        self.assertIn('ok hints', lines, out)

    def test_port2(self):
        out = self.check_mode('port2')
        self.assertIn('Xbox Series X Controller (mapped, port 1)', out)
        self.assertIn('Nintendo Switch Pro Controller (mapped, port 2)', out)

    def test_db(self):
        out = self.check_mode('db')
        self.assertRegex(out, r'\[PAD\] 1 controller mappings from .*gamecontrollerdb\.txt')
        self.assertNotIn('no controller mapping', out)

    def test_nofallback(self):
        out = self.check_mode('nofallback')
        self.assertNotIn('no controller mapping', out)

    def test_mutations_are_caught(self):
        if self.windows:
            self.skipTest('the host build runs the mutations')
        for m in MUTATIONS:
            with self.subTest(m[0]), tempfile.TemporaryDirectory() as d:
                binary = build(d, self.flags, m)
                mode = next(k for k, v in CASES.items() if m[4] in v)
                rc, lines, out = self.mode(binary, mode)
                self.assertTrue([l for l in lines if l.startswith('FAIL ' + m[4] + ':')]
                                or ('ok ' + m[4]) not in lines, out)


@unittest.skipUnless(os.environ.get('CONTROLLER_WINDOWS', '') not in ('', '0'),
                     'CONTROLLER_WINDOWS=1 runs the Windows build of the harness under Wine')
class ControllerHarnessWindows(ControllerHarness):
    @classmethod
    def setUpClass(cls):
        cls.windows = windows_toolchain()
        if cls.windows is None:
            raise unittest.SkipTest('needs the Windows toolchain, build/sdl2-mingw and wine')
        super().setUpClass()


if __name__ == '__main__':
    unittest.main()
