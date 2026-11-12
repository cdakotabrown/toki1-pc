# SPDX-License-Identifier: GPL-3.0-or-later
"""A presented standalone frame refreshes the watchdog; a real stall expires."""
from pathlib import Path
import signal
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class WatchdogTests(unittest.TestCase):
    def test_refresh_disarm_and_stalled_work(self):
        source = '''#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include "pc_watchdog.h"
int main(int argc, char **argv) {
    struct itimerval timer = {{0, 0}, {30, 0}};
    signal(SIGALRM, SIG_DFL);
    pc_watchdog_arm();
    if (argc > 1 && argv[1][0] == 'v') {
        /* a game loop inside one frame whose emulated VBlanks keep coming
         * (RPG_BAT's pause): 1.5 s, past WATCHDOG_SEC, no alarm */
        int i;
        for (i = 0; i < 30; i++) { usleep(50000); pc_watchdog_vblank(); }
        pc_watchdog_disarm();
        return 0;
    }
    if (argc > 1) {
        for (;;) pause();
    }
    getitimer(ITIMER_REAL, &timer);
    if (timer.it_value.tv_sec > 1 ||
        (!timer.it_value.tv_sec && !timer.it_value.tv_usec)) return 1;
    timer.it_value.tv_sec = 30;
    timer.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, 0);
    pc_watchdog_presented();
    getitimer(ITIMER_REAL, &timer);
    if (timer.it_value.tv_sec > 1 ||
        (!timer.it_value.tv_sec && !timer.it_value.tv_usec)) return 2;
    pc_watchdog_disarm();
    getitimer(ITIMER_REAL, &timer);
    if (timer.it_value.tv_sec || timer.it_value.tv_usec) return 3;
    timer.it_value.tv_sec = 30;
    setitimer(ITIMER_REAL, &timer, 0);
    pc_watchdog_presented();
    getitimer(ITIMER_REAL, &timer);
    pc_watchdog_disarm();
    return timer.it_value.tv_sec < 29 ? 4 : 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            run(['gcc', '-std=c89', '-O2', '-DWATCHDOG_SEC=1',
                 '-I', str(ROOT / 'pc/include'), str(path / 'test.c'),
                 str(ROOT / 'pc/hal/watchdog.c'), '-o', str(path / 'test')]).require_ok()
            run([str(path / 'test')]).require_ok()
            stalled = run([str(path / 'test'), 'stall'], timeout=3)
            self.assertIsNone(stalled.reason)
            self.assertEqual(stalled.returncode, -signal.SIGALRM)
            # emulated VBlanks refresh it (2026-11-01); the stall above,
            # with none, is the negative control
            vblanks = run([str(path / 'test'), 'vblank'], timeout=5)
            self.assertEqual(vblanks.returncode, 0)


if __name__ == '__main__':
    unittest.main()
