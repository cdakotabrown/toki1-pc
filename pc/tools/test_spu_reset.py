# SPDX-License-Identifier: GPL-3.0-or-later
"""SPU initialization register sequence, CPU work and cancellation."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run
import spu_core

ROOT = Path(__file__).resolve().parents[2]
CORE_HELPER = r'''#include "spu.h"
#include <algorithm>
extern "C" void ds_spu_init(void);
extern "C" void test_core_init(void) {
    ds_spu_init();
    auto& ram = SPU::GetWritableRAM();
    std::fill(ram.begin(), ram.end(), 0xa5);
}
extern "C" unsigned char test_core_ram(unsigned int address) {
    return SPU::GetWritableRAM()[address];
}
'''


class SpuResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        p = Path(cls.temp.name)
        flags = shlex.split(os.environ.get('SPU_RESET_CFLAGS', ''))
        source = Path(os.environ.get('SPU_RESET_SOURCE',
                                     ROOT/'pc/hal/spu_reset.c'))
        cls.binaries = []
        for real in (False, True):
            objects = []
            for name, path in [('reset', source),
                               ('harness', ROOT/'pc/tests/spu_reset_harness.c'),
                               *([('core', ROOT/'pc/hal/spu_psx.c')] if real else [])]:
                obj = p/(name + str(real) + '.o')
                run(['cc', '-std=c89', '-Wall', '-Wextra', '-Werror',
                     '-Wno-unused-parameter', '-O1', *flags,
                     '-Ipc/include/game', '-Ipc/include', '-Ipc/hal',
                     *(['-DREAL_CORE'] if real else []), '-c', str(path),
                     '-o', str(obj)], cwd=ROOT, timeout=30).require_ok()
                objects.append(str(obj))
            if real:
                helper = p/'helper.cpp'; helper.write_text(CORE_HELPER)
                obj = p/'helper.o'
                run(['c++', '-std=c++17', '-O1', *flags,
                     f'-I{spu_core.CORE}', f'-I{spu_core.GLUE}/stubs',
                     f'-I{spu_core.CORE}/upstream',
                     '-c', str(helper), '-o', str(obj)], cwd=ROOT).require_ok()
                objects += [str(obj), str(spu_core.archive())]
            binary = p/('actual' if real else 'ledger')
            run(['c++', *flags, *objects, '-o', str(binary)], cwd=ROOT).require_ok()
            cls.binaries.append(binary)

    def exercise(self, mode, real=False):
        # the settle ledger without SpuInit's other CPU (SPU_INIT_CPU, its
        # measured non-wait work, pc/hal/spu_reset.c)
        r = run([str(self.binaries[int(real)]), str(mode)],
                env=dict(os.environ, SPU_INIT_CPU='0'), timeout=10)
        r.require_ok()
        self.assertIn('SPU reset contract passed', r.stdout)

    def test_ready_reset_registers_pio_data_and_thirteen_actual_waits(self):
        self.exercise(0)

    def test_actual_core_selected_registers_and_silent_block_data(self):
        self.exercise(0, real=True)

    def test_cancel_before_pio_commit_does_not_finish(self):
        self.exercise(1)

    def test_cancel_after_pio_commit_does_not_enable_spu(self):
        self.exercise(2)

    def test_reentrant_reset_supersedes_old_sequence(self):
        self.exercise(3)

    def test_invalid_source_performs_no_device_work(self):
        self.exercise(4)
