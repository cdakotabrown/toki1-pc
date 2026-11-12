# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import sys
import tempfile
import unittest

from safe_process import lines, run


class SafeProcessTests(unittest.TestCase):
    def test_output_limit_and_file_bound(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'out'
            r = run([sys.executable, '-c',
                     "import os; b=b'x'*65536\nwhile True: os.write(2,b)"],
                    log_path=log, max_output=100000, timeout=2)
            self.assertEqual(r.reason, 'OUTPUT_LIMIT')
            self.assertFalse(r.ok)
            self.assertEqual(log.stat().st_size, 100000)
            self.assertEqual(len(r.stdout), 65536)

    def test_timeout_retains_text(self):
        r = run([sys.executable, '-c',
                 "import time; print('hello',flush=True); time.sleep(30)"],
                timeout=.1)
        self.assertEqual(r.reason, 'TIMEOUT')
        self.assertEqual(r.stdout, 'hello\n')
        with self.assertRaises(RuntimeError):
            r.require_ok()

    def test_reject_long_records(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'input'
            p.write_text('hello\n' + 'x' * 100)
            with self.assertRaises(ValueError):
                list(lines(p, max_line=50))
            with self.assertRaises(ValueError):
                list(lines(p, max_bytes=10))

    def test_normal_and_exit_failure(self):
        r = run([sys.executable, '-c', "print('ok')"])
        self.assertTrue(r.ok)
        self.assertEqual(r.stdout, 'ok\n')
        r = run([sys.executable, '-c', 'raise SystemExit(4)'])
        self.assertFalse(r.ok)
        self.assertEqual(r.returncode, 4)


if __name__ == '__main__':
    unittest.main()
