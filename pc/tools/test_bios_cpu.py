# SPDX-License-Identifier: GPL-3.0-or-later
"""BIOS printf/store observations and CPU budgets through actual clock events."""
import os
from pathlib import Path
import re
import shlex
import tempfile
import unittest
from safe_process import run
ROOT = Path(__file__).resolve().parents[2]


class BiosCpuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = []
        for real in (False, True):
            binary = Path(cls.temp.name) / ('clock' if real else 'format')
            cmd = ['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w',
                   '-O1', '-ffunction-sections', '-fdata-sections',
                   '-Ipc/include/game', '-Ipc/include', '-Ipc/hal',
                   *shlex.split(os.environ.get('BIOS_CPU_CFLAGS', '')),
                   'pc/tests/bios_cpu_harness.c',
                   str(Path(os.environ.get('BIOS_TRAMPOLINES_SOURCE',
                       ROOT / 'src/main/pc-port/bios_trampolines.c'))),
                   str(Path(os.environ.get('BIOS_PRINTF_SOURCE',
                                           ROOT / 'pc/hal/bios_printf.c'))),
                   'pc/hal/ptr_validate.c']
            if real:
                cmd += ['-DREAL_CLOCK', str(Path(os.environ.get(
                    'CD_SOURCE', ROOT / 'pc/hal/cd.c')))]
            run(cmd + ['-Wl,--gc-sections', '-lm', '-o', str(binary)],
                cwd=ROOT, timeout=30).require_ok()
            cls.binaries.append(binary)

    def exercise(self, mode, real=False, extra=()):
        r = run([str(self.binaries[int(real)]), str(mode), *extra], timeout=15)
        r.require_ok()
        m = re.search(r'result=(-?\d+) ticks=(\d+)', r.stdout)
        self.assertIsNotNone(m, r.stdout)
        output = r.stdout[:m.start()]
        return r, int(m[1]), int(m[2]), output

    def test_bios_outputs_return_counts_and_independent_rom_timing_vectors(self):
        # Golden observations from authored CD-booted SCPH-5500 probes.
        vectors = [(0, 'AAAA', 0, 7359), (1, '\n', 0, 3968),
                   (2, '00000000', 8, 17315), (3, 'ffffffff', 8, 19855),
                   (4, '-2147483648', 11, 21749), (5, '-001', 4, 12802),
                   (6, '4294967295', 10, 20507), (7, 'AAAA', 4, 11004),
                   (8, '\n', 1, 6956), (9, '%', 1, 4648),
                   (10, 'tag=12345678,89abcdef\n', 16, 45786)]
        for mode, text, result, observed in vectors:
            with self.subTest(mode=mode):
                r, value, ticks, output = self.exercise(mode)
                self.assertEqual(value, result)
                self.assertEqual(output, text)
                self.assertLessEqual(abs(ticks - observed), 100)

    def test_literals_do_not_contribute_to_formatted_return_count(self):
        r, result, ticks, output = self.exercise(11)
        self.assertEqual(result, 4)
        self.assertEqual(output, 'v=-7:ok\n')

    def test_unsupported_or_unsafe_requests_reject_without_partial_output(self):
        for mode in range(12, 19):
            with self.subTest(mode=mode):
                r, result, ticks, output = self.exercise(mode)
                self.assertEqual((result, ticks), (-1, 0))
                self.assertIn('[BIOS printf] rejected:', r.stdout)
                self.assertNotIn('discard', r.stdout)

    def test_stack_format_and_string_pointers_keep_their_upper_bits(self):
        r, result, ticks, output = self.exercise(19)
        self.assertEqual(result, 5)
        self.assertEqual(output, 'upper')

    def test_bzero_exact_budget_and_byte_store_visibility(self):
        for watch, visible in [('283', 0), ('284', 1), ('415', 1), ('416', 2)]:
            with self.subTest(watch=watch):
                r, result, ticks, output = self.exercise(20, extra=(watch,))
                self.assertEqual((result, ticks), (1, 713))
                self.assertIn('visible=' + str(visible), r.stdout)

    def test_empty_bzero_runs_the_measured_guard_path(self):
        r, result, ticks, output = self.exercise(21)
        self.assertEqual((result, ticks), (0, 223))

    def test_printf_retains_cpu_work_interrupted_by_vblank_printf(self):
        self.exercise(0, real=True)

    def test_printf_retains_cpu_work_interrupted_by_device_callback(self):
        self.exercise(1, real=True)

    def test_vblank_wait_keeps_absolute_device_deadline_semantics(self):
        self.exercise(2, real=True)

    def test_abandoned_cpu_stack_is_cleared_before_recovery(self):
        self.exercise(3, real=True)

    def test_callback_wait_with_nested_irq_counts_occupancy_once(self):
        self.exercise(4, real=True)
