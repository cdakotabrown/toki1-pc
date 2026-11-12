# SPDX-License-Identifier: GPL-3.0-or-later
"""Native SPRT discards zero texels, not opaque black (0x8000) texels."""
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "@GPU_SOURCE@"

int main(int argc, char **argv) {
    u16 expected;
    int blend;

    (void)argc;
    expected = 0x1234;
    blend = !strcmp(argv[1], "blend");
    g_vram[10][10] = expected;
    g_vram[256][0] = 0;
    if (!strcmp(argv[1], "black")) {
        g_vram[256][0] = expected = 0x8000;
    } else if (!strcmp(argv[1], "color")) {
        g_vram[256][0] = expected = 0x001f;
    }
    draw_sprt_blend(10, 10, 1, 1, 0, 0, 0, 0x110,
                    128, 128, 128, blend, 0);
    if (g_vram[256][0] != (!strcmp(argv[1], "black") ? 0x8000 :
                          !strcmp(argv[1], "color") ? 0x001f : 0)) return 2;
    return g_vram[10][10] == expected ? 0 : 1;
}
'''


class SpriteTransparencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = directory / 'sprite.c'
        source.write_text(HARNESS.replace('@GPU_SOURCE@',
                          str(ROOT / 'pc/hal/gpu.c')))
        cls.binary = directory / 'sprite'
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                timeout=10).require_ok().stdout)
        # The helper exists in the final source. The initial negative control
        # can also run before the literal sampler is introduced.
        texture = ROOT / 'pc/hal/gpu_texture.c'
        helpers = [str(texture)] if texture.exists() else []
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections', '-Ipc/include',
             '-Ipc/include/game', '-Ipc/hal', str(source), *helpers,
             '-Wl,--gc-sections', '-o', str(cls.binary), *flags],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        run([str(self.binary), case], timeout=10).require_ok()

    def test_zero_texel_preserves_background_for_opaque_sprite(self):
        self.check_case('solid')

    def test_zero_texel_preserves_background_for_semitransparent_sprite(self):
        self.check_case('blend')

    def test_stp_black_is_not_mistaken_for_transparency(self):
        self.check_case('black')

    def test_nonzero_texel_is_drawn(self):
        self.check_case('color')


if __name__ == '__main__':
    unittest.main()
