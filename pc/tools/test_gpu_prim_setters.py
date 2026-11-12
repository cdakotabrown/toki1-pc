# SPDX-License-Identifier: GPL-3.0-or-later
"""The matched libgpu tag setters the PC links write only len and code.

src/main/pc-port/gpu_prim_init.c used to re-implement these (and won the
link over the matched C); it was retired 2026-10-31.  This keeps the old
packet-bytes check, now against the bodies that run.
"""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402
SETTERS = (('SetPolyG3', 6, 0x30), ('SetPolyFT4', 9, 0x2c), ('SetPolyG4', 8, 0x38),
           ('SetPolyGT4', 12, 0x3c), ('SetSprt8', 3, 0x74), ('SetSprt16', 3, 0x7c),
           ('SetSprt', 4, 0x64), ('SetTile', 3, 0x60), ('SetBlockFill', 3, 0x02),
           ('SetLineG2', 4, 0x50))


class GpuPrimSetterTests(unittest.TestCase):
    def test_packet_bytes(self):
        decls = ''.join('void %s(u32 *p);\n' % n for n, _, _ in SETTERS)
        table = ',\n'.join('    {%s, %d, 0x%02x}' % s for s in SETTERS)
        source = '#include <string.h>\n' + decls + '''
static const struct { void (*fn)(u32 *); u8 len, code; } T[] = {
''' + table + '''
};
int main(void) {
    u32 words[4];
    u8 *p = (u8 *)words;
    unsigned i, j;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        memset(words, 0x5a, sizeof(words));
        T[i].fn(words);
        for (j = 0; j < 16; j++)
            if (p[j] != (j == 3 ? T[i].len : j == 7 ? T[i].code : 0x5a))
                return 1 + (int)i;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            srcs = [str(DECOMP / ('src/main/matched/%s.c' % n)) for n, _, _ in SETTERS]
            run(['gcc', '-std=gnu17', '-O2', '-w',
                 '-include', str(ROOT / 'pc/include/game/types.h'),
                 '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
                 '-I', str(DECOMP / 'include'), str(path / 'test.c')] + srcs +
                ['-o', str(path / 'test')]).require_ok()
            self.assertEqual(run([str(path / 'test')]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
