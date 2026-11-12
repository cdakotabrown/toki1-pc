# SPDX-License-Identifier: GPL-3.0-or-later
"""Freeze existing clamped stereo averaging before correcting playback ABI."""
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class SpuVolumeTests(unittest.TestCase):
    def test_existing_average_rounding_and_clamp_boundaries(self):
        source = '#include "' + str(ROOT / 'pc/hal/spu.c') + '"\n' + r'''
int main(void) {
    static const s16 cases[][3] = {
        {0, 0, 0}, {0, 1, 0}, {1, 2, 1}, {100, 126, 113},
        {112, 112, 112}, {126, 127, 126}, {127, 128, 127},
        {32767, 32767, 127}, {-32768, -32768, 0},
        {-32768, 32767, 0}, {-1, 0, 0}
    };
    unsigned int i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (spu_average_volume(cases[i][0], cases[i][1]) != cases[i][2])
            return 1;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            fixture = directory / 'volume.c'
            binary = directory / 'volume'
            fixture.write_text(source)
            flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                    timeout=10).require_ok().stdout)
            run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
                 '-fsanitize=undefined', '-fno-sanitize-recover=undefined',
                 '-ffunction-sections', '-fdata-sections',
                 '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', str(fixture),
                 '-Wl,--gc-sections', '-o', str(binary), '-lm', *flags],
                cwd=ROOT, timeout=30).require_ok()
            run([str(binary)], timeout=10).require_ok()


if __name__ == '__main__':
    unittest.main()
