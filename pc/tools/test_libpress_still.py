# SPDX-License-Identifier: GPL-3.0-or-later
"""libpress decodes a still BS from RAM the way the MDEC does.

decdct (the background pictures) runs DecDCTvlc on a bitstream the CD put in
main RAM and DecDCTout slices it into the same buffer. Until 2026-10-30 the
PC libpress knew only movie frames: the still was "unknown" and every slice
came back zero (scene 64's background, VRAM (320,416,320,88), stayed black).
The harness drives the real pc/hal/libpress.c and mdec_native.c.
LIBPRESS_SOURCE selects an older libpress.c for a negative control;
LIBPRESS_CFLAGS adds e.g. sanitizers.
"""
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


class LibpressStillTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'libpress-still'
        source = Path(os.environ.get('LIBPRESS_SOURCE', ROOT / 'pc/hal/libpress.c')).resolve()
        # the HAL's own flags (pc/Makefile CFLAGS), at -O1 -g
        run(['cc', '-std=gnu17', '-fpermissive', '-w', '-funsigned-char', '-O1', '-g',
             '-fno-strict-aliasing', '-D_GNU_SOURCE', '-DPC_PORT',
             '-include', 'pc/include/game/types.h', '-Ipc/include/game', '-Ipc/include',
             '-Ipc/hal', '-I' + decomp('include'),
             *shlex.split(os.environ.get('LIBPRESS_CFLAGS', '')),
             'pc/tests/libpress_still_harness.c', str(source),
             'pc/hal/mdec_native.c', 'pc/hal/ptr_validate.c', '-lm',
             '-o', str(cls.binary)], cwd=ROOT, timeout=60).require_ok()

    def check_case(self, name):
        run([str(self.binary), name], timeout=10).require_ok()

    def test_slices_decode_in_place_with_bit15(self):
        self.check_case('still')

    def test_mode_without_bit15(self):
        self.check_case('nostp')


if __name__ == '__main__':
    unittest.main()
