# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiled board/mesh regressions with high-address retail buffers."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class TtBoardNetTests(unittest.TestCase):
    def compile_fixture(self, name, body):
        source = ROOT / f'pc/src/overlay/TT/{name}.c'
        fixture = '''#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
''' + f'#include "{source}"\n' + '''
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
''' + body
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', '-I', str(ROOT / 'pc/include/game'),
                 '-I', str(ROOT / 'pc/include'), str(path / 'test.c'),
                 '-o', str(path / 'test')]).require_ok()
            result = run([str(path / 'test')])
            self.assertTrue(result.ok, f'{name}: fixture line {result.returncode}\n'
                                      + result.stdout)

    def test_board_initialization_and_scroll(self):
        for name in ('tt_board_init', 'tt_board_scroll'):
            with self.subTest(name=name):
                initialization = name == 'tt_board_init'
                state = ('u8 *tt_step_wk = state;' if initialization else
                         'TTHit *tt_step_wk = (TTHit *)state;')
                body = '''static u8 state[64];
''' + state + '''
TTRec *tt_anim_buf;
TTCel *tt_scale_buf;
int main(void) {
    u8 *memory, *records, *cells, *record, *cel;
    int i, j, phase, color;
    const u8 colors[3][3] = {{123,213,148}, {0,205,238}, {255,156,205}};
    CHECK(sizeof(TTRec) == 100 && sizeof(TTCel) == 16);
    CHECK(offsetof(TTRec, unk48) == 72 && offsetof(TTRec, unk4C) == 76);
    memory = mmap((void *)0x8b000000UL, 0x10000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(memory != MAP_FAILED);
    records = memory + 0x1000; cells = memory + 0x5000;
    tt_anim_buf = (TTRec *)records; tt_scale_buf = (TTCel *)cells;
'''
                if initialization:
                    body += '''    for (color = 12; color <= 15; color++) {
        memset(records, 0xa5, 12000); memset(cells, 0xa5, 25 * 16);
        memset(state, 0xa5, sizeof(state));
        tt_board_init(color, color & 1);
        for (i = 0; i < 25; i++) {
            record = records + (64 + i) * 100; cel = cells + i * 16;
            CHECK(*(u32 *)(record + 72) == (u32)(unsigned long)cel);
            CHECK(record[76] == 0 && record[77] == 0 && record[78] == 1);
            CHECK(record[79] == (color & 1) && record[80] == 0);
            CHECK(*(s16 *)cel == 4096 && *(s16 *)(cel + 2) == 4096);
            CHECK(*(s16 *)(cel + 4) == (i / 5) * 72 - 92);
            CHECK(*(s16 *)(cel + 6) == (i % 5) * 72 - 72);
            CHECK(*(s16 *)(record + 20) == (i / 5) * 72 - 128);
            CHECK(*(s16 *)(record + 22) == (i % 5) * 72 - 108);
            CHECK(*(u16 *)(record + 66) == 15360 + color);
            CHECK(record[0] == 1 && record[59] == 128);
            for (j = 8; j < 16; j++) CHECK(cel[j] == 0xa5);
        }
        for (i = 0; i < 4; i++) {
            record = records + (89 + i) * 100;
            CHECK(*(s16 *)(record + 20) == (i & 1 ? 88 : -96));
            CHECK(*(s16 *)(record + 22) == (i >> 1 ? 184 : 48));
        }
        CHECK(records[10900] == 1 && *(s16 *)(records + 10920) == -94);
        for (i = 0; i < 3; i++) CHECK(state[19 + i] ==
                                      (color < 15 ? colors[color - 12][i] : 0xa5));
        for (i = 0; i < 6400; i++) CHECK(records[i] == 0xa5);
        for (i = 11000; i < 12000; i++) CHECK(records[i] == 0xa5);
    }
'''
                else:
                    body += '''    (void)color; (void)colors;
    for (phase = 0; phase <= 71; phase += (phase ? 70 : 1)) {
        memset(records, 0xa5, 12000); memset(cells, 0xa5, 25 * 16);
        *(u16 *)(state + 46) = phase;
        tt_board_scroll();
        CHECK(*(u16 *)(state + 46) == (phase + 1) % 72);
        for (i = 0; i < 25; i++) {
            record = records + (64 + i) * 100; cel = cells + i * 16;
            CHECK(*(s16 *)(cel + 4) == (i / 5) * 72 - phase - 92);
            CHECK(*(s16 *)(cel + 6) == phase + (i % 5) * 72 - 72);
            CHECK(*(s16 *)(record + 20) == (i / 5) * 72 - phase - 128);
            CHECK(*(s16 *)(record + 22) == phase + (i % 5) * 72 - 108);
            for (j = 0; j < 100; j++) {
                if (j < 20 || j > 23) CHECK(record[j] == 0xa5);
            }
            for (j = 0; j < 16; j++) {
                if (j < 4 || j > 7) CHECK(cel[j] == 0xa5);
            }
        }
    }
'''
                body += '    return munmap(memory, 0x10000) != 0;\n}\n'
                self.compile_fixture(name, body)

    def test_net_packets_nested_cells_and_both_buffers(self):
        self.compile_fixture('tt_net_disp', '''
static NET state;
static u32 buffer_state[3], scroll_state[1];
NET *tt_step_wk = &state;
u8 *tt_disp_wk = (u8 *)buffer_state;
u8 *tt_stage_wk = (u8 *)scroll_state;
u8 *tt_db_buf;
int rcos(int angle) { return angle % 1024 - 512; }
int rsin(int angle) { return 512 - angle % 1024; }
static int coordinate(int value, int other, int y_axis) {
    int result;
    result = (value - 8) * (34816 - state.sway) / 1920;
    return result + (y_axis ? rsin(state.phase * 32 + other * 512) :
                    rcos(state.phase * 32 + other * 512)) / state.sway;
}
int main(void) {
    u8 *memory, *base, *packet, *indices, *tiles, *entry;
    u32 *table;
    TtDrawTag *ot;
    int buffer, scroll, i, j, x, y, s, u, v, tx, ty;
    CHECK(offsetof(NET, phase) == 50 && offsetof(NET, sway) == 52);
    CHECK(sizeof(TtDrawBuffer) == 32952 && sizeof(P_TAG) == 4);
    memory = mmap((void *)0x8a000000UL, 0x16000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(memory != MAP_FAILED);
    tt_db_buf = memory;
    table = (u32 *)(memory + 0x12000);
    indices = memory + 0x13000; tiles = memory + 0x14000;
    table[0] = (u32)(unsigned long)indices;
    table[1] = (u32)(unsigned long)tiles;
    table[2] = 0xdeadbeefU;
    state.tab_addr = (u32)(unsigned long)table;
    state.phase = 3; state.sway = 128;
    for (i = 0; i < 1024; i++) indices[i] = (i * 13 + 7) & 3;
    for (i = 0; i < 4; i++) {
        tiles[i * 3] = i & 1;
        tiles[i * 3 + 1] = 9 + i * 37;
        tiles[i * 3 + 2] = 0x21 + i;
    }
    for (buffer = 0; buffer < 2; buffer++) {
        for (scroll = 0; scroll < 3; scroll++) {
            buffer_state[2] = buffer;
            *(u16 *)((u8 *)scroll_state + 2) = scroll * 127;
            base = memory + buffer * 32952;
            memset(base, 0xa5, 32952);
            *(u32 *)(base + 4) = (u32)(unsigned long)(base + 0x100);
            ot = (TtDrawTag *)(base + 0x8000 + 60);
            ot->addr = 0xabcdef;
            tt_net_disp();
            CHECK(*(u32 *)(base + 4) == (u32)(unsigned long)(base + 0x100 + 10240));
            CHECK(*(u32 *)(base + 8) == 0xa5a5a5a5U && table[2] == 0xdeadbeefU);
            CHECK(ot->addr == (((u32)(unsigned long)(base + 0x100 + 255 * 40)) & 0xffffff));
            CHECK(ot->len == 0xa5);
            s = 768 - scroll * 127;
            for (i = 0; i < 256; i++) {
                x = i % 16; y = i / 16;
                packet = base + 0x100 + i * 40;
                CHECK(packet[3] == 9 && packet[7] == 45);
                CHECK(((P_TAG *)packet)->addr == (i ?
                      ((u32)(unsigned long)(packet - 40) & 0xffffff) : 0xabcdef));
                for (j = 0; j < 4; j++) {
                    tx = x + (j & 1); ty = y + (j >> 1);
                    CHECK(*(s16 *)(packet + 8 + j * 8) == coordinate(tx, ty, 0));
                    CHECK(*(s16 *)(packet + 10 + j * 8) ==
                          coordinate(ty, tx, 1) - (s & 15) + 128);
                }
                entry = tiles + indices[(y * 16 + x + (s & -16)) & 1023] * 3;
                u = ((entry[1] & 7) << 4) & 255;
                v = ((entry[1] >> 3) << 4) & 255;
                if (entry[0] == 1) v = (v + 128) & 255;
                CHECK(packet[12] == u && packet[13] == v);
                CHECK(packet[20] == u + 15 && packet[21] == v);
                CHECK(packet[28] == u && packet[29] == v + 15);
                CHECK(packet[36] == u + 15 && packet[37] == v + 15);
                CHECK(*(u16 *)(packet + 22) == 14);
                CHECK(*(u16 *)(packet + 14) ==
                      ((entry[2] >> 4) + 240) * 64 + (entry[2] & 15));
            }
        }
    }
    return munmap(memory, 0x16000) != 0;
}
''')


if __name__ == '__main__':
    unittest.main()
