# SPDX-License-Identifier: GPL-3.0-or-later
"""Retail matrix destinations, arithmetic and unchanged libgs composition."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402


class GteMatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'matrix'
        source = Path(os.environ.get('GTE_SOURCE', ROOT / 'pc/hal/gte.c'))
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1',
             '-ffunction-sections', '-fdata-sections', '-fno-strict-aliasing',
             '-Ipc/include', '-Ipc/include/game', '-I' + decomp('include'),
             *shlex.split(os.environ.get('GTE_CFLAGS', '')),
             'pc/tests/gte_matrix_harness.c', 'pc/tests/gte_test_tables.c',
             str(source),
             decomp('src/main/matched/GsMulCoord2.c'),
             '-Wl,--gc-sections', '-lm', '-o', str(cls.binary)],
            cwd=ROOT, timeout=30).require_ok()

    def exercise(self, mode):
        run([str(self.binary), mode], timeout=10).require_ok()

    def test_first_operand_receives_product(self):
        self.exercise('first')

    def test_second_operand_receives_product(self):
        self.exercise('second')

    def test_separate_destination_keeps_translation(self):
        self.exercise('separate')

    def test_same_matrix_can_be_both_operands(self):
        self.exercise('self')

    def test_shift_follows_full_dot_product(self):
        self.exercise('round')

    def test_wide_accumulator_and_signed_saturation(self):
        self.exercise('saturate')

    def test_m22_word_store_writes_the_pad(self):
        # Retail swc2 $11,16(dst): bytes 18..19 get IR3's sign extension.
        self.exercise('pad')

    def test_libgs_composes_each_child_without_mutating_view(self):
        self.exercise('coords')

    def test_composite_still_includes_parent_translation(self):
        self.exercise('composite')
