# SPDX-License-Identifier: GPL-3.0-or-later
"""TT scene state and callback cells retain their retail offsets on x64."""
from pathlib import Path
import re
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402


class TtSceneTests(unittest.TestCase):
    def test_timer_callback_and_scene_initialization(self):
        original = DECOMP / 'src/overlay/TT/tt_scene_step.c'
        source = ROOT / 'pc/src/overlay/TT/tt_scene_step.c'
        if not source.exists():
            source = original
        # Only the scene controller runs; its sibling substeps are isolated.
        body = original.read_text().split('void tt_scene_step()', 1)[1]
        calls = set(re.findall(r'\b([a-z_]\w*)\s*\(', body))
        calls -= {'if', 'for', 'switch', 'bzero', 'tt_mode_set'}
        stubs = '\n'.join(f'int {name}() {{ return 0; }}' for name in sorted(calls))
        fixture = '''#include <stddef.h>
#include <string.h>
#include <strings.h>
#include "types.h"
''' + f'#include "{source}"\n' + stubs + '''
void tt_mode_set(void) {}
static TTMain main_;
static TTState state;
static TTDelta delta;
static TTTimer timer;
static TTPad pad, other_pad;
static TTRec records[128];
static TTObj objects[128];
TTMain *tt_game_wk = &main_;
TTState *tt_step_wk = &state;
TTDelta *tt_scroll_wk = &delta;
TTTimer *tt_stage_wk = &timer;
TTPad *tt_player_wk = &pad;
TTPad *tt_boss_wk = &other_pad;
TTRec *tt_anim_buf = records;
TTObj *tt_text_buf = objects;
u8 tt_scene_seq_dat[4];
static int callbacks;
static void callback(void) { callbacks++; }
int main(void) {
    if (sizeof(TTTimer) != 32 || offsetof(TTTimer, pad10) != 16 ||
        offsetof(TTState, unk1C) != 28 || offsetof(TTState, unk22) != 34)
        return 10;
    state.unk2 = 80;
    delta.unk0 = 0x8000;
    timer.unk0.w = 0x20000;
    *(u32 *)((u8 *)&timer + 12) = (u32)(unsigned long)callback;
    *(u32 *)((u8 *)&timer + 16) = 0xdeadbeefU;
    tt_scene_step();
    if (callbacks != 1 || timer.unk0.w != 0x28000 ||
        *(u32 *)((u8 *)&timer + 16) != 0xdeadbeefU) return 11;
    memset(&state, 0, sizeof(state));
    tt_scene_step();
    if (state.unk2 != 64 || state.unk11 != 1 || state.unk12 != 1 ||
        *(u32 *)((u8 *)&state + 24) != (u32)(unsigned long)tt_scene_seq_dat ||
        state.unk1C != 2 || state.unk22 ||
        objects[0].unkA != -112 || objects[1].unkA != -72 ||
        records[7].unk16 != 176 || records[8].unk00) return 12;
    state.unk2 = 64;
    timer.unk0.w = 1025 << 16;
    tt_scene_step();
    if (state.unk2 != 80) return 13;
    return 0;
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
