# SPDX-License-Identifier: GPL-3.0-or-later
"""Busy readiness is independent of eager pixels and SDK dispatch callbacks."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from gpu_sdk_fixture import clear_objects
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP, decomp  # noqa: E402


class GpuTimingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'gpu-timing'
        flags = shlex.split(os.environ.get('GPU_TIMING_CFLAGS', ''))
        sdk = clear_objects(cls.temp.name, flags)
        display = Path(cls.temp.name) / 'display.o'
        run(['cc', '-std=gnu17', '-O1', '-Ipc/include/game',
             '-DPutDispEnv=gpu_sdk_put_disp_env',
             '-Dgraph_jtb=gpu_sdk_display_jtb', *flags, '-c',
             decomp('src/main/matched/PutDispEnv.c'), '-o', str(display)],
            cwd=ROOT, timeout=30).require_ok()
        gpu = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c')).resolve()
        queue = Path(os.environ.get('GPU_QUEUE_SOURCE',
                                    ROOT / 'pc/hal/gpu_queue.c')).resolve()
        clear = Path(os.environ.get('GPU_CLEAR_SOURCE',
                                    ROOT / 'pc/hal/gpu_clear.c')).resolve()
        sdl = shlex.split(run(['pkg-config', '--cflags', 'sdl2']).require_ok().stdout)
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1',
             '-ffunction-sections', '-fdata-sections',
             '-DGPU_SOURCE="' + str(gpu) + '"', '-Ipc/include/game',
             '-Ipc/include', '-Ipc/hal', *flags,
             'pc/tests/gpu_timing_harness.c', 'pc/hal/cd.c', str(queue),
             'pc/hal/gpu_reset.c', str(clear), 'pc/hal/gpu_vram.c',
             'pc/hal/ptr_validate.c', str(display), *sdk,
             *[str(DECOMP / 'src/main/matched' / (name + '.c')) for name in
               ['SetDrawEnv', 'get_cs', 'get_ce', 'get_ofs', 'get_mode',
                'get_tw', 'GetVideoMode', 'get_dx', '_getctl']],
             '-Wl,--gc-sections', '-lm', '-o', str(cls.binary), *sdl],
            cwd=ROOT, timeout=30).require_ok()

    # The harness asserts the queue's poll phases tick for tick as they were
    # first measured (181 / 150 / 42); the round-5 busy-wait phases
    # (gpu_queue.c DRAWSYNC_BUSY_POLL) and ResetGraph(0)'s CPU work
    # (gpu_reset.c RESETGRAPH0_CPU) are switched off here and checked by
    # test_busy_poll_phases below.
    LEGACY = {'DRAWSYNC_BUSY_POLL': '0', 'RESETGRAPH0_CPU': '0'}

    def exercise(self, mode, env=None):
        run([str(self.binary), str(mode)], timeout=15,
            env=dict(os.environ, **(self.LEGACY if env is None else env))).require_ok()

    def test_busy_poll_phases(self):
        # the round-5 measured phases, then their negative control
        self.exercise(25, env={'RESETGRAPH0_CPU': '0'})
        self.exercise(25, env=self.LEGACY)

    def test_callback_can_observe_gpu_busy_and_is_not_repeated_at_idle(self):
        self.exercise(0)

    def test_empty_fill_and_small_tile_finish_during_poll_entry(self):
        self.exercise(1)

    def test_tile_uses_pixel_work_instead_of_fill_cost(self):
        self.exercise(2)

    def test_back_to_back_clear_work_is_serialized_without_restart(self):
        self.exercise(3)

    def test_both_resets_cancel_busy_without_erasing_vram(self):
        self.exercise(4)

    def test_busy_sample_survives_completion_during_poll_return(self):
        self.exercise(5)

    def test_command_accumulation_preserves_fractional_cpu_ticks(self):
        self.exercise(6)

    def test_device_event_reset_replaces_the_old_deadline(self):
        self.exercise(7)

    def test_vblank_reset_and_nested_submission_progress_during_fence(self):
        self.exercise(8)

    def test_nested_device_fence_preserves_outer_cpu_work(self):
        self.exercise(9)

    def test_sdk_alarm_bounds_a_stalled_device_wait(self):
        self.exercise(10)

    def test_idle_sample_survives_submission_during_poll_return(self):
        self.exercise(11)

    def test_queue_disabled_clear_fences_before_the_next_worker(self):
        self.exercise(12)

    def test_zero_width_fill_keeps_row_work_without_touching_pixels(self):
        self.exercise(13)

    def test_queue_disabled_worker_runs_even_after_its_fence_times_out(self):
        self.exercise(14)

    def test_entry_is_interruptible_before_alarm_and_submission(self):
        self.exercise(15)

    def test_entry_reset_uses_the_new_submission_generation(self):
        self.exercise(16)

    def test_alarm_samples_after_a_vblank_crossed_in_entry(self):
        self.exercise(17)

    def test_blocking_alarm_stores_have_the_measured_entry_phase(self):
        self.exercise(18)

    def test_nonblocking_query_preserves_alarm_words(self):
        self.exercise(19)

    def test_busy_poll_counter_is_visible_between_iterations(self):
        self.exercise(20)

    def test_wait_uses_the_shared_deadline_and_leaves_count_on_expiry(self):
        self.exercise(21)

    def test_poll_limit_increments_the_shared_counter_before_expiry(self):
        self.exercise(22)

    def test_graph_resets_preserve_sdk_alarm_words(self):
        self.exercise(23)

    def test_alarm_keeps_its_sample_when_vblank_arrives_in_return_work(self):
        self.exercise(24)
