# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise TT's retail draw buffers at high, four-byte PS1 addresses."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402

PRELUDE = '''#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
static int calls, bad;
static void *packets[4];
void tt_qstr_disp(void *entry);
void SetDrawMode(void *packet, s32 a, s32 b, s32 page, s32 window) {
    u32 *p;
    p = packet;
    if (a || window || calls >= 4) { bad = 1; return; }
    packets[calls++] = packet;
    p[0] = (p[0] & 0xffffffU) | 0x02000000U;
    p[1] = page;
    p[2] = b;
}
'''

COMMON = '''
static u8 *memory;
static u8 *buffer;
void tt_qstr_disp(void *entry) {
    (void)entry;
    /* Text can allocate packets before the final DR_MODE. */
    *(u32 *)(buffer + 4) += 12;
}
int main(void) {
    int index;
    u8 *prim;
    u32 *ot;
    if (sizeof(DB) != 32952 || offsetof(DB, ot) != 0x8000)
        return 10;
    memory = mmap((void *)0x85000000UL, 0x20000,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory == MAP_FAILED) return 11;
    for (index = 0; index < 2; index++) {
        memset(memory, 0, 0x20000);
        buffer = memory + index * 32952;
        prim = buffer + 8;
        ot = (u32 *)(buffer + 0x8000);
        *(u32 *)(buffer + 4) = (u32)(unsigned long)prim;
        *(u32 *)(buffer + 0x8000 - 4) = 0xdeadbeefU;
        tt_db_buf = (DB *)memory;
        tt_disp_wk = (TTDisp *)(memory + 0x11000);
        tt_disp_wk->buf = index;
        calls = bad = 0;
        BODY
        if (bad || *(u32 *)(buffer + 0x8000 - 4) != 0xdeadbeefU)
            return 12;
        /* Neither the adjacent cursor word nor the other DB was touched. */
        if (*(u32 *)buffer || *(u32 *)(memory + (1-index)*32952 + 4))
            return 13;
    }
    return munmap(memory, 0x20000) != 0;
}
'''


class TtRenderTests(unittest.TestCase):
    def check_renderer(self, name, body):
        source = ROOT / 'pc/src/overlay/TT' / f'{name}.c'
        if not source.exists():
            source = DECOMP / 'src/overlay/TT' / f'{name}.c'
        globals_ = {'tt_text_disp': 'Ent *tt_text_buf;',
                    'tt_recs_disp': 'Rec *tt_recs_buf;',
                    'tt_board_disp': 'TTState *tt_step_wk;',
                    'tt_rec_disp': 'Rec *tt_rec_pool;',
                    'tt_sprite_disp': 'Rec *tt_player_shot_buf;',
                    'tt_rec_sprite_disp': 'u8 *tt_step_wk;'}
        fixture = (PRELUDE + f'#include "{source}"\n' +
                   'DB *tt_db_buf; TTDisp *tt_disp_wk;\n' + globals_[name] +
                   COMMON.replace('BODY', '{\n' + body + '\n}'))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', '-I', str(ROOT / 'pc/include/game'),
                 '-I', str(ROOT / 'pc/include'), str(path / 'test.c'),
                 '-o', str(path / 'test')]).require_ok()
            result = run([str(path / 'test')])
            self.assertTrue(result.ok, f'{name}: exit {result.returncode}\n{result.stdout}')

    def test_text_buffer_stride_and_cursor(self):
        self.check_renderer('tt_text_disp', '''
        tt_text_buf = (Ent *)(memory + 0x12000);
        tt_text_buf[0].flag = 1;
        tt_text_buf[1].flag = 1;
        tt_text_buf[1].bits = 1;
        tt_text_buf[127].flag = 1;
        ot[2] = 0x00ffffffU;
        tt_text_disp();
        if (calls != 1 || packets[0] != prim + 24 ||
            *(u32 *)(buffer + 4) != (u32)(unsigned long)(prim + 36) ||
            ot[2] != ((u32)(unsigned long)(prim + 24) & 0xffffffU) ||
            *(u32 *)(prim + 24) != 0x02ffffffU ||
            *(u32 *)(prim + 28) != 10) return 20;
''')

    def test_record_packets_and_ot_links(self):
        self.check_renderer('tt_recs_disp', '''
        tt_recs_buf = (Rec *)(memory + 0x12000);
        tt_recs_buf[0].flag = tt_recs_buf[7].flag = 1;
        tt_recs_buf[0].pri = tt_recs_buf[7].pri = 3;
        tt_recs_buf[0].x = 42;
        tt_recs_buf[0].y = 24;
        tt_recs_buf[0].u = 7;
        tt_recs_buf[0].v = 9;
        tt_recs_buf[0].clut = 123;
        tt_recs_buf[0].tpage = 28;
        ot[5] = 0x00ffffffU;
        tt_recs_disp();
        if (sizeof(Rec) != 88 || calls != 2 ||
            packets[0] != prim + 20 || packets[1] != prim + 52 ||
            *(u32 *)(buffer + 4) != (u32)(unsigned long)(prim + 64) ||
            ot[5] != ((u32)(unsigned long)(prim + 52) & 0xffffffU) ||
            *(u32 *)prim != 0x04ffffffU || prim[7] != 101 ||
            *(s16 *)(prim + 8) != 34 || *(s16 *)(prim + 10) != 16 ||
            prim[12] != 7 || prim[13] != 9 || *(u16 *)(prim + 14) != 123 ||
            *(u32 *)(prim + 24) != 28) return 30;
''')

    def test_other_record_passes_keep_retail_stride(self):
        for name, global_, count, size in (
                ('tt_rec_disp', 'tt_rec_pool', 8, 88),
                ('tt_sprite_disp', 'tt_player_shot_buf', 12, 84)):
            with self.subTest(renderer=name):
                body = '''
        GLOBAL = (Rec *)(memory + 0x12000);
        GLOBAL[0].flag = GLOBAL[LAST].flag = 1;
        GLOBAL[0].pri = GLOBAL[LAST].pri = 3;
        GLOBAL[0].x = 42;
        GLOBAL[0].y = 24;
        GLOBAL[0].tpage = 28;
        HIDDEN
        ot[5] = 0x00ffffffU;
        RENDER();
        if (sizeof(Rec) != SIZE || calls != 2 ||
            packets[0] != prim + 20 || packets[1] != prim + 52 ||
            *(u32 *)(buffer + 4) != (u32)(unsigned long)(prim + 64) ||
            ot[5] != ((u32)(unsigned long)(prim + 52) & 0xffffffU) ||
            *(u32 *)prim != 0x04ffffffU || prim[7] != 101 ||
            *(s16 *)(prim + 8) != 34 || *(s16 *)(prim + 10) != 16 ||
            *(u32 *)(prim + 24) != 28) return 31;
'''
                hidden = ('GLOBAL[1].flag = 1; GLOBAL[1].bits = 1;'
                          if name == 'tt_rec_disp' else '')
                body = body.replace('HIDDEN', hidden).replace('GLOBAL', global_)
                body = body.replace('LAST', str(count - 1)).replace('SIZE', str(size))
                self.check_renderer(name, body.replace('RENDER', name))

    def test_single_sprite_own_and_palette_colors(self):
        for palette in (0, 8):
            with self.subTest(palette=palette):
                self.check_renderer('tt_rec_sprite_disp', '''
        Rec *r;
        r = (Rec *)(memory + 0x12000);
        tt_step_wk = memory + 0x13000;
        tt_step_wk[29] = 7;
        tt_step_wk[30] = 8;
        tt_step_wk[31] = 9;
        r->pri = 3;
        r->x = 42; r->y = 24; r->w = 20; r->h = 30;
        r->r = 1; r->g = 2; r->b = 3; r->code = 101;
        r->bits = PALETTE;
        r->tpage = 28;
        r->u = 10; r->v = 11; r->clut = 123;
        ot[5] = 0x00ffffffU;
        tt_rec_sprite_disp(r);
        if (calls != 1 || packets[0] != prim + 20 ||
            *(u32 *)(buffer + 4) != (u32)(unsigned long)(prim + 32) ||
            ot[5] != ((u32)(unsigned long)(prim + 20) & 0xffffffU) ||
            *(u32 *)prim != 0x04ffffffU ||
            prim[4] != (PALETTE ? 7 : 1) ||
            prim[5] != (PALETTE ? 8 : 2) ||
            prim[6] != (PALETTE ? 9 : 3) ||
            prim[7] != (PALETTE ? 100 : 101) ||
            *(s16 *)(prim + 8) != 42 || *(s16 *)(prim + 10) != 24 ||
            *(s16 *)(prim + 16) != 20 || *(s16 *)(prim + 18) != 30 ||
            prim[12] != 10 || prim[13] != 11 || *(u16 *)(prim + 14) != 123 ||
            *(u32 *)(prim + 24) != 28) return 32;
'''.replace('PALETTE', str(palette)))

    def test_board_texture_cells_and_packets(self):
        self.check_renderer('tt_board_disp', '''
        tt_step_wk = (TTState *)(memory + 0x12000);
        /* State +24 -> three adjacent four-byte texture addresses. */
        *(u32 *)((u8 *)tt_step_wk + 24) =
            (u32)(unsigned long)(memory + 0x13000);
        *(u32 *)(memory + 0x13000) = (u32)(unsigned long)(memory + 0x14000);
        *(u32 *)(memory + 0x13004) = (u32)(unsigned long)(memory + 0x15000);
        *(u32 *)(memory + 0x13008) = (u32)(unsigned long)(memory + 0x16000);
        memory[0x15000] = 1;
        memory[0x15001] = 9;
        memory[0x15002] = 0x23;
        memory[0x16001] = 0xc0;
        ot[15] = ot[17] = 0x00ffffffU;
        tt_board_disp();
        if (calls != 3 || packets[0] != prim + 2048 ||
            packets[1] != prim + 2060 || packets[2] != prim + 2108 ||
            *(u32 *)(buffer + 4) != (u32)(unsigned long)(prim + 2120) ||
            *(u32 *)prim != 0x03ffffffU || prim[7] != 125 ||
            *(s16 *)(prim + 8) != -128 || *(s16 *)(prim + 10) != 112 ||
            prim[12] != 144 || prim[13] != 144 ||
            *(u16 *)(prim + 14) != (3 + (242 << 6)) ||
            ot[15] != ((u32)(unsigned long)(prim + 2048) & 0xffffffU) ||
            ot[17] != ((u32)(unsigned long)(prim + 2108) & 0xffffffU) ||
            *(u32 *)(prim + 2116) != 1) return 40;
''')


if __name__ == '__main__':
    unittest.main()
