# SPDX-License-Identifier: GPL-3.0-or-later
"""A completed report with failed rows must not return a successful CLI code."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import scene_matrix as matrix


class SceneMatrixExitTests(unittest.TestCase):
    def check_statuses(self, statuses, expected):
        matrix.STOP.clear()
        self.addCleanup(matrix.STOP.clear)
        rows = [dict(scene=i, end='0/0/0', frames=2,
                     crashes=int(status == 'CRASH'), first='', status=status)
                for i, status in enumerate(statuses)]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'matrix.txt'
            args = SimpleNamespace(out=str(output), jobs=1, frames=2,
                                   clock='vblank', timeout=1)
            # Exercise real report aggregation, not a copied exit predicate.
            # No game process, images or audio are simulated here.
            with patch.object(matrix, 'run_scene',
                              side_effect=lambda scene, *unused: rows[scene]):
                with redirect_stdout(io.StringIO()):
                    code = matrix.run_matrix(args, range(len(rows)),
                                             'synthetic-report-only')
            report = json.loads(Path(str(output) + '.json').read_text())
            self.assertTrue(report['complete'])
            self.assertEqual([row['status'] for row in report['rows']], statuses)
            self.assertEqual(code, expected)

    def test_all_ok_returns_success(self):
        self.check_statuses(['OK', 'OK'], 0)

    def test_recovered_crash_returns_failure(self):
        self.check_statuses(['CRASH'], 1)

    def test_timeout_returns_failure(self):
        self.check_statuses(['HANG'], 1)

    def test_early_process_exit_returns_failure(self):
        self.check_statuses(['EXIT(0)'], 1)

    def test_cancelled_row_returns_failure(self):
        self.check_statuses(['CANCELLED'], 1)

    def test_one_failed_row_cannot_hide_behind_successful_rows(self):
        self.check_statuses(['OK', 'CRASH'], 1)

    def test_empty_report_is_not_success(self):
        self.check_statuses([], 1)


if __name__ == '__main__':
    unittest.main()
