# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import override_audit as audit
from safe_process import Result


class OverrideTests(unittest.TestCase):
    def test_exception_restores_source_bytes(self):
        for failure in (KeyboardInterrupt, RuntimeError):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as d:
                root = Path(d)
                src = root / 'src'
                stash = root / 'stash'
                src.mkdir(); stash.mkdir()
                (src / 'foo.c').write_bytes(b'original\n')
                args = SimpleNamespace(stash=str(stash), build_jobs=2,
                                       frames=3, jobs=1, apply=False)
                with patch.object(audit, 'PCPORT', str(src)), \
                     patch.object(audit, 'build', side_effect=failure):
                    with self.assertRaises(failure):
                        audit.experiment(args, ['foo'], str(root), {0: ('0/0/0', 'OK')})
                self.assertEqual((src / 'foo.c').read_bytes(), b'original\n')
                self.assertEqual((stash / 'foo.c').read_bytes(), b'original\n')

    def test_incomplete_matrix_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'out'
            def fake_run(*args, **kwargs):
                out.write_text('0 0/0/0 1 0 OK\n')
                return Result(0, '')
            with patch.object(audit, 'run', side_effect=fake_run):
                with self.assertRaisesRegex(RuntimeError, 'incomplete'):
                    audit.matrix(str(out), str(Path(d) / 'logs'), 3, 1)

    def test_failed_matrix_does_not_grade_stale_report(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'out'
            out.write_text('0 0/0/0 1 0 OK\n')
            with patch.object(audit, 'run', return_value=Result(1, 'failed')):
                with self.assertRaises(RuntimeError):
                    audit.matrix(str(out), str(Path(d) / 'logs'), 3, 1)
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
