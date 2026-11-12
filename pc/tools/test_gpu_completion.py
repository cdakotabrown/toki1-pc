# SPDX-License-Identifier: GPL-3.0-or-later
"""Callbacks observe actual completed transfers and native OT rendering."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class GpuCompletionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'completion'
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2']).require_ok().stdout)
        queue = Path(os.environ.get('GPU_QUEUE_SOURCE',
                                    ROOT / 'pc/hal/gpu_queue.c')).resolve()
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1',
             '-ffunction-sections', '-fdata-sections',
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             *shlex.split(os.environ.get('GPU_QUEUE_CFLAGS', '')),
             'pc/tests/gpu_completion_harness.c', str(queue),
             'pc/hal/gpu_texture.c', 'pc/hal/gpu_vram.c',
             'pc/hal/ptr_validate.c', '-Wl,--gc-sections',
             '-o', str(cls.binary), *flags], cwd=ROOT, timeout=30).require_ok()

    def check_operation(self, name):
        run([str(self.binary), name], timeout=10).require_ok()

    def test_upload_pixels_are_visible_to_completion_callback(self):
        self.check_operation('load')

    def test_readback_buffer_is_visible_to_completion_callback(self):
        self.check_operation('store')

    def test_move_pixels_are_visible_to_completion_callback(self):
        self.check_operation('move')

    def test_ot_rendered_pixels_are_visible_to_completion_callback(self):
        self.check_operation('ot')
