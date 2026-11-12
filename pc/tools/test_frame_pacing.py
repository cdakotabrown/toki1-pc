# SPDX-License-Identifier: GPL-3.0-or-later
"""Wall pacing follows consumed emulated time, not the number of VSync calls."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include "pc/hal/system.c"
static u64 ticks;
static Uint32 wall = 1000;
static Uint32 waited;
static Uint32 oversleep;
static int turbo;
u64 hal_clock_ticks(void) { return ticks; }
u32 hal_clock_ticks_per_second(void) { return 33868800u; }
Uint32 SDL_GetTicks(void) { return wall; }
void SDL_Delay(Uint32 ms) { wall += ms + oversleep; waited += ms; }
void SDL_RenderPresent(SDL_Renderer *renderer) { (void)renderer; }
int SDL_PollEvent(SDL_Event *event) { (void)event; return 0; }
void gpu_display(void) { }
void pad_handle_event(const SDL_Event *event) { (void)event; }
void debug_handle_input(SDL_Event *event) { (void)event; }
void debug_frame(void) { }
int trace_turbo(void) { return turbo; }
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "failed: %s\n", #expression); return 1; } } while (0)

int main(int argc, char **argv) {
    int i;
    if (argc != 2) return 90;
    if (!strcmp(argv[1], "multi")) {
        hal_frame_begin(); ticks += 4u * 566204u; hal_frame_end();
        CHECK(waited == 66);
    } else if (!strcmp(argv[1], "fraction")) {
        for (i = 0; i < 60; i++) {
            hal_frame_begin(); ticks += 566204u; hal_frame_end();
        }
        CHECK(waited == (u32)((ticks * 1000u) / 33868800u));
    } else if (!strcmp(argv[1], "work")) {
        hal_frame_begin(); ticks += 4u * 566204u; wall += 20; hal_frame_end();
        CHECK(waited == 46);
    } else if (!strcmp(argv[1], "stall")) {
        hal_frame_begin(); ticks += 566204u; wall += 10000; hal_frame_end();
        CHECK(waited == 0);
        hal_frame_begin(); ticks += 566204u; hal_frame_end();
        CHECK(waited == 17);
    } else if (!strcmp(argv[1], "oversleep") || !strcmp(argv[1], "oversleep_legacy")) {
        /* every SDL_Delay sleeps 1 ms too long (and 4 ms of event polling
         * between frames): the schedule absorbs it, 60 VBlanks still take
         * 1.001 s of wall time; the legacy pacing loses 5 ms a frame */
        u32 start = wall, expect;
        if (!strcmp(argv[1], "oversleep_legacy")) setenv("TOKI_PACE_SLACK_MS", "0", 1);
        oversleep = 1;
        for (i = 0; i < 60; i++) {
            wall += 4;
            hal_frame_begin(); ticks += 566204u; hal_frame_end();
        }
        expect = (u32)((ticks * 1000u) / 33868800u);
        if (!strcmp(argv[1], "oversleep")) CHECK(wall - start <= expect + 5);
        else CHECK(wall - start >= expect + 60 * 5 - 5);
    } else if (!strcmp(argv[1], "late")) {
        /* a frame 30 ms late (within the 50 ms slack) is made up by the
         * next frames; one 200 ms late starts a new schedule */
        u32 start = wall;
        hal_frame_begin(); ticks += 566204u; hal_frame_end();
        hal_frame_begin(); ticks += 566204u; wall += 30 + 16; hal_frame_end();
        for (i = 0; i < 8; i++) { hal_frame_begin(); ticks += 566204u; hal_frame_end(); }
        CHECK(wall - start == (u32)((ticks * 1000u) / 33868800u));
        start = wall;
        hal_frame_begin(); ticks += 566204u; wall += 200; hal_frame_end();
        CHECK(wall - start == 200);
    } else if (!strcmp(argv[1], "zero")) {
        hal_frame_begin(); hal_frame_end();
        CHECK(waited == 0);
    } else if (!strcmp(argv[1], "turbo")) {
        turbo = 1;
        hal_frame_begin(); ticks += 4u * 566204u; hal_frame_end();
        CHECK(waited == 0);
    } else return 91;
    return 0;
}
'''


class FramePacingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = directory / 'pacing.c'
        source.write_text(SOURCE)
        cls.binary = directory / 'pacing'
        run(['gcc', '-std=c89', '-ffunction-sections', '-fdata-sections',
             '-I.', '-Ipc/include', '-Ipc/include/game', str(source),
             '-Wl,--gc-sections', '-o', str(cls.binary)],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, name):
        run([str(self.binary), name], timeout=10).require_ok()

    def test_four_consumed_vblanks_are_not_capped_as_one(self):
        self.check_case('multi')

    def test_fractional_milliseconds_do_not_drift(self):
        self.check_case('fraction')

    def test_work_time_is_part_of_the_frame_budget(self):
        self.check_case('work')

    def test_host_stall_does_not_create_catchup_burst(self):
        self.check_case('stall')

    def test_oversleep_is_made_up_by_the_schedule(self):
        self.check_case('oversleep')

    def test_negative_control_legacy_pacing_loses_the_oversleep(self):
        self.check_case('oversleep_legacy')

    def test_late_frame_is_made_up_within_the_slack_only(self):
        self.check_case('late')

    def test_no_emulated_time_does_not_invent_a_wait(self):
        self.check_case('zero')

    def test_turbo_bypasses_wall_waits(self):
        self.check_case('turbo')


if __name__ == '__main__':
    unittest.main()
