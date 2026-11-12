# SPDX-License-Identifier: GPL-3.0-or-later
"""VBlank latching through the real clock, system handler and BIOS events."""
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


class IrqTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'irq'
        source = Path(os.environ.get('IRQ_SOURCE', ROOT / 'pc/hal/irq.c'))
        system = Path(os.environ.get('SYSTEM_SOURCE', ROOT / 'pc/hal/system.c'))
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2']).require_ok().stdout)
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1',
             '-ffunction-sections', '-fdata-sections', '-Ipc/include/game',
             '-Ipc/include', '-Ipc/hal', *flags,
             *shlex.split(os.environ.get('IRQ_CFLAGS', '')),
             *getattr(cls, 'extra_flags', []),
             getattr(cls, 'harness', 'pc/tests/irq_harness.c'),
             str(source), str(system),
             os.environ.get('SDK_IRQ_SOURCE', 'pc/hal/sdk_irq.c'),
             '-DD_800E8CFC=hal_sdk_vsync_hook', '-Dlibapi_vsync_callback=hal_sdk_vsync_hook',
             decomp('src/main/matched/trapIntrVSync.c'),
             decomp('src/main/matched/CheckCallback.c'),
             *getattr(cls, 'extra_sources', []),
             'pc/hal/cd.c', 'src/main/pc-port/bios_trampolines.c',
             'pc/hal/bios_printf.c', 'pc/hal/ptr_validate.c',
             '-Wl,--gc-sections', '-lm', '-o', str(cls.binary)],
            cwd=ROOT, timeout=30).require_ok()

    def exercise(self, mode):
        # The interrupt mechanics are pinned in whole-tick frames with the
        # boot / BIOS CPU models off (their own tests: test_cd_clock
        # ClockModelTests).
        env = dict(os.environ, VBLANK_EXACT_PERIOD='0', BOOT_CPU_COST='0',
                   BIOS_CPU_COST='0')
        run([str(self.binary), str(mode)], env=env, timeout=10).require_ok()

    def test_unmasked_physical_edges_deliver_callbacks(self):
        self.exercise(0)

    def test_masked_edges_run_kernel_events_and_latch_one_libetc_delivery(self):
        self.exercise(1)

    def test_other_mask_bits_do_not_enable_vblank_or_stop_devices(self):
        self.exercise(2)

    def test_acknowledgment_discards_masked_pending_work(self):
        self.exercise(3)

    def test_critical_section_skips_kernel_events_and_libetc_delivery(self):
        self.exercise(4)

    def test_mask_restore_does_not_override_cpu_disable(self):
        self.exercise(5)

    def test_cpu_enable_does_not_override_hardware_mask(self):
        self.exercise(6)

    def test_mask_exchange_truncates_to_the_eleven_hardware_bits(self):
        self.exercise(7)

    def test_handler_overrun_coalesces_before_late_ack_without_recursion(self):
        self.exercise(8)

    def test_callback_mask_changes_survive_successful_dispatch(self):
        self.exercise(9)

    def test_reassertion_after_ack_runs_after_handler_return(self):
        self.exercise(10)

    def test_ack_can_discard_a_repeated_latch_without_delivery(self):
        self.exercise(11)

    def test_recovery_abandons_irq_and_cpu_work_without_stale_gates(self):
        self.exercise(12)

    def test_explicit_software_event_delivery_remains_synchronous(self):
        self.exercise(13)

    def test_mask_changes_preserve_the_existing_pending_latch(self):
        self.exercise(14)

    def test_cpu_work_across_a_delivered_edge_loses_the_exception_time(self):
        self.exercise(15)

    def test_kernel_only_exception_and_latched_restore_are_not_charged(self):
        self.exercise(16)

    def test_critical_section_cpu_work_is_not_charged(self):
        self.exercise(17)

    def test_vsync_wait_is_absolute(self):
        self.exercise(18)
