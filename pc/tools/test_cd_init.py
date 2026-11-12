# SPDX-License-Identifier: GPL-3.0-or-later
"""CD initialization uses real commands and the unchanged retry wrapper."""
import os
from pathlib import Path
import re
import shlex
import tempfile
import unittest
from safe_process import run
ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402


class CdInitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        p = Path(cls.temp.name)
        flags = shlex.split(os.environ.get('CD_INIT_CFLAGS', ''))
        sdk = p / 'sdk.o'
        run(['cc', '-std=gnu17', '-O1', '-Ipc/include/game', *flags,
             '-Dprintf=cd_init_error_printf',
             '-c', decomp('src/main/matched/CdInit.c'), '-o', str(sdk)],
            cwd=ROOT, timeout=30).require_ok()
        cls.binaries = {}
        for name, defines in [('normal', []),
                              ('missing', ['-DBOOT_PATH="other"']),
                              ('invalid', ['-DBOOT_SIZE=2048']),
                              ('overflow', ['-DBOOT_LBA=0xfffffff0u'])]:
            binary = p / name
            source = Path(os.environ.get('CD_SOURCE', ROOT / 'pc/hal/cd.c'))
            run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w',
                 '-O1', '-ffunction-sections', '-fdata-sections',
                 '-Ipc/include/game', '-Ipc/include', '-Ipc/hal', *flags,
                 '-DCD_SOURCE="' + str(source.resolve()) + '"', *defines,
                 'pc/tests/cd_init_harness.c', str(sdk),
                 *[decomp('src/main/matched/' + x + '.c') for x in
                   ['def_cbsync', 'def_cbready', 'def_cbread']],
                 '-Wl,--gc-sections', '-Wl,--wrap=CD_init', '-lm',
                 '-o', str(binary)], cwd=ROOT, timeout=30).require_ok()
            cls.binaries[name] = binary

    def exercise(self, mode, binary='normal'):
        result = run([str(self.binaries[binary]), str(mode)],
                     env=dict(os.environ, PC_DEBUG='cd'), timeout=15).require_ok()
        if mode not in [6, 7, 21, 22]:
            interval = result.stdout.split('BEGIN_INIT\n', 1)[1].split('END_INIT')[0]
            self.assertEqual(re.findall(r'issue cmd (\d+)', interval), ['1', '10', '12'])
            self.assertLess(interval.index('command 10 complete'),
                            interval.index('issue cmd 12'))

    # Retail coordinates for the same five calls, from the authored probe
    # (docs/pc-port/CD-INIT-RESULTS.json): elapsed ticks and the SDK Vblank
    # crossings around each CdInit.  The probe runs as the boot executable, so
    # its first count is 1 where the native harness starts at 0; only the
    # deltas are comparable, and the cases are replayed with the same
    # mode/Setloc calls between them.
    REFERENCE = [
        ('boot', -1, -1, 27297303, 48, 1),
        ('warm', -1, -1, 4181044, 8, 1),
        ('single_near', 0, 100, 4180927, 7, 1),
        ('double_near', 128, 100, 25612035, 45, 1),
        ('double_far', 128, 500, 27294613, 49, 1),
    ]

    def coordinates(self):
        result = run([str(self.binaries['normal']), '30'],
                     env=dict(os.environ, PC_DEBUG='cd'), timeout=15).require_ok()
        rows = []
        for line in result.stdout.splitlines():
            if not line.startswith('CASE '):
                continue
            f = line.split()
            rows.append((f[1], int(f[2]), int(f[3]), int(f[4]),
                         int(f[6]) - int(f[5]), int(f[7])))
        return rows

    def test_reference_coordinates_agree_within_a_tenth_of_a_frame(self):
        rows = self.coordinates()
        self.assertEqual([row[0] for row in rows], [r[0] for r in self.REFERENCE])
        for native, retail in zip(rows, self.REFERENCE):
            self.assertEqual(native[1:3], retail[1:3], native[0])
            # Every gap is under 46,200 ticks since libcd's per-command CPU
            # work (CD_cw's register write, the acknowledge interrupt and
            # CD_cw's exit; pc/hal/cd.c) is charged; it was 44,166-55,028
            # before.  The rest is still unaccounted CPU work.
            self.assertLess(abs(native[3] - retail[3]), 47000, native[0])
            # The VBlank crossings of a call that spans N.15-N.25 frames
            # depend on where in a frame it starts.  The harness's start
            # phase comes from its own preceding Setmode/SeekL, not from the
            # retail probe's, so a crossing count can differ by one; it must
            # be one of the two counts the native duration allows.
            self.assertLess(abs(native[4] - retail[4]), 2, native[0])
            self.assertIn(native[4] - native[3] // 566204, (0, 1), native[0])
            self.assertEqual(native[5], retail[5], native[0])

    def test_coordinate_sequence_is_deterministic(self):
        self.assertEqual(self.coordinates(), self.coordinates())

    def test_controller_attempt_waits_for_both_init_responses(self): self.exercise(0)
    def test_public_wrapper_returns_success_and_native_default_callbacks(self): self.exercise(1)
    def test_repeated_init_executes_the_protocol_again(self): self.exercise(2)
    def test_boot_extent_handoff_preserves_double_speed_seek_cost(self): self.exercise(3)
    def test_near_double_speed_context_changes_the_reset_cost(self): self.exercise(4)
    def test_single_speed_context_uses_the_reset_minimum(self): self.exercise(5)
    def test_handoff_does_not_overwrite_a_running_clock(self): self.exercise(6)
    def test_handoff_is_not_applied_twice(self): self.exercise(7)
    def test_missing_metadata_does_not_fabricate_boot_state(self): self.exercise(8, 'missing')
    def test_invalid_extent_does_not_fabricate_boot_state(self): self.exercise(8, 'invalid')
    def test_overflow_extent_is_rejected(self): self.exercise(8, 'overflow')
    def test_reset_stops_an_active_stream(self): self.exercise(9)
    def test_model_drains_a_pending_response_before_new_commands(self): self.exercise(10)
    def test_internal_attempt_clears_only_its_owned_user_callbacks(self): self.exercise(11)
    def test_default_callbacks_deliver_the_original_bios_events(self): self.exercise(12)
    def test_retry_wrapper_succeeds_on_fifth_attempt(self): self.exercise(20)
    def test_retry_wrapper_stops_after_five_failures(self): self.exercise(21)
    def test_retry_wrapper_stops_on_first_success(self): self.exercise(22)
    def test_retry_wrapper_stops_on_intermediate_success(self): self.exercise(23)
