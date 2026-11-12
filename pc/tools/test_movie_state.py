# SPDX-License-Identifier: GPL-3.0-or-later
"""Native movie helpers must not use PS1 offsets or truncate host pointers."""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402


class MovieStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'movie-state'
        sources = []
        for name in ('strSync', 'strNextVlc'):
            override = ROOT / 'src/main/pc-port' / (name + '.c')
            sources.append(str(override if override.exists() else
                               DECOMP / 'src/main/matched' / (name + '.c')))
        run(['gcc', '-std=c89', '-O1', '-fno-strict-aliasing', '-fPIE', '-pie',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-Ipc/include/game', 'pc/tests/movie_state_harness.c',
             *sources, '-o', str(cls.binary)], cwd=ROOT, timeout=30).require_ok()

    def check_case(self, name):
        run([str(self.binary), name], timeout=10).require_ok()

    def test_completed_frame_does_not_erase_second_rectangle(self):
        self.check_case('done')

    def test_sync_timeout_selects_next_rectangle_without_corruption(self):
        self.check_case('sync_timeout')

    def test_vlc_toggles_index_and_preserves_full_pointers(self):
        self.check_case('vlc')

    def test_retry_call_order_is_preserved(self):
        self.check_case('retry')

    def test_exhausted_poll_does_not_mutate_decoder_state(self):
        self.check_case('vlc_timeout')


if __name__ == '__main__':
    unittest.main()
