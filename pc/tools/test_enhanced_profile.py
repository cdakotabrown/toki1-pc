# SPDX-License-Identifier: GPL-3.0-or-later
"""The run profile (pc/hal/profile.c) and the enhanced profile's features.

Unit harnesses compile the HAL file under test with pc/hal/profile.c:
  - profile selection and switch precedence (CLI > env > config > profile);
  - fast-forward in hal_frame_end (presents skipped, wall pacing divided);
  - the dialogue text assists (pc/hal/enhance.c) against fake engine state;
  - instant CD data reads and instant card I/O, each against parity, which
    must keep the timed models;
and, when the port is built and your assets are present, an enhanced
new_game playthrough that must reach the same milestones as parity in fewer
VBlanks.
"""
import json
import os
import sys
from pathlib import Path
import tempfile
import unittest

from safe_process import run
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
GCC = ['gcc', '-std=gnu17', '-fpermissive', '-w', '-O1', '-fno-pie', '-no-pie',
       '-ffunction-sections', '-fdata-sections', '-I', str(ROOT),
       '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
       '-I', str(ROOT / 'pc/hal')]
CLEAN_ENV = {k: v for k, v in os.environ.items()
             if not k.startswith(('TOKI_', 'PC_', 'SCENE', 'AUTOPLAY'))
             and k not in ('GAME_CPU_COST', 'CD_DRIVE_TIMING', 'CARD_IO_TIMING',
                           'GPU_DRAW_TIME')}


def build(directory, name, source, extra=()):
    path = Path(directory)
    (path / f'{name}.c').write_text(source)
    run(GCC + [str(path / f'{name}.c'), *map(str, extra), '-Wl,--gc-sections', '-lm',
               '-o', str(path / name)], timeout=60).require_ok()
    return path / name


def execute(binary, *args, env=None, cwd=None):
    e = dict(CLEAN_ENV)
    e.update(env or {})
    return run([str(binary), *args], env=e, cwd=cwd, timeout=30)


PROFILE_MAIN = r'''
#include <stdio.h>
#include "profile.h"
int main(int argc, char **argv) {
    unsigned i;
    argc = hal_profile_parse_args(argc, argv);
    printf("profile=%s source=%s argc=%d\n", hal_profile_name(), hal_profile_source(), argc);
    for (i = 1; i < (unsigned)argc; i++) printf("arg=%s\n", argv[i]);
    for (i = 0; i < hal_profile_switch_count; i++)
        printf("%s=%d\n", hal_profile_switches[i].name, hal_switch(hal_profile_switches[i].name));
    printf("TOKI_FF_SPEED=%ld\n", hal_setting_long("TOKI_FF_SPEED", 4));
    return 0;
}
'''


def parse(out):
    return dict(line.split('=', 1) for line in out.splitlines()
                if '=' in line and not line.startswith('[') and ' ' not in line.split('=', 1)[0])


class ProfileSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin = build(cls.tmp.name, 'profile', PROFILE_MAIN, [ROOT / 'pc/hal/profile.c'])
        cls.work = Path(cls.tmp.name) / 'work'
        cls.work.mkdir()

    def values(self, *args, env=None, cfg=None):
        if cfg is not None:
            (self.work / 'toki.cfg').write_text(cfg)
        elif (self.work / 'toki.cfg').exists():
            (self.work / 'toki.cfg').unlink()
        r = execute(self.bin, *args, env=env, cwd=self.work).require_ok()
        first = r.stdout.splitlines()
        head = next(l for l in first if l.startswith('profile='))
        return dict(kv.split('=', 1) for kv in head.split()), parse(r.stdout)

    def test_default_is_parity_with_every_timing_model_on(self):
        head, v = self.values()
        self.assertEqual(head['profile'], 'parity')
        self.assertEqual(head['source'], 'default')
        for name in ('GAME_CPU_COST', 'GPU_DRAW_TIME', 'CD_DRIVE_TIMING', 'CARD_IO_TIMING',
                     'BOOT_CPU_COST', 'MDEC_VLC_STILL', 'CARD_CREATE_IO'):
            self.assertEqual(v[name], '1', name)
        for name in ('TOKI_FAST_FORWARD', 'TOKI_TEXT_AUTO'):
            self.assertEqual(v[name], '0', name)

    def test_enhanced_turns_the_timing_models_off(self):
        head, v = self.values(env={'TOKI_PROFILE': 'enhanced'})
        self.assertEqual((head['profile'], head['source']), ('enhanced', 'env'))
        for name in ('GAME_CPU_COST', 'GPU_DRAW_TIME', 'CD_DRIVE_TIMING', 'CARD_IO_TIMING'):
            self.assertEqual(v[name], '0', name)
        self.assertEqual(v['TOKI_FAST_FORWARD'], '1')

    def test_an_environment_variable_overrides_the_profile_default(self):
        _, v = self.values(env={'TOKI_PROFILE': 'enhanced', 'GAME_CPU_COST': '1',
                                'CD_DRIVE_TIMING': ''})
        self.assertEqual(v['GAME_CPU_COST'], '1')
        self.assertEqual(v['CD_DRIVE_TIMING'], '0')      # empty: not set
        _, v = self.values(env={'GPU_DRAW_TIME': '0'})
        self.assertEqual(v['GPU_DRAW_TIME'], '0')        # the old negative control

    def test_config_file_sets_the_profile_and_switches(self):
        head, v = self.values(cfg='# comment\n[pc]\nprofile = Enhanced\ngame_cpu_cost = 1\n'
                                  'TOKI_FF_SPEED = 8 ; eight\n')
        self.assertEqual((head['profile'], head['source']), ('enhanced', 'config'))
        self.assertEqual(v['GAME_CPU_COST'], '1')
        self.assertEqual(v['GPU_DRAW_TIME'], '0')
        self.assertEqual(v['TOKI_FF_SPEED'], '8')
        _, v = self.values(env={'GAME_CPU_COST': '0'}, cfg='game_cpu_cost = 1\n')
        self.assertEqual(v['GAME_CPU_COST'], '0')        # env beats the file

    def test_command_line_beats_env_and_is_removed_from_argv(self):
        head, _ = self.values('--profile=parity', 'assets', env={'TOKI_PROFILE': 'enhanced'})
        self.assertEqual((head['profile'], head['source'], head['argc']), ('parity', 'cli', '2'))
        head, _ = self.values('--enhanced', 'assets')
        self.assertEqual((head['profile'], head['argc']), ('enhanced', '2'))
        head, _ = self.values('--profile', 'enhanced')
        self.assertEqual((head['profile'], head['argc']), ('enhanced', '1'))

    def test_unknown_profile_falls_back_to_parity(self):
        head, v = self.values(env={'TOKI_PROFILE': 'turbo'})
        self.assertEqual(head['profile'], 'parity')
        self.assertEqual(v['GAME_CPU_COST'], '1')

    def test_explicit_config_path(self):
        cfg = self.work / 'other.cfg'
        cfg.write_text('profile=enhanced\n')
        head, _ = self.values(env={'TOKI_CONFIG': str(cfg)})
        self.assertEqual(head['profile'], 'enhanced')


FF_SOURCE = r'''
#include "pc/hal/system.c"
static u64 ticks;
static Uint32 wall = 1000, waited;
static int presents, ff = 1;
u64 hal_clock_ticks(void) { return ticks; }
u32 hal_clock_ticks_per_second(void) { return 33868800u; }
Uint32 SDL_GetTicks(void) { return wall; }
void SDL_Delay(Uint32 ms) { wall += ms; waited += ms; }
void SDL_RenderPresent(SDL_Renderer *renderer) { (void)renderer; presents++; }
int SDL_PollEvent(SDL_Event *event) { (void)event; return 0; }
void gpu_display(void) { }
void pad_handle_event(const SDL_Event *event) { (void)event; }
void debug_handle_input(SDL_Event *event) { (void)event; }
void debug_frame(void) { }
int trace_turbo(void) { return 0; }
int hal_ff_frames(void) { return ff; }
int main(int argc, char **argv) {
    int i;
    ff = atoi(argv[1]);
    for (i = 0; i < 60; i++) { hal_frame_begin(); ticks += 566204u; hal_frame_end(); }
    printf("presents=%d\nwaited=%u\n", presents, waited);
    return 0;
}
'''


class FastForwardPacingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name) / 'ff.c'
        path.write_text(FF_SOURCE)
        cls.bin = Path(cls.tmp.name) / 'ff'
        run(['gcc', '-std=c89', '-ffunction-sections', '-fdata-sections', '-I.', '-Ipc/include',
             '-Ipc/include/game', str(path), '-Wl,--gc-sections', '-o', str(cls.bin)],
            cwd=ROOT, timeout=30).require_ok()

    def frames(self, ff):
        out = run([str(self.bin), str(ff)], timeout=10).require_ok().stdout
        return parse(out)

    def test_normal_speed_presents_every_frame_in_real_time(self):
        v = self.frames(1)
        self.assertEqual(v['presents'], '60')
        self.assertIn(int(v["waited"]), (1002, 1003))          # 60 VBlanks at 59.94 Hz

    def test_fast_forward_shows_every_nth_frame_n_times_faster(self):
        v = self.frames(4)
        self.assertEqual(v['presents'], '15')
        self.assertIn(int(v["waited"]), (250, 251))

    def test_unlimited_fast_forward_never_sleeps(self):
        v = self.frames(0)
        self.assertEqual(v['waited'], '0')
        self.assertEqual(v['presents'], '3')     # one in 16


TEXT_SOURCE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
s32 serifu_state;
s16 check_end_k_tbl0, check_end_k_tbl1;
u8 yazirushi_cw_disp, now_step, now_sub_step, now_sub_sub_step;
extern int text_assist_confirm(u32 player);
extern unsigned text_assist_presses(void);
static void page(int serifu, int printing, int arrow) {
    serifu_state = serifu;
    check_end_k_tbl0 = 10;
    check_end_k_tbl1 = printing ? 4 : 10;
    yazirushi_cw_disp = arrow ? 0x80 : 0;
}
int main(int argc, char **argv) {
    int i, out[400];
    for (i = 0; i < 400; i++) {
        u32 player = 0;
        if (!strcmp(argv[1], "serifu")) {
            /* serifu_main: a page prints, completes, the arrow comes when
             * its voice ends; then another screen; then a result screen
             * that blinks the arrow (16 polls on / 16 off), where the
             * player presses a button himself at 300..309 */
            now_step = 0x40;
            if (i < 50) page(1, 1, 0);
            else if (i < 200) page(1, 0, i >= 70);
            else if (i < 250) { now_step = 0x34; page(0, 0, 0); }
            else {
                now_step = 0x75;
                page(0, 0, ((i - 250) / 16) % 2 == 0);
                player = (i >= 300 && i < 310) ? 0x40 : 0;
            }
        } else {
            /* the TEL call: no serifu_state; a page waits with the arrow,
             * the next prints without it on the same step; then a menu on
             * another step prints its help text */
            now_step = 0x45;
            if (i < 30) page(0, 0, 1);
            else if (i < 80) page(0, 1, 0);
            else if (i < 120) page(0, 0, 0);
            else { now_step = 0x46; page(0, 1, 0); }
        }
        out[i] = text_assist_confirm(player);
    }
    for (i = 0; i < 400; i++) putchar('0' + out[i]);
    printf("\npresses=%u\n", text_assist_presses());
    return 0;
}
'''


class TextAssistTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin = build(cls.tmp.name, 'text', TEXT_SOURCE,
                        [ROOT / 'pc/hal/enhance.c', ROOT / 'pc/hal/profile.c'])

    def trace(self, env, mode='serifu'):
        out = execute(self.bin, mode, env=env).require_ok().stdout.splitlines()
        line = next(l for l in out if set(l) <= {'0', '1'} and len(l) == 400)
        return line, parse('\n'.join(out))

    def test_parity_and_plain_enhanced_never_press(self):
        for env in ({}, {'TOKI_PROFILE': 'enhanced'}):
            for mode in ('serifu', 'tel'):
                line, _ = self.trace(env, mode)
                self.assertEqual(line, '0' * 400)

    def test_never_presses_while_a_page_prints(self):
        line, _ = self.trace({'TOKI_TEXT_AUTO': '1', 'TOKI_TEXT_AUTO_DELAY': '1'})
        self.assertEqual(line[:70], '0' * 70)            # printing, then no arrow yet
        line, _ = self.trace({'TOKI_TEXT_AUTO': '1', 'TOKI_TEXT_AUTO_DELAY': '1'}, 'tel')
        self.assertEqual(line[30:], '0' * 370)

    def test_auto_advance_presses_after_the_arrow_and_the_delay(self):
        line, v = self.trace({'TOKI_PROFILE': 'enhanced', 'TOKI_TEXT_AUTO': '1',
                              'TOKI_TEXT_AUTO_DELAY': '30'})
        self.assertEqual(line[:99], '0' * 99)            # arrow at 70, +30 polls
        self.assertEqual(line[99:101], '11')             # one press, held 2 polls
        self.assertEqual(line[101:130], '0' * 29)        # re-press only after a delay
        self.assertEqual(line[130], '1')
        self.assertEqual(line[200:250], '0' * 50)        # another screen: nothing
        self.assertEqual(line[279:281], '11')            # the blinking arrow still waits
        self.assertEqual(line[300:310], '0' * 10)        # the player's own input wins
        self.assertGreaterEqual(int(v['presses']), 6)

    def test_auto_advance_in_a_tel_call(self):
        line, v = self.trace({'TOKI_PROFILE': 'enhanced', 'TOKI_TEXT_AUTO': '1',
                              'TOKI_TEXT_AUTO_DELAY': '10'}, 'tel')
        self.assertEqual(line[9:11], '11')
        self.assertEqual(line[30:], '0' * 370)           # no arrow after the page: no press

    def test_every_auto_press_is_an_edge(self):
        line, v = self.trace({'TOKI_PROFILE': 'enhanced', 'TOKI_TEXT_AUTO': '1',
                              'TOKI_TEXT_AUTO_DELAY': '1'})
        self.assertEqual(line[70:73], '110')             # the shortest delay still releases
        self.assertNotIn('111', line)
        self.assertGreater(int(v['presses']), 20)


CD_SOURCE = r'''#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include "pc/hal/cd.c"
const FileLbaEntry g_file_lba_table[1] = {{"XA/TEST.XA", 1000, 64 * 2048}};
const u32 g_file_lba_table_count = 1;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_vblank_interrupt(void) {}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) {}
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
static u64 read_ticks(u32 lba, int n, u32 *p) {
    u8 loc[4];
    u64 before;
    CdIntToPos((s32)lba, loc);
    CdControlB(2, loc, 0);
    before = hal_clock_ticks();
    assert(CdRead(n, p, 0x80));
    while (CdReadSync(1, 0) > 0) hal_cpu_spend(1000);
    return hal_clock_ticks() - before;
}
int main(void) {
    u32 *p = mmap((void *)0x80140000UL, 64 * 2048, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    u8 mode = 0x80;
    assert(p != MAP_FAILED);
    CdControlB(0x0e, &mode, 0);        /* double speed */
    printf("cooked=%llu\n", (unsigned long long)read_ticks(20000, 32, p));
    printf("raw=%llu\n", (unsigned long long)read_ticks(1000, 16, p));
    return 0;
}
'''


class CdInstantLoadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin = build(cls.tmp.name, 'cd', CD_SOURCE, [ROOT / 'pc/hal/profile.c'])
        raw = Path(cls.tmp.name) / 'pc/assets/raw/XA'
        raw.mkdir(parents=True)
        (raw / 'TEST.XA').write_bytes(bytes(64 * 2336))    # stored raw: a stream file

    def ticks(self, env):
        r = execute(self.bin, env=env, cwd=self.tmp.name).require_ok()
        return {k: int(v) for k, v in parse(r.stdout).items()}

    def test_parity_reads_at_the_drive_rate(self):
        t = self.ticks({})
        per_sector = 33868800 // 150
        self.assertGreater(t['cooked'], 32 * per_sector)
        self.assertGreater(t['raw'], 16 * per_sector)

    def test_enhanced_data_reads_are_instant_and_streams_stay_real_time(self):
        p, e = self.ticks({}), self.ticks({'TOKI_PROFILE': 'enhanced'})
        self.assertLess(e['cooked'], 566204)               # under a frame (32 sectors)
        self.assertLess(e['cooked'] * 20, p['cooked'])
        per_sector = 33868800 // 150
        self.assertGreaterEqual(e['raw'], 16 * per_sector)  # XA / STR: the drive's rate

    def test_cd_drive_timing_override_restores_the_model(self):
        p = self.ticks({})
        e = self.ticks({'TOKI_PROFILE': 'enhanced', 'CD_DRIVE_TIMING': '1'})
        self.assertEqual(p['cooked'] > 32 * (33868800 // 150), e['cooked'] > 32 * (33868800 // 150))


CARD_SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include "pc/hal/memcard.c"
#include "file_id.h"
unsigned int g_pc_debug_flags;
int g_pc_debug_initialized;
u8 D_800FF869;
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
void *hal_ps1_ptr(const void *p) { return (void *)p; }
int pc_ptr_is_mapped(const void *p, unsigned long n) { return p != 0; }
extern void hal_bios_vblank_tick(void);
extern s32 OpenEvent(u32, u32, u32, void (*)(void));
extern s32 EnableEvent(s32);
extern u32 hal_clock_vblanks(void);
static int sw_events;
static void sw(void) { sw_events++; }
void hal_vblank_interrupt(void) { hal_bios_vblank_tick(); }
static u32 mask_value = 13;
u32 GetIntrMask(void) { return mask_value; }
u32 SetIntrMask(s32 value) { u32 old = mask_value; mask_value = (u32)value & 0x7ffu; return old; }
void hal_irq_ack(u16 bits) { (void)bits; }
static u8 page[0x2000], back[0x2000];
int main(void) {
    u32 v0, v1;
    int fd, i, entry;
    s32 ev = OpenEvent(CARD_SW_EVENT, CARD_IOE, 0x1000, sw);
    EnableEvent(ev);
    memcard_card_init();
    memcard_card_start();
    v0 = hal_clock_vblanks();
    assert(memcard_bu_init() == 0);
    printf("bu_init=%u\n", hal_clock_vblanks() - v0);
    fd = memcard_file_open("bu00:ALBUM", 0x10200);
    assert(fd >= 0 && memcard_file_close(fd) == 0);
    fd = memcard_file_open("bu00:ALBUM", 0x8002);
    entry = dir_find("ALBUM", 1);
    for (i = 0; i < (int)sizeof page; i++) page[i] = (u8)(i * 13 + 5);
    sw_events = 0;     /* _bu_init's directory load delivered one */
    v0 = hal_clock_vblanks();
    assert(memcard_file_write(fd, page, sizeof page) == (int)sizeof page);
    while (memcard_card_status(0) != 1) hal_cpu_spend(10000);
    v1 = hal_clock_vblanks();
    assert(memcmp(block_data(entry), page, sizeof page) == 0);
    assert(sw_events == 1);
    memcard_file_close(fd);
    printf("write=%u\n", v1 - v0);
    fd = memcard_file_open("bu00:ALBUM", 0x8001);
    v0 = hal_clock_vblanks();
    assert(memcard_file_read(fd, back, sizeof back) == (int)sizeof back);
    while (memcard_card_status(0) != 1) hal_cpu_spend(10000);
    assert(memcmp(back, page, sizeof page) == 0 && sw_events == 2);
    printf("read=%u\n", hal_clock_vblanks() - v0);
    return 0;
}
'''


class CardInstantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin = build(cls.tmp.name, 'card', CARD_SOURCE,
                        [ROOT / 'pc/hal/cd.c', ROOT / 'src/main/pc-port/bios_trampolines.c',
                         ROOT / 'pc/hal/profile.c'])

    def vblanks(self, env):
        with tempfile.TemporaryDirectory() as work:
            r = execute(self.bin, env=env, cwd=work).require_ok()
            return {k: int(v) for k, v in parse(r.stdout).items()}

    def test_parity_writes_one_sector_per_slot_turn(self):
        v = self.vblanks({})
        self.assertGreaterEqual(v['write'], 128)      # 64 sectors, every second VBlank
        self.assertGreaterEqual(v['read'], 127)
        self.assertGreater(v['bu_init'], 60)

    def test_enhanced_card_io_is_instant_with_the_same_data_and_events(self):
        v = self.vblanks({'TOKI_PROFILE': 'enhanced'})
        self.assertLessEqual(v['write'], 1)
        self.assertLessEqual(v['read'], 1)
        self.assertLessEqual(v['bu_init'], 2)


PC_BIN = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine


@unittest.skipUnless(PC_BIN.exists() and (ROOT / 'pc/assets/raw').exists(),
                     'needs the built port and your pc/assets')
class EnhancedPlaythroughTests(unittest.TestCase):
    """new_game.txt on both profiles: same milestones, fewer VBlanks."""

    def playthrough(self, profile, out):
        r = run([sys.executable, str(ROOT / 'pc/tools/playthrough.py'),
                 str(ROOT / 'pc/tools/playthrough/new_game.txt'), '--out', str(out),
                 '--profile', profile], cwd=ROOT, env=CLEAN_ENV, timeout=1200,
                max_output=8 * 1024 * 1024)
        with open(out / 'report.json') as fh:
            report = json.load(fh)
        self.assertTrue(r.ok, f'{profile}: {report["failures"]}')
        return report['pc']

    def test_enhanced_new_game_reaches_the_milestones_sooner(self):
        with tempfile.TemporaryDirectory() as d:
            par = self.playthrough('parity', Path(d) / 'parity')
            enh = self.playthrough('enhanced', Path(d) / 'enhanced')
        # every milestone that is not decided by the game's rand
        for label in ('title_menu', 'surname_entry', 'schedule_ready', 'lesson',
                      'title_menu_2', 'album_loaded'):
            self.assertIn(label, enh)
            self.assertLess(enh[label]['vblank'], par[label]['vblank'], label)
        # loads and card writes: the album creation (sysfile_prompt -> surname
        # entry) and the reload (title -> album loaded) at least halve
        def span(m, a, b):
            return m[b]['vblank'] - m[a]['vblank']
        self.assertLess(span(enh, 'sysfile_prompt', 'surname_entry') * 2,
                        span(par, 'sysfile_prompt', 'surname_entry'))
        self.assertLess(span(enh, 'title_menu_2', 'album_loaded') * 1.5,
                        span(par, 'title_menu_2', 'album_loaded'))
        # no frame overruns outside the boot movie: game frames keep up with
        # VBlanks from the title on
        for label in ('surname_entry', 'lesson'):
            par_lag = par[label]['vblank'] - par[label]['game']
            enh_lag = enh[label]['vblank'] - enh[label]['game']
            self.assertLess(enh_lag, par_lag, label)


if __name__ == '__main__':
    unittest.main()
