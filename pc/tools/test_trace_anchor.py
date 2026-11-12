# SPDX-License-Identifier: GPL-3.0-or-later
"""State-anchored PC_INPUT (wait / +N:MASK / end) in the actual trace.c.

A wait blocks until its conditions hold at a VBlank, anchors the relative
presses after it to that game frame, and a wait that is not satisfied in
time exits 3 with a NOT-reached line.  No SDL, assets or game code: the
game state is driven by the harness, the pad mask read back through
trace_input_mask_pc (the HAL's layout: PSX bytes swapped).
"""
import os
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]

HARNESS = f'#include "{ROOT}/pc/hal/trace.c"\n' + r'''
u8 now_step, now_sub_step, now_sub_sub_step, now_girl, cd_state2, step_mode;
s32 frame_counter, step_frame_cnt, vblnk_counter, pad_data, pad_trig, pad_rel;
s32 activeBuff;
volatile int g_crash_count;
u8 screen_w[512];
u32 g_psx_rand_seed;
u16 probe16 = 0;
u64 hal_clock_ticks(void) { return 0; }
void parity_anchor(void) { }
void parity_vblank(void) { }
/* one VBlank of game frame g: anchored input resolves, the pad is sampled */
static u32 vblank(long g) {
    u32 m;
    frame_counter = (s32)g;
    trace_vblank_begin();
    m = trace_input_mask_pc();
    trace_vblank();
    return m;
}
int main(void) {
    long g;
    trace_init();
    for (g = 0; g < 10; g++) if (vblank(g)) { puts("early-press"); return 1; }
    now_step = 0x12; now_sub_step = 2;          /* the wait's state */
    for (g = 10; g < 15; g++) {
        u32 m = vblank(g);
        /* anchored at 10: +3 START (held 2) -> 13,14 */
        if ((g == 13 || g == 14) != (m == 0x0800)) { printf("start@%ld %x\n", g, m); return 1; }
    }
    probe16 = 0x1234;                           /* symbol condition, size 2 */
    for (g = 15; g < 40; g++) {
        u32 m = vblank(g);
        /* anchored at 15: +5 CROSS held 4 -> 20..23 */
        if ((g >= 20 && g < 24) != (m == 0x0040)) { printf("cross@%ld %x\n", g, m); return 1; }
    }
    puts("no-end");
    return 1;
}
'''

SCRIPT = '''hold 2
wait menu now_step=0x12 now_sub_step=2 within 100
+3:0008
wait probe probe16:2=0x1234 now_step!=0
+5:4000*4
end 10
'''


class TraceAnchorTests(unittest.TestCase):
    def build(self, d):
        (d / 'test.c').write_text(HARNESS)
        # -rdynamic: the symbol condition resolves through dlsym, as in the game
        run(['gcc', '-std=c89', '-O0', '-rdynamic', '-I', str(ROOT / 'pc/include/game'),
             str(d / 'test.c'), '-ldl', '-o', str(d / 'test')]).require_ok()

    def env(self, script):
        env = {k: v for k, v in os.environ.items() if not k.startswith('PC_')}
        env['PC_INPUT'] = script
        return env

    def test_waits_anchor_presses_and_end(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            self.build(d)
            (d / 's.txt').write_text(SCRIPT)
            r = run([str(d / 'test')], env=self.env(str(d / 's.txt')))
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn('wait menu reached: game frame 10', r.stdout)
            self.assertIn('wait probe reached: game frame 15', r.stdout)
            # end 10 after the last press (20): exits at game frame 30
            self.assertIn('PC_INPUT end at game frame 30', r.stdout)

    def test_unreached_wait_fails_loudly(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            self.build(d)
            (d / 's.txt').write_text('wait never now_step=0x55 within 4\n+1:0008\n')
            r = run([str(d / 'test')], env=self.env(str(d / 's.txt')))
            self.assertEqual(r.returncode, 3, r.stdout)
            self.assertIn('wait never NOT reached within 4 vblanks', r.stdout)

    def test_mash_presses_until_the_wait_holds(self):
        """`mash MASK/K` presses every K game frames from the wait's start
        while the conditions fail and never once they hold."""
        harness = HARNESS.split('int main(void) {')[0] + r'''
int main(void) {
    long g;
    trace_init();
    for (g = 0; g < 40; g++) {
        u32 m;
        if (g == 17) probe16 = 0x55AA;          /* the screen arrives */
        m = vblank(g);
        /* mash CROSS/5 from 0: 5,6 10,11 15,16; then +3 START at 20,21 */
        if ((g == 5 || g == 6 || g == 10 || g == 11 || g == 15 || g == 16) != (m == 0x0040)
            || (g == 20 || g == 21) != (m == 0x0800)) {
            printf("mask@%ld %x\n", g, m);
            return 1;
        }
    }
    return 1;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            (d / 'test.c').write_text(harness)
            run(['gcc', '-std=c89', '-O0', '-rdynamic', '-I', str(ROOT / 'pc/include/game'),
                 str(d / 'test.c'), '-ldl', '-o', str(d / 'test')]).require_ok()
            (d / 's.txt').write_text('hold 2\nwait w probe16:2=0x55aa mash 4000/5\n+3:0008\nend 10\n')
            r = run([str(d / 'test')], env=self.env(str(d / 's.txt')))
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn('wait w reached: game frame 17', r.stdout)
            # a mash period not above the hold is refused
            (d / 'b.txt').write_text('hold 2\nwait w probe16:2=0x55aa mash 4000/2\n')
            r = run([str(d / 'test')], env=self.env(str(d / 'b.txt')))
            self.assertEqual(r.returncode, 2, r.stdout)

    def test_branch_takes_the_first_case_that_holds(self):
        """`branch LABEL N` + N `case LABEL TARGET COND...` lines: the first
        case to hold is the milestone reached; its items run, the other's
        do not, and `goto` joins them after the branch."""
        harness = HARNESS.split('int main(void) {')[0] + r'''
int main(void) {
    long g;
    trace_init();
    for (g = 0; g < 40; g++) {
        u32 m;
        if (g == 7) probe16 = 2;                /* case b holds, not a */
        if (g == 20) now_step = 0x34;           /* the join */
        m = vblank(g);
        /* case b: +2 CIRCLE (9,10); join at 20: +3 START (23,24);
         * case a's SQUARE (0x8000 PSX -> 0x0080 here) never */
        if ((g == 9 || g == 10) != (m == 0x0020)
            || (g == 23 || g == 24) != (m == 0x0800)) {
            printf("mask@%ld %x\\n", g, m);
            return 1;
        }
    }
    return 1;
}
'''
        script = ('hold 2\n'
                  'branch answer 2 within 50 mash 4000/30\n'     # 0
                  'case yes 3 probe16:2=1\n'                     # 1
                  'case no 5 probe16:2=2\n'                      # 2
                  '+2:8000\n'                                    # 3 (yes)
                  'goto 7\n'                                     # 4
                  '+2:2000\n'                                    # 5 (no)
                  'goto 7\n'                                     # 6
                  'wait join now_step=0x34\n'                    # 7
                  '+3:0008\n'
                  'end 10\n')
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            (d / 'test.c').write_text(harness)
            run(['gcc', '-std=c89', '-O0', '-rdynamic', '-I', str(ROOT / 'pc/include/game'),
                 str(d / 'test.c'), '-ldl', '-o', str(d / 'test')]).require_ok()
            (d / 's.txt').write_text(script)
            r = run([str(d / 'test')], env=self.env(str(d / 's.txt')))
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn('wait answer reached: game frame 7', r.stdout)
            self.assertIn('branch answer -> no', r.stdout)
            self.assertIn('wait no reached: game frame 7', r.stdout)
            self.assertNotIn('wait yes reached', r.stdout)
            self.assertIn('wait join reached: game frame 20', r.stdout)
            # a branch whose case lines are missing is refused
            (d / 'b.txt').write_text('branch x 2\ncase a 2 probe16:2=1\n+1:0008\n')
            r = run([str(d / 'test')], env=self.env(str(d / 'b.txt')))
            self.assertEqual(r.returncode, 2, r.stdout)

    def test_long_scripts_load_fast_and_press_as_sorted(self):
        """Relative presses are inserted in order and the mask scan starts
        past the presses that ended (pc/tools/explorer.py writes scripts of
        ~10,000 presses; a qsort per relative press was quadratic in the
        first VBlank).  Every VBlank's mask equals the model's: the OR of
        the presses with frame <= g < frame + hold -- absolute lines out of
        order, relative presses with their own holds, presses after a wait."""
        import random
        import time
        rng = random.Random(7)
        n, lines, model, cur = 30000, ['hold 3'], [], 0
        for _ in range(40):
            f, m = rng.randint(0, 5000), 1 << rng.randint(0, 15)
            lines.append('%d:%04x' % (f, m))
            model.append((f, m, 3))
        for _ in range(n):
            delta = rng.randint(0, 9)
            cur += delta
            m, h = 1 << rng.randint(0, 15), rng.choice((0, 0, 1, 5, 40))
            lines.append('+%d:%04x%s' % (delta, m, '*%d' % h if h else ''))
            model.append((cur, m, h or 3))
        wait = cur + 100
        lines += ['wait w probe16:2=0x77', '+0:4000*50', '+3:0008']
        model += [(wait, 0x4000, 50), (wait + 3, 0x0008, 3)]
        last = wait + 80
        harness = HARNESS.split('int main(void) {')[0] + r'''
int main(int argc, char **argv) {
    long g, last = atol(argv[1]), wait = atol(argv[2]);
    trace_init();
    for (g = 0; g <= last; g++) {
        if (g == wait) probe16 = 0x77;
        printf("m %ld %x\n", g, (unsigned)vblank(g));
    }
    return 0;
}
'''

        def swap(m):    # the HAL layout: PSX bytes swapped
            return ((m & 0xFF) << 8) | (m >> 8)
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            (d / 'test.c').write_text(harness)
            run(['gcc', '-std=c89', '-O0', '-rdynamic', '-I', str(ROOT / 'pc/include/game'),
                 str(d / 'test.c'), '-ldl', '-o', str(d / 'test')]).require_ok()
            (d / 's.txt').write_text('\n'.join(lines) + '\n')
            t0 = time.monotonic()
            r = run([str(d / 'test'), str(last), str(wait)], env=self.env(str(d / 's.txt')),
                    timeout=120, max_output=64 << 20)
            took = time.monotonic() - t0
            self.assertEqual(r.returncode, 0, r.stdout[-2000:])
            got = {}
            for line in r.stdout.splitlines():
                if line.startswith('m '):
                    _, g, m = line.split()
                    got[int(g)] = int(m, 16)
            want = [0] * (last + 1)
            for f, m, h in model:
                for g in range(max(f, 0), min(f + h, last + 1)):
                    want[g] |= m
            bad = [g for g in range(last + 1) if got.get(g) != swap(want[g])]
            self.assertEqual(bad[:5], [], 'VBlanks whose mask differs from the model')
            self.assertLess(took, 30, 'a %d-press script took %.1f s' % (n, took))

    def test_mark_lines(self):
        """PC_MARK_EVERY=N: one `[MARK] game frame` line per new multiple."""
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            self.build(d)
            (d / 's.txt').write_text(SCRIPT)
            env = self.env(str(d / 's.txt'))
            env['PC_MARK_EVERY'] = '10'
            r = run([str(d / 'test')], env=env)
            marks = [l for l in r.stdout.splitlines() if l.startswith('[MARK]')]
            self.assertEqual([l.split()[3] for l in marks], ['0', '10', '20'])
            self.assertIn('step 12/02/00', marks[1])

    def test_bad_wait_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            self.build(d)
            (d / 's.txt').write_text('wait bad no_such_symbol_xyz=1\n')
            r = run([str(d / 'test')], env=self.env(str(d / 's.txt')))
            self.assertEqual(r.returncode, 2, r.stdout)


if __name__ == '__main__':
    unittest.main()
