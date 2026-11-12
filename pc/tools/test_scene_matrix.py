#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resource and lifecycle regression tests; no game assets required."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import scene_matrix as matrix


def process_finished(path):
    """Read procfs once: reaping can race both exists() and read()."""
    try:
        return path.read_text().rsplit(')', 1)[1].split()[0] == 'Z'
    except (FileNotFoundError, ProcessLookupError):
        return True


class SceneMatrixTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.bin = self.work / 'fake-game'
        self.logs = self.work / 'logs'
        self.logs.mkdir()
        self.patches = patch.multiple(matrix, PC_BIN=str(self.bin),
                                     LOGDIR=str(self.logs))
        self.patches.start()
        self.addCleanup(self.patches.stop)
        matrix.STOP.clear()
        self.addCleanup(matrix.STOP.clear)

    def fake(self, body):
        self.bin.write_text('#!/usr/bin/env python3\n' + body)
        self.bin.chmod(0o755)

    def run_scene(self, timeout=2):
        return matrix.run_scene(18, 2, timeout)

    def test_ok_trace_and_stderr(self):
        self.fake("import os, sys\n"
                  "open(os.environ['PC_TRACE'], 'w').write("
                  "'frame=1 now_step=12 now_sub_step=02 now_sub_sub_step=03\\n')\n"
                  "print('PC_TRACE_FRAMES=2 reached', file=sys.stderr)\n")
        result = self.run_scene()
        self.assertEqual((result['status'], result['frames'], result['end']),
                         ('OK', 1, '18/2/3'))
        self.assertFalse(list(self.logs.iterdir()))

    def test_budget_clock_is_explicit_not_inherited(self):
        self.fake("import os\n"
                  "assert os.environ['PC_TRACE_CLOCK'] == 'game'\n"
                  "print('PC_TRACE_FRAMES=2 reached')\n")
        with patch.dict(os.environ, {'PC_TRACE_CLOCK': 'vblank'}):
            self.assertEqual(self.run_scene()['status'], 'OK')
        self.fake("import os\n"
                  "assert os.environ['PC_TRACE_CLOCK'] == 'vblank'\n"
                  "print('PC_TRACE_FRAMES=2 reached (clock=vblank)')\n")
        self.assertEqual(matrix.run_scene(18, 2, 2, clock='vblank')['status'], 'OK')

    def test_crash_count_and_first(self):
        self.fake("print('[CRASH] sig=11 pc=0x1 sym=first+0x2')\n"
                  "print('[CRASH] sig=7 pc=0x2 sym=second+0x3')\n"
                  "print('PC_TRACE_FRAMES=2 reached')\n")
        result = self.run_scene()
        self.assertEqual(result['status'], 'CRASH')
        self.assertEqual(result['crashes'], 2)
        self.assertEqual(result['first'], 'first+0x2 (sig 11)')

    def test_fault_fold_count(self):
        # the summary wins; without one (a process that died) the site lines count
        self.fake("print('[FOLD] site f+0x4 value=ffffffff80100000 scene=91 sub=0')\n"
                  "print('[FOLD] total=7 sites=1')\n"
                  "print('PC_TRACE_FRAMES=2 reached')\n")
        result = self.run_scene()
        self.assertEqual((result['status'], result['folds']), ('FOLD', 7))
        self.fake("print('[FOLD] site f+0x4 value=ffffffff80100000 scene=91 sub=0')\n"
                  "print('[FOLD] site g+0x8 value=ffffffff80100000 scene=91 sub=0')\n"
                  "print('PC_TRACE_FRAMES=2 reached')\n")
        self.assertEqual(self.run_scene()['folds'], 2)
        self.fake("print('PC_TRACE_FRAMES=2 reached')\n")
        self.assertEqual(self.run_scene()['folds'], 0)

    def test_failed_exit_cannot_be_ok(self):
        self.fake("print('PC_TRACE_FRAMES=2 reached')\nraise SystemExit(3)\n")
        self.assertEqual(self.run_scene()['status'], 'EXIT(3)')

    def test_trace_watchdog_recovery_cannot_be_ok(self):
        self.fake("import os\n"
                  "open(os.environ['PC_TRACE'], 'w').write("
                  "'frame=1 now_step=12 crashes=12\\n')\n"
                  "print('PC_TRACE_FRAMES=2 reached')\n")
        r = self.run_scene()
        self.assertEqual(r['status'], 'CRASH')
        self.assertEqual(r['crashes'], 12)

    def test_silent_timeout(self):
        self.fake('import time\ntime.sleep(30)\n')
        start = time.monotonic()
        self.assertEqual(self.run_scene(.15)['status'], 'HANG')
        self.assertLess(time.monotonic() - start, 2)

    def test_noisy_process_without_newlines_is_capped(self):
        self.fake("import os\nchunk = b'x' * 65536\n"
                  "while True: os.write(1, chunk)\n")
        result = self.run_scene()
        self.assertEqual(result['status'], 'OUTPUT_LIMIT')
        size = (self.logs / 'scene18.log').stat().st_size
        self.assertGreaterEqual(size, matrix.OUTPUT_LIMIT)
        self.assertLess(size, matrix.OUTPUT_LIMIT + 128)

    def test_trace_limit(self):
        self.fake("import os, time\n"
                  "with open(os.environ['PC_TRACE'], 'wb') as f:\n"
                  "    f.truncate(32 * 1024 * 1024)\n"
                  "time.sleep(30)\n")
        self.assertEqual(self.run_scene()['status'], 'TRACE_LIMIT')

    def test_capture_under_128_mib_address_space_limit(self):
        self.fake("import os\nchunk = b'x' * 65536\n"
                  "for _ in range(1024): os.write(1, chunk)\n")
        code = ("import resource, sys\n"
                f"sys.path.insert(0, {str(Path(matrix.__file__).parent)!r})\n"
                "import scene_matrix as m\n"
                "resource.setrlimit(resource.RLIMIT_AS, (128 << 20, 128 << 20))\n"
                f"m.PC_BIN = {str(self.bin)!r}\n"
                "assert m.run_scene(18, 2, 5)['status'] == 'OUTPUT_LIMIT'\n"
                "print('peak_rss_kib=' + str(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss))\n")
        result = subprocess.run([sys.executable, '-c', code],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('peak_rss_kib=', result.stdout)

    def test_trace_limit_on_fast_exit(self):
        self.fake("import os\n"
                  "with open(os.environ['PC_TRACE'], 'wb') as f:\n"
                  "    f.truncate(32 * 1024 * 1024)\n"
                  "print('PC_TRACE_FRAMES=2 reached')\n")
        self.assertEqual(self.run_scene()['status'], 'TRACE_LIMIT')

    def test_cancel_running_process(self):
        self.fake('import time\ntime.sleep(30)\n')
        timer = threading.Timer(.15, matrix.STOP.set)
        timer.start()
        self.addCleanup(timer.join)
        self.assertEqual(self.run_scene()['status'], 'CANCELLED')

    def test_descendant_holding_pipe_is_killed(self):
        pidfile = self.work / 'pid'
        self.fake("import os, signal, time\n"
                  "if os.fork() == 0:\n"
                  "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                  f"    open({str(pidfile)!r}, 'w').write(str(os.getpid()))\n"
                  "    time.sleep(30)\n"
                  "else:\n    os._exit(0)\n")
        self.assertEqual(self.run_scene(.3)['status'], 'HANG')
        pid = int(pidfile.read_text())
        # A zombie may remain briefly until init reaps the orphan.
        path = Path(f'/proc/{pid}/stat')
        deadline = time.monotonic() + 1
        while not process_finished(path):
            if time.monotonic() > deadline:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    break
                self.fail('descendant survived the harness timeout')
            time.sleep(.01)

    def test_procfs_reaping_race_and_process_names(self):
        path = Path('/proc/fixture/stat')
        for error in (FileNotFoundError(), ProcessLookupError()):
            with patch.object(Path, 'read_text', side_effect=error):
                self.assertTrue(process_finished(path))
        for state in ('S', 'Z'):
            with patch.object(Path, 'read_text', return_value=f'123 (worker name) {state} 1'):
                self.assertEqual(process_finished(path), state == 'Z')

    def test_spawn_failure_cleans_temporary_files(self):
        scratch = self.work / 'scratch'
        scratch.mkdir()
        with patch('tempfile.tempdir', str(scratch)):
            with self.assertRaises(FileNotFoundError):
                self.run_scene()
        self.assertEqual(list(scratch.iterdir()), [])

    def test_report_flush_and_sigterm_cleanup(self):
        pidfile = self.work / 'running'
        report = self.work / 'matrix.txt'
        self.fake("import os, time\n"
                  "if os.environ['SCENE_NAV'] == '18':\n"
                  "    print('PC_TRACE_FRAMES=2 reached')\n"
                  "else:\n"
                  f"    open({str(pidfile)!r}, 'w').write("
                  "str(os.getpid()) + '\\n' + os.environ['PC_TRACE'])\n"
                  "    time.sleep(30)\n")
        code = ("import sys, signal\n"
                f"sys.path.insert(0, {str(Path(matrix.__file__).parent)!r})\n"
                "import scene_matrix as m\n"
                f"m.PC_BIN = {str(self.bin)!r}\n"
                "signal.signal(signal.SIGTERM, m.terminate)\n"
                "sys.exit(m.main())\n")
        with subprocess.Popen([sys.executable, '-c', code, '--scenes',
                               '18-19', '--jobs', '1', '--out', str(report)],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL) as proc:
            try:
                deadline = time.monotonic() + 3
                while not pidfile.exists() or not pidfile.read_text():
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                self.assertIn('OK', report.read_text())
                pid, trace = pidfile.read_text().splitlines()
                proc.terminate()
                self.assertEqual(proc.wait(timeout=3), 143)
                self.assertFalse(Path(trace).parent.exists())
                self.assertFalse(Path(f'/proc/{pid}').exists())
            finally:
                if proc.poll() is None:
                    proc.kill()


if __name__ == '__main__':
    unittest.main()
