# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise TT actor states at the retail 120-byte stride on x64."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402


class TtActorTests(unittest.TestCase):
    def test_packed_records_and_state_transitions(self):
        override = ROOT / 'pc/src/overlay/TT/tt_actor_pool_step.c'
        source = override if override.exists() else DECOMP / 'src/overlay/TT/tt_actor_pool_step.c'
        fixture = '''#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
''' + f'#include "{source}"\n' + '''
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
static TTState state;
TTState *tt_step_wk = &state;
Actor *tt_obj_pool;
static int spawned, moved, animated, stepped, spread, fan, cue, selected;
static int bad, complete, ready, move_state;
static u8 *expected;

int tt_obj_spawn(void) { spawned++; return 0; }
void tt_obj_move(Actor *actor) {
    if ((u8 *)actor != expected) bad = 1;
    expected += 120;
    moved++;
    if (move_state) actor->h4 = move_state;
}
int tt_anim(void *animation, u8 *x, u8 *y) {
    if ((u8 *)animation != expected - 120 + 56 ||
        x != expected - 120 + 92 || y != expected - 120 + 93) bad = 1;
    *x = 144; *y = 32;
    animated++;
    return 0;
}
int anim_cel_step(void *animation) {
    if ((u8 *)animation != expected - 120 + 56) bad = 1;
    if (complete) ((u8 *)animation)[8] |= 1;
    stepped++;
    return 0;
}
int tt_ready_check(void) { return ready; }
int tt_touzyou_set(Actor *actor, int value) {
    if ((u8 *)actor != expected - 120) bad = 1;
    selected = value;
    return 0;
}
int tt_cmd_once(int value) { cue = value; return 0; }
int tt_shot_spread_make(int x, int y, int angle, int speed, int life) {
    if (x != 17 || y != 13 || angle != 256 || speed != 32 || life != 160)
        bad = 1;
    spread++;
    return 0;
}
int tt_shot_fan4_make(int x, int y, int angle, int speed, int life) {
    if (x != 17 || y != 13 || angle != 256 || speed != 32 || life != 160)
        bad = 1;
    fan++;
    return 0;
}
static void reset(void) {
    memset(tt_obj_pool, 0, 32 * 120);
    memset(&state, 0, sizeof(state));
    spawned = moved = animated = stepped = spread = fan = cue = selected = 0;
    bad = complete = ready = move_state = 0;
    expected = (u8 *)tt_obj_pool;
}
static Actor *activate(int slot, int type) {
    Actor *actor;
    actor = tt_obj_pool + slot;
    actor->b0 = 1;
    actor->h4 = type;
    actor->x = 17;
    actor->y = 13;
    expected = (u8 *)actor;
    return actor;
}
static void dirty_reset_fields(Actor *actor) {
    actor->h6 = actor->h8 = actor->h10 = 123;
    actor->b13 = actor->b14 = actor->b15 = 45;
}
static int reset_fields_clear(Actor *actor) {
    return !(actor->h6 || actor->h8 || actor->h10 ||
             actor->b13 || actor->b14 || actor->b15);
}
int main(void) {
    u8 *memory;
    Actor *actor;
    int i;
    CHECK(sizeof(Actor) == 120 && offsetof(Actor, b60) == 60 &&
          offsetof(Actor, b64) == 64 && offsetof(Actor, h86) == 86 &&
          offsetof(Actor, b92) == 92 && offsetof(Actor, h102) == 102);
    memory = mmap((void *)0x88000000UL, 0x4000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(memory != MAP_FAILED);
    tt_obj_pool = (Actor *)memory;
    memset(memory + 3840, 0xa5, 64);
    reset();
    tt_actor_pool_step();
    CHECK(spawned == 1 && !moved && !animated && !stepped);

    /* Every live slot, including the last, must use a 120-byte stride. */
    for (i = 0; i < 32; i++) activate(i, 64);
    expected = memory;
    tt_actor_pool_step();
    CHECK(!bad && moved == 32 && animated == 32 && !stepped);
    for (i = 0; i < 32; i++) {
        CHECK(memory[i * 120 + 92] == 144 && memory[i * 120 + 93] == 32);
    }
    for (i = 0; i < 64; i++) CHECK(memory[3840 + i] == 0xa5);

    /* Initializing the animation pointer cannot consume its adjacent bytes. */
    reset();
    actor = activate(31, 112);
    dirty_reset_fields(actor);
    tt_actor_pool_step();
    CHECK(!bad && moved == 1 && stepped == 1 && !animated && cue == 1549);
    CHECK(*(u32 *)((u8 *)actor + 56) == (u32)(unsigned long)tt_anim_tbl);
    CHECK(actor->b60 == 0 && actor->b61 == 2 && actor->b62 == 14 &&
          actor->b64 == 32 && actor->h4 == 113 && reset_fields_clear(actor));
    CHECK(!spread && !fan && state.b37 == 1);

    for (i = 0; i < 4; i++) {
        reset();
        activate(0, 112);
        state.b36 = 1;
        state.b37 = i;
        tt_actor_pool_step();
        CHECK(!bad && spread == (i == 0) && fan == (i == 2));
    }
    reset();
    activate(0, 112);
    state.b36 = 1; state.b37 = 255;
    tt_actor_pool_step();
    CHECK(!state.b37 && !spread && !fan);

    for (i = 0; i < 3; i++) {
        reset();
        actor = activate(31, 113);
        dirty_reset_fields(actor);
        complete = 1;
        ready = i;
        tt_actor_pool_step();
        CHECK(!bad && selected == 109 + i && reset_fields_clear(actor));
        CHECK(actor->h4 == (i ? 122 : 120));
        CHECK(actor->h102 == (i ? 0 : 60));
    }
    reset();
    actor = activate(0, 120);
    dirty_reset_fields(actor);
    actor->h102 = 1;
    tt_actor_pool_step();
    CHECK(!bad && actor->h4 == 121 && actor->h102 == 30 && actor->h86 == 2);
    CHECK(reset_fields_clear(actor));
    reset();
    actor = activate(0, 120);
    actor->h102 = 2;
    tt_actor_pool_step();
    CHECK(actor->h4 == 120 && actor->h102 == 1);
    reset();
    actor = activate(0, 121);
    actor->h102 = 1;
    tt_actor_pool_step();
    CHECK(actor->h4 == 128 && actor->b0);
    expected = (u8 *)actor;
    tt_actor_pool_step();
    CHECK(!actor->b0 && moved == 2);

    for (i = 0; i < 2; i++) {
        reset();
        actor = activate(0, i ? 122 : 64);
        actor->y = 249;
        tt_actor_pool_step();
        CHECK(!actor->b0 && moved == 1 && animated == !i && !bad);
    }
    reset();
    activate(0, 64);
    move_state = 113;
    tt_actor_pool_step();
    CHECK(!animated && stepped == 1 && !bad);
    reset();
    activate(0, 55);
    tt_actor_pool_step();
    CHECK(moved == 1 && !animated && !stepped && !bad);
    return munmap(memory, 0x4000) != 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(fixture)
            run(['gcc', '-std=c89', '-O2', '-fno-strict-aliasing',
                 '-fno-pie', '-no-pie', '-mcmodel=large',
                 '-I', str(ROOT / 'pc/include/game'),
                 str(path / 'test.c'), '-Wl,--defsym,tt_anim_tbl=0x88003000',
                 '-o', str(path / 'test')]).require_ok()
            result = run([str(path / 'test')])
            self.assertTrue(result.ok, f'actor fixture failed at C line {result.returncode}\n'
                                      + result.stdout)


if __name__ == '__main__':
    unittest.main()
