# SPDX-License-Identifier: GPL-3.0-or-later
"""SPU register bridge (pc/hal/spu.c spu_mmio_sync) and SPU core timing.

Builds pc/tests/spu_mmio_harness.c against the actual pc/hal/spu.c and the
actual SPU core archive.  NOT a PCM or sound-parity test.

  python -m unittest discover -s pc/tools -p test_spu_mmio.py -v

Negative controls: SPU_SOURCE=<older spu.c> (no bridge: fails to build or
fails `bridge`) and SPU_ARCHIVE=<older spu_psx.a> (register accesses ran one
discarded SPU sample each: fails `reads`); the spu.c before the reverb
bridge (reverb registers and SPUCNT held back) fails `reverb`.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run
import spu_core

ROOT = Path(__file__).resolve().parents[2]


class SpuMmioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='spu-mmio-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'spu-mmio'
        source = Path(os.environ.get('SPU_SOURCE', ROOT / 'pc/hal/spu.c')).resolve(strict=True)
        archive = (Path(os.environ['SPU_ARCHIVE']) if 'SPU_ARCHIVE' in os.environ
                   else spu_core.archive()).resolve(strict=True)
        sdl = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2'],
                              timeout=10).require_ok().stdout)
        cc = shlex.split(os.environ.get('CC', 'cc'))
        # Section GC drops the unreferenced audio/XA/SEQ paths; the SDL
        # library satisfies the audio-device calls left in reachable code.
        run([*cc, '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1',
             '-ffunction-sections', '-fdata-sections',
             '-DSPU_SOURCE="' + str(source) + '"',
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/spu_mmio_harness.c', 'pc/hal/spu_psx.c', str(archive),
             '-Wl,--gc-sections', '-Wl,--unresolved-symbols=ignore-in-object-files',
             '-lstdc++', '-lm', *sdl, '-no-pie', '-o', str(cls.binary)],
            cwd=ROOT, timeout=60).require_ok()

    def case(self, name):
        run([str(self.binary), name], cwd=ROOT, timeout=20, max_output=16000).require_ok()

    def test_register_reads_do_not_advance_the_spu(self):
        self.case('reads')

    def test_page_writes_reach_the_core_and_keys_are_consumed(self):
        self.case('bridge')

    def test_reverb_registers_and_enable_reach_the_core(self):
        self.case('reverb')


if __name__ == '__main__':
    unittest.main()
