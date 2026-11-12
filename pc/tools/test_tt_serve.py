# SPDX-License-Identifier: GPL-3.0-or-later
"""TT serve/script layouts and hit updates retain retail byte offsets."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402
PRELUDE = '''#include <stddef.h>
#include <string.h>
#include "types.h"
'''
CHECKS = '''
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
#define H(offset) (*(u16 *)(raw + (offset)))
#define W(offset) (*(u32 *)(raw + (offset)))
'''


class TtServeTests(unittest.TestCase):
    def compile_fixture(self, name, body):
        source = ROOT / f'pc/src/overlay/TT/{name}.c'
        if not source.exists():
            source = DECOMP / f'src/overlay/TT/{name}.c'
        fixture = PRELUDE + f'#include "{source}"\n' + CHECKS + body
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', '-mcmodel=large',
                 '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
                 str(path / 'test.c'), '-Wl,--defsym,tt_anim_tbl=0x88003000',
                 '-o', str(path / 'test')]).require_ok()
            result = run([str(path / 'test')])
            self.assertTrue(result.ok, f'{name}: fixture line {result.returncode}\n'
                                      + result.stdout)

    def test_intro_and_script_sequences(self):
        for name, work_type, unit_type, unit_field, step, timer, cursor, clear, end in (
                ('tt_serve_intro', 'Serve', 'Unit', 'unit', 'h8', 'h868', 'h980', 133, 156),
                ('tt_script_step', 'TTWork', 'Rec20', 'rec', 'f8', 'f868', 'f980', 143, 166)):
            with self.subTest(name=name):
                scripted = name == 'tt_script_step'
                globals_ = ('''s32 tt_script_seq_tbl, tt_puff_seq_tbl;
s16 tt_script_scr[] = {10, 11, 22, 33, 18, -1};
''' if scripted else '''u8 tt_flash_seq_tbl[1], tt_puff_seq_tbl[1];
s16 tt_serve_intro_scr[] = {10, 11, 22, 33, 18, -1};
''')
                body = globals_ + f'''static {work_type} work;
{work_type} *tt_boss_wk = &work;
static int calls, cue;
int tt_cmd_once(int value) {{ cue = value; return 0; }}
int tt_shape_clear(void) {{ return 0; }}
int tt_seq_step(void *script, int slot) {{
    (void)script; (void)slot; calls++; return 0;
}}
int main(void) {{
    u8 *raw;
    int i;
    CHECK(sizeof({unit_type}) == 20);
    CHECK(offsetof({work_type}, {unit_field}) == 56 &&
          offsetof({work_type}, {timer}) == 868 &&
          offsetof({work_type}, {cursor}) == 980);
    raw = (u8 *)&work;
    memset(raw, 0xa5, sizeof(work));
    H(8) = 0;
    {name}();
    CHECK(H(8) == 64 && H(868) == 1 && H(980) == 0 && cue == 1281);
    for (i = 16; i < 19; i++) {{
        CHECK(W(56 + i * 20) == 0x88003000U);
        CHECK(raw[60 + i * 20] == 0 && raw[61 + i * 20] == 4);
        CHECK(raw[66 + i * 20] == 0 && raw[72 + i * 20] == 0);
        CHECK(H(584 + i * 2) == 0 && H(632 + i * 2) == 0);
        CHECK(H(680 + i * 2) == {8 if scripted else 7});
        CHECK(raw[784 + i] == 0 && raw[62 + i * 20] == 0xa5);
    }}
'''
                if scripted:
                    body += '''    for (i = 3; i < 9; i++) {
        CHECK(W(56 + i * 20) == 0x88003000U);
        CHECK(raw[64 + i * 20] == 128 && H(680 + i * 2) == 6);
    }
    CHECK(H(590) == (u16)-38 && H(592) == 38 && H(638) == 15);
    CHECK(calls == 6);
'''
                else:
                    body += '''    for (i = 8; i < 16; i++) CHECK(raw[64 + i * 20] == 128);
    CHECK(calls == 0 && raw[836] == 1);
'''
                body += f'''    calls = 0;
    H(868) = 10;
    {name}();
    CHECK(H(980) == 5 && H(868) == 11 && calls == {9 if scripted else 11});
    CHECK(raw[802] == 1 && raw[424] == 128);
    CHECK(H(620) == 11 && H(668) == 22 && H(716) == 33);
    H(868) = {clear};
    {name}();
    for (i = 0; i < 16; i++) CHECK(raw[784 + i] == 0);
    CHECK(raw[802] == 1 && H(8) == 64);
    H(868) = {end};
    {name}();
    for (i = 0; i < 24; i++) CHECK(raw[784 + i] == 0);
    CHECK(H(8) == 0 && H(4) == 160 && H(868) == {end + 1});
    H(8) = 99; H(868) = 65535;
    {name}();
    CHECK(H(868) == 0);
    return 0;
}}
'''
                self.compile_fixture(name, body)

    def test_hit_flags_palette_ranges_and_board_stride(self):
        self.compile_fixture('tt_serve_hit_step', '''
static Serve work;
static TTRec records[50];
static Board boards[2];
Serve *tt_boss_wk = &work;
TTRec *tt_anim_buf = records;
Board *tt_board_ptr = boards;
static int hit, cue, drawn, slot_hits, body_hits, bad;
int tt_hit_check(void) { return hit; }
int tt_cmd_once(int value) { cue = value; return 0; }
int tt_board_hit(Board *board) {
    if (board != boards + drawn) bad = 1;
    drawn++; return 0;
}
int tt_slot_hit(void) { slot_hits++; return 0; }
int tt_body_hit_scan(void) { body_hits++; return 0; }
int main(void) {
    u8 *raw;
    int i, mode;
    raw = (u8 *)&work;
    CHECK(sizeof(Unit) == 20 && sizeof(TTRec) == 100 && sizeof(Board) == 3836);
    CHECK(offsetof(Serve, b834) == 834 && offsetof(Serve, h782) == 782);
    for (mode = 0; mode < 3; mode++) {
        memset(raw, 0, sizeof(work));
        memset(records, 0x55, sizeof(records));
        for (i = 0; i < 24; i++) raw[66 + i * 20] = 0x7f;
        raw[0] = 1; H(782) = mode;
        hit = mode & 1;
        drawn = slot_hits = body_hits = cue = bad = 0;
        tt_serve_hit_step();
        CHECK(!bad && drawn == 2 && body_hits == (mode != 0));
        CHECK(slot_hits == (mode == 1) && cue == (hit ? 1288 : 0));
        for (i = 0; i < 24; i++) CHECK(raw[66 + i * 20] == 0);
        for (i = 0; i < 50; i++) {
            CHECK(records[i].unk42 == ((i >= 34 && i <= 40) ||
                  (i >= 42 && i <= 48) ? (hit ? 15583 : 15577) : 0x5555));
        }
    }
    raw[834] = raw[835] = 1;
    drawn = cue = 0;
    records[40].unk42 = 123;
    tt_serve_hit_step();
    CHECK(!drawn && !cue && records[40].unk42 == 123);
    raw[0] = 0; raw[66] = 99;
    tt_serve_hit_step();
    CHECK(raw[66] == 99);
    return 0;
}
''')

    def test_unit_initializer_cannot_overwrite_visibility_with_height(self):
        self.compile_fixture('tt_unit_init', '''
static struct Obj work;
struct Obj *tt_boss_wk = &work;
short tt_unit_y_tbl[5] = {-9, 12, -7, 15, 20};
int main(void) {
    u8 *raw;
    int i;
    raw = (u8 *)&work;
    CHECK(sizeof(work.rec[0]) == 20 && offsetof(struct Obj, x) == 584);
    CHECK(offsetof(struct Obj, h) == 680 && offsetof(struct Obj, flag) == 784);
    memset(raw, 0xa5, sizeof(work));
    tt_unit_init();
    for (i = 2; i < 7; i++) {
        CHECK(W(56 + i * 20) == 0x88003000U);
        CHECK(raw[60 + i * 20] == 0 && raw[61 + i * 20] == 6);
        CHECK(raw[62 + i * 20] == (i == 6 ? 20 : 31));
        CHECK(raw[64 + i * 20] == 96 && raw[65 + i * 20] == 0);
        CHECK(raw[67 + i * 20] == 15 && raw[72 + i * 20] == 0);
        CHECK(H(584 + i * 2) == (u16)(i == 6 ? 0 : i * 16 - 56));
        CHECK(H(632 + i * 2) == (u16)tt_unit_y_tbl[i - 2]);
        CHECK(H(680 + i * 2) == 9 && raw[784 + i] == 1);
        CHECK(raw[66 + i * 20] == 0xa5);
    }
    CHECK(raw[784] == 0xa5 && raw[785] == 0xa5 && raw[791] == 0xa5);
    return 0;
}
''')

    def test_swing_initialization_and_reset_states(self):
        self.compile_fixture('tt_serve', '''
static Serve work;
static TTMain main_state;
static TTState state;
static TTTimer timer;
static TTFlag flag;
Serve *tt_boss_wk = &work;
TTMain *tt_game_wk = &main_state;
TTState *tt_step_wk = &state;
TTTimer *tt_stage_wk = &timer;
TTFlag *tt_player_wk = &flag;
static int tails, cue;
#define STUB(name) int name(void) { return 0; }
STUB(tt_sprite_pool_clear) STUB(tt_serve_init) STUB(tt_serve_ball_step)
STUB(tt_racket_disp) STUB(tt_serve_seq_step) STUB(tt_camera_ease)
STUB(tt_board_shot_step) STUB(tt_serve_walk_step) STUB(tt_serve_attack_cutscene_step) STUB(tt_swing_step)
STUB(tt_seq6_step) STUB(tt_seq_group_step) STUB(tt_rally) STUB(tt_serve_throw_cutscene_step)
int device_cmd_send(int value) { (void)value; return 0; }
int change_trans_pointer(u32 value) { (void)value; return 0; }
int tt_cmd_once(int value) { cue = value; return 0; }
int tt_serve_hit_step(void) { tails++; return 0; }
int main(void) {
    u8 *raw;
    int i;
    raw = (u8 *)&work;
    CHECK(sizeof(Unit) == 20 && offsetof(Serve, h940) == 940);
    memset(raw, 0xa5, sizeof(work));
    H(4) = 82;
    tt_serve();
    CHECK(H(4) == 83 && H(782) == 1 && H(940) == 1);
    CHECK(!H(932) && !H(938) && !H(870) && tails == 1);
    for (i = 0; i < 4; i++) CHECK(H(728 + i * 2) == 24 && raw[851 + i] == 1);
    for (i = 16; i < 21; i++) {
        CHECK(W(56 + i * 20) == 0x88003000U);
        CHECK(raw[60 + i * 20] == 0 && raw[61 + i * 20] == 6);
        CHECK(raw[64 + i * 20] == 96 && raw[66 + i * 20] == 0);
        CHECK(raw[72 + i * 20] == 0 && raw[784 + i] == 0);
        CHECK(H(584 + i * 2) == 0 && H(632 + i * 2) == 0);
        CHECK(H(680 + i * 2) == 8 && raw[62 + i * 20] == 0xa5);
    }
    memset(raw + 851, 0, 4);
    tt_serve();
    CHECK(H(4) == 84 && H(782) == 2 && H(780) == 72);
    H(780) = 65535;
    tt_serve();
    CHECK(H(4) == 128 && H(782) == 3 && cue == 1284 && raw[184] == 128);
    H(4) = 160;
    tt_serve();
    CHECK(!raw[0] && !state.b18 && timer.h30 == 1 && state.h2 == 160);
    CHECK(tails == 4);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
