# SPDX-License-Identifier: GPL-3.0-or-later
"""SDK Vcount follows delivered libetc IRQs, not physical VBlank edges."""
import os
import unittest
import test_irq
from pc_paths import decomp


class SdkIrqTests(unittest.TestCase):
    harness = 'pc/tests/sdk_irq_harness.c'
    # the harness includes the matched set_alarm.c from toki1-decomp
    extra_flags = ['-iquote', decomp('src/main')]
    extra_sources = [os.environ.get('GPU_QUEUE_SOURCE', 'pc/hal/gpu_queue.c')]
    setUpClass = classmethod(test_irq.IrqTests.setUpClass.__func__)
    exercise = test_irq.IrqTests.exercise

    def test_counter_waits_for_sdk_initialization(self):
        self.exercise(0)

    def test_bios_precedes_sdk_counter_and_callback_context(self):
        self.exercise(1)

    def test_full_width_hook_exchange_and_removal(self):
        self.exercise(2)

    def test_repeated_reset_preserves_counter_and_hook(self):
        self.exercise(3)

    def test_stop_and_restart_preserve_counter_but_clear_hook(self):
        self.exercise(4)

    def test_replacing_irq_zero_freezes_only_sdk_counter(self):
        self.exercise(5)

    def test_irq_removal_and_restore_deliver_one_pending_counter(self):
        self.exercise(6)

    def test_alarm_reads_sdk_counter_while_masked(self):
        self.exercise(7)

    def test_sdk_hook_overrun_can_reassert_after_ack(self):
        self.exercise(8)

    def test_recovery_releases_sdk_callback_context(self):
        self.exercise(9)

    def test_stop_discards_pending_before_restart(self):
        self.exercise(10)

    def test_repeated_reset_does_not_reinstall_removed_irq(self):
        self.exercise(11)

    def test_cpu_critical_gate_also_freezes_sdk_alarm_counter(self):
        self.exercise(12)

    def test_alarm_expiry_ignores_masked_physical_edges(self):
        self.exercise(13)

    def test_hook_can_remove_its_irq_and_restore_one_pending_delivery(self):
        self.exercise(14)

    def test_bios_mask_change_defers_sdk_handler_until_restore(self):
        self.exercise(15)

    def test_bios_ack_can_discard_the_edge_before_sdk_dispatch(self):
        self.exercise(16)
