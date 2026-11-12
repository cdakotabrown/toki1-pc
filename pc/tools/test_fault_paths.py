# SPDX-License-Identifier: GPL-3.0-or-later
"""The port's fault model, end to end, on Linux and (under Wine) Windows.

pc/src/main.c handles the faults game code makes on the host: a call
through a PS1 code address, a load or store on the PS1 zero page
(emulated), a sign-extended PS1 address (folded), anything else (a [CRASH]
report and a longjmp to the main loop's recovery point), and a host stall
(the watchdog: SIGALRM on Linux, a watchdog thread on Windows).  Linux does
it in a signal handler, Windows in a vectored exception handler that
redirects the faulting thread.
PC_TEST_FAULT=VBLANK:KIND provokes each one at an emulated VBlank, game
code on the stack:

  segv   [CRASH] sig=11, the game runs on to PC_TRACE_FRAMES
  zero   the load reads 0, no crash
  fold   one [FOLD] site, the load reads the PS1 byte, no crash
  stall  [CRASH] sig=14 after WATCHDOG_SEC, the game runs on
  fold with PC_FOLD=off: the negative control -- the same load is a
         [CRASH] (the fold is what recovered it)

Skipped when the binary (or Wine, for the Windows build) is missing.
"""
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
LINUX = ROOT / 'pc/build/tokimeki-pc'
WINDOWS = ROOT / 'pc/build-win/tokimeki-pc.exe'
FRAMES = 120
VBLANK = 200   # in the main loop (its first frame is at ~VBlank 134), game code on the stack


def boot(command, kind, extra=None, timeout=300):
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as work:
        os.makedirs(os.path.join(work, 'pc'))
        os.symlink(ROOT / 'pc/assets', os.path.join(work, 'pc/assets'))
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_TRACE=os.path.join(work, 't.trace'), PC_TRACE_FRAMES=str(FRAMES),
                   PC_TEST_FAULT=f'{VBLANK}:{kind}', WINEDEBUG='-all')
        env.update(extra or {})
        r = run(command + [str(ROOT / 'pc/assets')], cwd=work, env=env, timeout=timeout,
                max_output=64 * 1024 * 1024)
        return r.returncode, r.reason, r.stdout


class FaultPaths:
    command = None

    def check(self, kind, extra=None):
        rc, reason, log = boot(self.command, kind, extra)
        self.assertIsNone(reason, log[-3000:])
        self.assertEqual(rc, 0, log[-3000:])
        self.assertIn(f'[TEST] PC_TEST_FAULT {kind} at VBlank {VBLANK}', log)
        self.assertIn(f'PC_TRACE_FRAMES={FRAMES} reached', log, log[-3000:])
        return log

    def test_segv_recovers(self):
        log = self.check('segv')
        self.assertIn('[CRASH] sig=11', log)
        self.assertNotIn('[TEST] PC_TEST_FAULT segv returned', log)

    def test_zero_page_is_emulated(self):
        log = self.check('zero')
        self.assertIn('[TEST] zero-page load read 0', log)
        self.assertIn('[TEST] PC_TEST_FAULT zero returned', log)
        self.assertNotIn('[CRASH]', log)

    def test_sign_extended_address_folds(self):
        log = self.check('fold')
        self.assertIn('[FOLD] site pc_test_fault_tick', log)
        self.assertIn('[FOLD] total=1 ', log)
        self.assertIn('[TEST] PC_TEST_FAULT fold returned', log)
        self.assertNotIn('[CRASH]', log)

    def test_fold_off_is_a_crash(self):
        # negative control: without the fold the same load is a fault the
        # crash recovery catches
        log = self.check('fold', {'PC_FOLD': 'off'})
        self.assertIn('[CRASH] sig=11', log)
        self.assertNotIn('[TEST] PC_TEST_FAULT fold returned', log)

    def test_stall_trips_the_watchdog(self):
        log = self.check('stall')
        self.assertIn('[CRASH] sig=14', log)
        self.assertNotIn('[TEST] PC_TEST_FAULT stall returned', log)


class WindowsHostStall:
    def test_stall_in_a_system_dll_is_waited_out(self):
        """A stall inside Windows itself (a file write an antivirus scans:
        real Windows, 2026-11-02, the first memory-card write) is not
        jumped out of: the watchdog logs it and waits, and the call
        returns.  The unfixed build reported [CRASH] here instead."""
        log = self.check('sleep')
        self.assertIn('[WATCHDOG] host stall in', log)
        self.assertNotIn('[CRASH] sig=14', log)
        self.assertIn('[TEST] PC_TEST_FAULT sleep returned', log)


@unittest.skipUnless(LINUX.is_file(), 'Linux build missing (make -f pc/Makefile all)')
class LinuxFaultPaths(FaultPaths, unittest.TestCase):
    command = [str(LINUX)]


@unittest.skipUnless(WINDOWS.is_file() and shutil.which('wine'),
                     'Windows build or Wine missing (make -f pc/Makefile windows)')
class WindowsFaultPaths(FaultPaths, WindowsHostStall, unittest.TestCase):
    # safe_process.run starts the .exe under Wine in a private prefix
    command = [str(WINDOWS)]


if __name__ == '__main__':
    unittest.main()
