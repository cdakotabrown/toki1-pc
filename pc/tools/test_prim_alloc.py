# SPDX-License-Identifier: GPL-3.0-or-later
"""Native packet allocations must preserve retail cursor advancement.

The no-bump bug aliases successive primitives and turns raw OT insertion into
self-links. These tests use the actual allocator, real GPU traversal/rendering,
and private child-process PS1 mappings, with no assets or synthetic renderer.
PRIM_ALLOC_SOURCE selects the pre-fix/candidate native override.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class PrimAllocTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='prim-alloc-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'prim-alloc'
        source = Path(os.environ.get('PRIM_ALLOC_SOURCE',
                                    ROOT / 'src/main/pc-port/prim_alloc.c'))
        source = source.resolve(strict=True)
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                timeout=10).require_ok().stdout)
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        run([*compiler, '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections',
             '-DPRIM_ALLOC_SOURCE="' + str(source) + '"',
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/prim_alloc_harness.c', 'pc/hal/gpu_texture.c',
             'pc/hal/gpu_vram.c', 'pc/hal/gpu_queue.c', '-Wl,--gc-sections', '-o', str(cls.binary),
             *flags], cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        env = dict(os.environ, MDFN_GPU='0', GP0_TRACE='0', PC_DEBUG='0')
        run([str(self.binary), case], cwd=ROOT, env=env,
            timeout=10, max_output=16000).require_ok()

    def test_returns_old_cursor_and_advances_selected_record(self):
        self.check_case('cursor')

    def test_request_rounding_preserves_retail_cursor_units(self):
        self.check_case('round')

    def test_banks_advance_independently_without_touching_size(self):
        self.check_case('banks')

    def test_zero_request_keeps_cursor_and_reads_only_32_bit_base(self):
        self.check_case('zero')

    def test_save_shade_six_allocation_sequence_never_aliases(self):
        self.check_case('shade')

    def test_two_raw_inserted_allocations_render_without_self_links(self):
        self.check_case('render')


if __name__ == '__main__':
    unittest.main()
