# SPDX-License-Identifier: GPL-3.0-or-later
"""TT sprite emission reads packed tables without eating adjacent flags."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402


class TtSpriteTests(unittest.TestCase):
    def test_nested_tables_flips_palettes_and_both_buffers(self):
        override = ROOT / 'pc/src/overlay/TT/anim_sprite_disp.c'
        source = override if override.exists() else DECOMP / 'src/overlay/TT/anim_sprite_disp.c'
        fixture = '''#define _GNU_SOURCE
#include <sys/mman.h>
#include <string.h>
#include "types.h"
''' + f'#include "{source}"\n' + '''
s32 *tt_disp_wk;
u8 *tt_db_buf;
s16 *tt_scale_buf;
static u8 *buffer;
static int count, bad;
void tt_sprite_ot_add_16x16(s32 slot, s32 packet, s32 scratch) {
    if ((u32)slot != (u32)(unsigned long)(buffer + 0x8014) ||
        (u32)packet != (u32)(unsigned long)(buffer + 8 + count * 48) ||
        (u32)scratch != (u32)(unsigned long)tt_scale_buf) bad = 1;
    count++;
}
int main(void) {
    u8 *memory;
    u8 *req;
    u8 *cel;
    u8 *p;
    u32 *entry;
    s16 pos[2];
    int index, flags, palette, scale, i;
    memory = mmap((void *)0x87000000UL, 0x20000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory == MAP_FAILED) return 1;
    for (index = 0; index < 2; index++) {
        for (flags = 0; flags < 4; flags++) {
            for (palette = 0; palette < 2; palette++) {
                for (scale = 0; scale < 2; scale++) {
                    memset(memory, 0, 0x20000);
                    buffer = memory + index * 32952;
                    tt_db_buf = memory;
                    tt_disp_wk = (s32 *)(memory + 0x11000);
                    tt_disp_wk[2] = index;
                    tt_scale_buf = (s16 *)(memory + 0x19000);
                    req = memory + 0x12000;
                    *(u32 *)req = (u32)(unsigned long)(memory + 0x13000);
                    req[4] = flags; req[5] = 1; req[6] = 1;
                    req[10] = palette; req[11] = 0x45; req[16] = scale;
                    *(s16 *)(req + 12) = 123;
                    *(s16 *)(req + 14) = 234;
                    *(u32 *)(memory + 0x13000) = 0xdeadbeefU;
                    *(u32 *)(memory + 0x13004) = (u32)(unsigned long)(memory + 0x14000);
                    entry = (u32 *)(memory + 0x14000);
                    entry[0] = (u32)(unsigned long)(memory + 0x15000);
                    entry[1] = (u32)(unsigned long)(memory + 0x16000);
                    entry[2] = (u32)(unsigned long)(memory + 0x17000);
                    entry[3] = (u32)(unsigned long)(memory + 0x18000);
                    entry[4] = 0x100;
                    *(u16 *)(memory + 0x16000) = 2;
                    *(u16 *)(memory + 0x16002) = 1;
                    for (i = 1; i <= 2; i++) {
                        cel = memory + 0x17000 + i * 8;
                        *(u16 *)cel = 0x49;
                        *(s16 *)(cel + 2) = 3;
                        *(s16 *)(cel + 4) = 4;
                        cel[6] = 0x23; cel[7] = 2;
                    }
                    memory[0x18001] = 0xd3;
                    *(u32 *)(buffer + 4) = (u32)(unsigned long)(buffer + 8);
                    pos[0] = 42; pos[1] = 24;
                    count = bad = 0;
                    anim_sprite_disp(pos, req, 3);
                    if (bad || count != 2 ||
                        *(u32 *)(buffer + 4) != (u32)(unsigned long)(buffer + 104) ||
                        tt_scale_buf[2] != 42 || tt_scale_buf[3] != 24 ||
                        tt_scale_buf[0] != (scale ? 123 : 0) ||
                        tt_scale_buf[1] != (scale ? 234 : 0)) return 2;
                    for (i = 0; i < 2; i++) {
                        p = buffer + 8 + i * 48;
                        if (*(s16 *)(p + 8) != ((flags & 2) ? 23 : 45) ||
                            *(s16 *)(p + 10) != ((flags & 1) ? 4 : 28) ||
                            p[12] != 144 || p[13] != 144 ||
                            *(u16 *)(p + 14) != (palette ? 0x205 : 0x183) ||
                            *(u16 *)(p + 22) != 19 ||
                            p[40] != (2 ^ flags) || p[41] || p[42] != 1 ||
                            p[43] != scale || p[44]) return 3;
                    }
                    *(u32 *)req = 0;
                    req[4] = 255;
                    anim_sprite_disp(pos, req, 3);
                    if (count != 2) return 4;
                }
            }
        }
    }
    return munmap(memory, 0x20000) != 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', '-I', str(ROOT / 'pc/include/game'),
                 str(path / 'test.c'), '-o', str(path / 'test')]).require_ok()
            run([str(path / 'test')]).require_ok()


if __name__ == '__main__':
    unittest.main()
