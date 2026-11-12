# SPDX-License-Identifier: GPL-3.0-or-later
"""The CD queue override must call the BIOS printf thunk, not the host libc.

Retail cdq_command_process prints " CDQ.EXE %x\\n" through kernel A(0x3F)
printf before every queued read; that call is ~21,000 ticks of PS1 CPU time
and decides on which frame a load completes.  pc-port overrides are compiled
without the force-included sony_bios_alias.h, so the override must map the
Sony libc names itself.  Before 2026-10-30 it called the host printf, which
costs no emulated time, and boot_data_load completed one frame early.
"""
from pathlib import Path
import os
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402
SOURCE = Path(os.environ.get('CDQ_SOURCE', ROOT / 'src/main/pc-port/cdq_command_process.c'))
HOST_LIBC = {'printf', 'puts', 'putchar', 'vprintf'}


def undefined_symbols(source):
    with tempfile.TemporaryDirectory() as d:
        obj = Path(d) / 'cdq.o'
        run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-funsigned-char', '-fno-strict-aliasing',
             '-O2', '-D_GNU_SOURCE', '-DPC_PORT', '-include', str(ROOT / 'pc/include/game/types.h'),
             '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'), '-I', str(DECOMP / 'include'),
             '-c', str(source), '-o', str(obj)], cwd=str(ROOT)).require_ok()
        out = run(['nm', '-u', str(obj)])
        out.require_ok()
        return {line.split()[-1] for line in out.stdout.splitlines() if line.strip()}


class CdqBiosCallTests(unittest.TestCase):
    def test_printf_goes_through_bios_trampoline(self):
        syms = undefined_symbols(SOURCE)
        self.assertIn('func_800B4850', syms)
        self.assertFalse(syms & HOST_LIBC, f'host libc output calls: {sorted(syms & HOST_LIBC)}')


OVERRIDE_DIR = Path(os.environ.get('PRINTF_OVERRIDE_DIR', ROOT / 'src/main/pc-port'))
# Other pc-port overrides whose retail body calls printf (on error branches:
# unsupported TMD codes, an out-of-range BG cell or pan index, a bad MDEC
# header).  Off the timed boot path, but the retail call is the kernel's.
PRINTF_OVERRIDES = ('GsLinkObject4', 'GsSortObject4', '_make_packet', 'bg_set', 'decdct')


class OverridePrintfTests(unittest.TestCase):
    def test_override_printf_goes_through_bios_trampoline(self):
        for name in PRINTF_OVERRIDES:
            with self.subTest(override=name):
                syms = undefined_symbols(OVERRIDE_DIR / (name + '.c'))
                self.assertIn('func_800B4850', syms)
                self.assertFalse(syms & HOST_LIBC, f'host libc output calls: {sorted(syms & HOST_LIBC)}')


if __name__ == '__main__':
    unittest.main()
