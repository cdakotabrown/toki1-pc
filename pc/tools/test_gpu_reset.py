# SPDX-License-Identifier: GPL-3.0-or-later
"""ResetGraph effects against actual system.c/gpu.c and byte-layout canaries."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run
from gpu_sdk_fixture import clear_objects

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP, decomp  # noqa: E402


class GpuResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'gpu-reset'
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2']).require_ok().stdout)
        source = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c')).resolve()
        system = Path(os.environ.get('SYSTEM_SOURCE', ROOT / 'pc/hal/system.c')).resolve()
        reset = Path(os.environ.get('GPU_RESET_SOURCE',
                                    ROOT / 'pc/hal/gpu_reset.c')).resolve()
        queue = Path(os.environ.get('GPU_QUEUE_SOURCE',
                                    ROOT / 'pc/hal/gpu_queue.c')).resolve()
        gpu_cpu = Path(os.environ.get('GPU_CPU_SOURCE',
                                      ROOT / 'pc/hal/gpu_cpu.c')).resolve()
        clear = clear_objects(cls.temp.name, shlex.split(
            os.environ.get('GPU_RESET_CFLAGS', '')))
        sdk = Path(cls.temp.name) / 'sdk-display.o'
        run(['cc', '-std=gnu17', '-O1', '-fno-strict-aliasing', '-Ipc/include/game',
             '-DPutDispEnv=gpu_sdk_put_disp_env',
             '-Dgraph_jtb=gpu_sdk_display_jtb',
             *shlex.split(os.environ.get('GPU_RESET_CFLAGS', '')),
             '-c', decomp('src/main/matched/PutDispEnv.c'), '-o', str(sdk)],
            cwd=ROOT, timeout=30).require_ok()
        # The harness defines the mdfn_* entry points as counting mocks, so
        # gpu.c is compiled with the backend switch present: without
        # MDFN_GPU_BACKEND (041f4a8f96) mdfn_gpu_enabled() is a constant 0 and
        # the backend=1 cases would test nothing.
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1',
             '-ffunction-sections', '-fdata-sections', '-DMDFN_GPU_BACKEND=1',
             '-DGPU_SOURCE="' + str(source) + '"',
             '-Ipc/include/game', '-Ipc/include', '-Ipc/hal',
             *shlex.split(os.environ.get('GPU_RESET_CFLAGS', '')),
             'pc/tests/gpu_reset_harness.c', str(system), str(reset), str(queue),
             *([str(gpu_cpu)] if gpu_cpu.exists() else []),
             'pc/hal/gpu_vram.c', str(Path(os.environ.get('GPU_CLEAR_SOURCE',
                 ROOT / 'pc/hal/gpu_clear.c'))),
             'pc/hal/ptr_validate.c', str(sdk), *clear,
             *[str(DECOMP / 'src/main/matched' / (name + '.c')) for name in
               ['SetDrawEnv', 'get_cs', 'get_ce', 'get_ofs', 'get_mode',
                'get_tw', 'GetVideoMode', 'get_dx', '_getctl']],
             '-Wl,--gc-sections', '-o', str(cls.binary), *flags],
            cwd=ROOT, timeout=30).require_ok()

    def exercise(self, mode, backend=0):
        run([str(self.binary), str(mode), str(backend)], timeout=10).require_ok()

    def test_full_reset_exact_sdk_bytes_preserves_vram_and_ot(self):
        self.exercise(0)

    def test_soft_modes_preserve_device_state_and_clear_only_queue_functions(self):
        for mode in [1, 2, 3, -1]:
            with self.subTest(mode=mode):
                self.exercise(mode)

    def test_type_zero_full_reset_ignores_wide_variant_flag(self):
        self.exercise(8)

    def test_mednafen_receives_full_reset_state(self):
        self.exercise(0, 1)

    def test_mednafen_soft_reset_preserves_state(self):
        self.exercise(1, 1)

    def test_packet_and_drawenv_dither_change_real_native_pixels(self):
        self.exercise('dither')

    def test_sdk_calls_spend_retail_cpu_time_and_mask_where_libgpu_does(self):
        # vblank_function's post-VSync calls: ResetGraph(1) 1,001 (247
        # masked), PutDispEnv 942, SetDispMask 100, PutDrawEnv 1,443 (257
        # masked), DrawOTag 1,404 (1,167 masked).
        self.exercise('sdk-cpu')

    def test_environment_defaults_layout_packets_and_complete_cache_copies(self):
        self.exercise('cache')

    def test_aligned_background_executes_the_sdk_fill_packet(self):
        self.exercise('clear-aligned')

    def test_unaligned_background_executes_the_sdk_tile_packet(self):
        self.exercise('clear-tile')

    def test_background_fill_wraps_and_ignores_drawing_area(self):
        self.exercise('clear-fill-edge')

    def test_background_tile_obeys_wrapped_invalid_drawing_area(self):
        self.exercise('clear-tile-edge')

    def test_tile_position_and_offset_use_signed_eleven_bit_coordinates(self):
        self.exercise('clear-wrapped-offset')

    def test_tile_height_masks_to_zero_instead_of_drawing_512_rows(self):
        self.exercise('clear-zero-hardware-height')

    def test_display_shadow_readback_and_conditional_sdk_cache_updates(self):
        self.exercise('display-0')

    def test_display_width_modes_pal_interlace_rgb24_and_reverse(self):
        self.exercise('display-1')

    def test_display_range_clamps_and_type_specific_origin(self):
        self.exercise('display-2')

    def test_gpu_completion_follows_real_clear_and_preserves_sdk_storage(self):
        self.exercise('queue-0')

    def test_gpu_nested_work_and_sync_busy_results(self):
        self.exercise('queue-1')

    def test_gpu_callback_can_unregister_and_submit_more_work(self):
        self.exercise('queue-2')

    def test_gpu_reset_invalidates_old_work_and_soft_reset_keeps_callback(self):
        self.exercise('queue-3')

    def test_gpu_queue_disabled_work_retains_activity_without_notification(self):
        self.exercise('queue-4')

    def test_gpu_drawenv_background_notifies_after_the_whole_submission(self):
        self.exercise('queue-5')

    def test_gpu_completion_uses_current_callback_and_honors_registration_clear(self):
        self.exercise('queue-6')

    def test_gpu_self_submitting_callback_defers_notification_without_recursion(self):
        self.exercise('queue-7')

    def test_gpu_callback_can_reset_register_and_submit_a_new_generation(self):
        self.exercise('queue-8')

    def test_sdk_clear_fill_tile_wrap_signed_and_empty_geometry(self):
        for case in range(22):
            with self.subTest(case=case):
                self.exercise('sdk-clear-'+str(case))

    def test_sdk_clear_rect_diagnostics_precede_activity_and_packet_writes(self):
        self.exercise('sdk-clear-22')
        self.exercise('sdk-clear-23')

    def test_sdk_clear_sign_extended_rect_pointer(self):
        flags = shlex.split(os.environ.get('GPU_RESET_CFLAGS', ''))
        if any(f.startswith('-fsanitize=') and
               'address' in f.split('=')[1].split(',') for f in flags):
            self.skipTest('ASan reserves the fixed PS1 window; separately '
                          'verified without destructive MAP_FIXED')
        self.exercise('sdk-clear-24')

    def test_sdk_clear_invalid_rects_do_not_submit(self):
        self.exercise('sdk-clear-27')

    def test_device_area_info_survives_soft_reset_and_clears_on_full_reset(self):
        self.exercise('sdk-clear-25')

    def test_sdk_clear_restores_all_twenty_drawing_area_bits(self):
        self.exercise('sdk-clear-26')
