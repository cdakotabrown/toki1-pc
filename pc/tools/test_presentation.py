# SPDX-License-Identifier: GPL-3.0-or-later
"""Scanout refresh and SDL backbuffer lifetime regressions (no assets/window)."""
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
from pc_paths import decomp  # noqa: E402


class PresentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        cls.binary = directory / 'presentation'
        gfx = Path(os.environ.get('GFX_TEST_SOURCE',
                                  ROOT / 'pc/hal/gfx_test.c')).read_text()
        (directory / 'gfx_draw_fixture.h').write_text(
            gfx[gfx.index('void gfxtest_draw(void) {'):])
        flags = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2'],
                                timeout=10).require_ok().stdout)
        # Also exercise the old registration hook when testing pre-fix code.
        gpu = (ROOT / 'pc/hal/gpu.c').read_text()
        if 'void mark_textures_loaded_low_x(' in gpu:
            flags.append('-DCLEARIMAGE_LEGACY_TEXTURE_HOOK')
        clear = clear_objects(directory)
        sdk = directory / 'sdk-display.o'
        run(['gcc', '-std=gnu17', '-O1', '-fno-strict-aliasing', '-Ipc/include/game',
             '-DPutDispEnv=gpu_sdk_put_disp_env',
             '-Dgraph_jtb=gpu_sdk_display_jtb',
             '-c', decomp('src/main/matched/PutDispEnv.c'), '-o', str(sdk)],
            cwd=ROOT, timeout=30).require_ok()
        # Match the port's legacy callback-table compatibility flag.
        run(['gcc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections', '-Ipc/include',
             '-Ipc/include/game', '-I' + str(directory),
             'pc/tests/presentation_harness.c', 'pc/hal/gpu_scanout.c',
             'pc/hal/gpu_reset.c', 'pc/hal/gpu_queue.c', decomp('src/main/matched/GetVideoMode.c'),
             decomp('src/main/matched/get_dx.c'), str(sdk), *clear,
             'pc/hal/gpu_clear.c', 'pc/hal/gpu_vram.c',
             'pc/hal/ptr_validate.c',
             '-Wl,--gc-sections',
             '-Wl,--wrap=SDL_RenderPresent', '-Wl,--wrap=SDL_GetTicks',
             '-Wl,--wrap=SDL_Delay', '-lm',
             '-o', str(cls.binary), *flags], cwd=ROOT, timeout=30).require_ok()

    def check_case(self, name):
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        run([str(self.binary), name], env=env, timeout=10).require_ok()

    def test_every_present_refreshes_vram_and_discarded_backbuffer(self):
        self.check_case('repeat')

    def test_disabled_display_is_opaque_black_without_clearing_vram(self):
        self.check_case('mask')

    def test_small_display_has_initialized_black_padding(self):
        self.check_case('size')

    def test_display_origin_is_not_rewritten_by_observation(self):
        self.check_case('origin')

    def test_debug_presents_before_restoring_offsets_and_paces_host_only(self):
        self.check_case('debug')

    def test_debug_pacing_deducts_host_work(self):
        self.check_case('debug-work')

    def test_debug_turbo_presents_without_waiting(self):
        self.check_case('debug-turbo')

    def test_clear_buffer0_after_texture_registration_uses_requested_address(self):
        self.check_case('clear-buffer0')

    def test_clear_buffer1_after_texture_registration_uses_requested_address(self):
        self.check_case('clear-buffer1')

    def test_clear_texture_region_is_not_redirected_to_x960(self):
        self.check_case('clear-texture')

    def test_clear_is_not_suppressed_by_mainrom_registration(self):
        self.check_case('clear-mainrom')

    def test_display_environment_preserves_retail_layout_and_mode_defaults(self):
        self.check_case('dispenv')

    def test_rgb24_packed_bytes_and_return_to_bgr555(self):
        self.check_case('rgb24')

    def test_debug_draw_selects_bgr555_then_restores_rgb24(self):
        self.check_case('debug-rgb24')

    def test_debug_hud_is_opt_in_and_toggle_off_restores_scanout(self):
        self.check_case('hud')

    def test_native_loop_does_not_insert_a_nonretail_framebuffer_clear(self):
        main = (ROOT / 'pc/src/main.c').read_text()
        frame = main[main.index('                ot_buff_set();'):]
        frame = frame[:frame.index('                game_main();')]
        self.assertNotIn('ClearImage(', frame)


if __name__ == '__main__':
    unittest.main()
