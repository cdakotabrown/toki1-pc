#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the checkpoint builder (checkpoints.py), the album fields it
added (tokimeki_album.py) and the suite's frame comparison
(coverage_suite.py).  No game assets needed; the ending-model test needs
disks/SLPS_000.65 and is skipped without it."""
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import checkpoints as cp  # noqa: E402
import coverage_suite as cs  # noqa: E402
import playthrough as pt  # noqa: E402
import tokimeki_album as alb  # noqa: E402

TBL = 0x80001000


def calendar_ram(events):
    """a RAM image with the cal_font_load pictures: EVENTS {(y, m, d): ev};
    every month has its real day count, kinds 0"""
    import calendar
    ram = bytearray(0x200000)
    table = 0x80100000
    struct.pack_into('<I', ram, TBL - 0x80000000, table)
    for idx in range(36):
        y, m = 95 + (idx + 3) // 12, (idx + 3) % 12 + 1
        n = calendar.monthrange(1900 + y, m)[1]
        pic = 0x80110000 + idx * 0x3000
        struct.pack_into('<I', ram, table - 0x80000000 + 4 * idx, pic)
        base = pic - 0x80000000 + 8192
        struct.pack_into('<II', ram, base, n, n)
        for i in range(n):
            struct.pack_into('<I', ram, base + 8 + 4 * (n + i), events.get((y, m, i + 1), 0))
    return bytes(ram)


SYMS = {'cal_font_load_tbl0': TBL}
# the game's season events for 1995-96
EVENTS = {(95, 7, 24): 6, (95, 9, 1): 10, (95, 12, 25): 14, (96, 1, 8): 16, (96, 3, 24): 20,
          (96, 4, 4): 2, (96, 7, 25): 6}


class AlbumFieldTest(unittest.TestCase):
    def test_season_from_the_calendar(self):
        ram = calendar_ram(EVENTS)
        self.assertEqual(alb.season_at(ram, 95, 7, 23, SYMS), 0)
        self.assertEqual(alb.season_at(ram, 95, 7, 24, SYMS), 1)    # the event's own day
        self.assertEqual(alb.season_at(ram, 95, 8, 31, SYMS), 1)
        self.assertEqual(alb.season_at(ram, 95, 9, 1, SYMS), 0)
        self.assertEqual(alb.season_at(ram, 95, 12, 31, SYMS), 2)
        self.assertEqual(alb.season_at(ram, 96, 1, 8, SYMS), 0)
        self.assertEqual(alb.season_at(ram, 96, 3, 31, SYMS), 3)
        self.assertEqual(alb.season_at(ram, 96, 4, 4, SYMS), 0)     # event 2 ends spring
        # the approximation it replaces is wrong on two of these nights
        self.assertNotEqual(alb.season_of(7, 20), alb.season_at(ram, 95, 7, 20, SYMS))

    def test_march_98_keeps_februarys_table(self):
        ram = calendar_ram({})
        self.assertEqual(alb.calendar_cal(ram, 98, 3, SYMS), alb.calendar_cal(ram, 98, 2, SYMS))
        self.assertEqual(alb.calendar_cal(ram, 98, 2, SYMS)[64], 31)   # [Feb 98][Mar 98]

    def test_new_fields_round_trip(self):
        rec = bytearray(alb.RECORD_SIZE)
        alb.set_field(rec, 'girl.yuko.spot.3', '5')
        alb.set_field(rec, 'girl.rei.birthday', '0x778')
        alb.set_field(rec, 'bukatu.baseball', '320')
        alb.set_field(rec, 'bukatu.baseball.b', '8')
        alb.set_field(rec, 'rec_tbl', 'hex:0102')
        alb.set_field(rec, 'icon_mode', '2')
        g = alb.FIELDS['girl_tbl'][0]
        self.assertEqual(rec[g + 56 * 8 + 23], 5)
        self.assertEqual(struct.unpack_from('<H', rec, g + 56 * 12 + 16)[0], 0x778)
        b = alb.FIELDS['bukatu_rec'][0] + 4 * alb.CLUBS.index('baseball')
        self.assertEqual(struct.unpack_from('<HB', rec, b), (320, 8))
        self.assertEqual(rec[1564:1566], b'\x01\x02')
        self.assertEqual(rec[3612], 2)
        with self.assertRaises(alb.AlbumError):
            alb.set_field(rec, 'girl.yuko.spot.36', '1')

    def test_record_layout_matches_save_game_restore(self):
        # save_game_restore's Chr offsets - 512 (src/main/matched/save_game_restore.c)
        for name, struct_off in (('date_basyo_hist', 2024), ('last_date_spot', 2028),
                                 ('last_date_spot_timer', 2044), ('last_date_spot_flag', 2060),
                                 ('rec_tbl', 2076), ('icon_mode', 4124), ('voice_level', 4128),
                                 ('activity', 4132), ('csr_speed', 4136), ('mouse_pad_st', 4140)):
            self.assertEqual(alb.FIELDS[name][0], struct_off - 512, name)


class CheckpointTest(unittest.TestCase):
    def test_month_nights(self):
        self.assertEqual(cp.month_start_night(95, 5), (95, 4, 30))
        self.assertEqual(cp.month_start_night(96, 4), (96, 3, 31))     # 1 Apr 96 is a Monday
        self.assertEqual(cp.month_start_night(98, 3), (98, 2, 22))     # 1 Mar 98 is a Sunday
        for y, m in ((95, 6), (96, 9), (97, 2)):
            s = cp.month_start_night(y, m)
            self.assertEqual(alb.weekday(*s), 6)
            self.assertLess(cp.as_date(s), cp.as_date((y, m, 1)))
            self.assertLessEqual(cp.as_date((y, m, 1)), cp.as_date(cp.add_days(s, 7)))

    def test_recipes_and_segments(self):
        recipes = cp.load_recipes()
        self.assertGreaterEqual(len(recipes), 36 + 14)
        months = {r['id'] for r in recipes if r['group'] == 'months'}
        self.assertEqual(len(months), 36)
        ending_girls = set()
        for r in recipes:
            seg = r['segment']
            if seg['kind'] == 'ending':
                ending_girls |= set(seg.get('rand', [seg.get('girl')]))
            rec = bytearray(alb.RECORD_SIZE)
            y, m, d = cp.ymd(r['date'])
            rec[4:9] = bytes((0, y, m, d, alb.weekday(y, m, d)))
            text = cp.segment(r, rec)
            with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as fh:
                fh.write(text)
            try:
                script = pt.parse(fh.name)
            finally:
                os.unlink(fh.name)
            labels = [it['label'] for it in pt.walk(script['phases'][0]['items'])
                      if it['kind'] in ('wait', 'check', 'case')]
            self.assertIn('album_loaded', labels, r['id'])
            if seg['kind'] == 'ending':
                self.assertIn('ending', labels)
            elif seg['kind'] == 'date':
                # the holiday screen's heart icon, never the press cycle's icon 5
                for lab in ('date_day', 'date_reserved', 'date_icon', 'date_start', 'date_home'):
                    self.assertIn(lab, labels, r['id'])
                self.assertIn('now_icon:1=9', text)
            elif seg.get('end_wait'):
                # a segment that ends where it is (ev_culture96_b3: in TT)
                self.assertIn('seg_end', labels, r['id'])
            elif seg['kind'] != 'name_entry':
                self.assertIn('week_end', labels)
        self.assertEqual(ending_girls, set(range(14)))

    def test_recipe_validation(self):
        with self.assertRaises(cp.CheckpointError):
            cp.validate(dict(id='x', date='95/5/1', segment=dict(kind='week')))   # a Monday
        with self.assertRaises(cp.CheckpointError):
            cp.validate(dict(id='x', date='95/4/16', segment=dict(kind='week')))  # before the base
        cp.validate(dict(id='x', date='95/4/30', segment=dict(kind='week')))

    def test_fragments(self):
        # every tracked fragment is used by a recipe and parses in place
        used = set()
        for r in cp.load_recipes():
            seg = r['segment']
            used |= {w[1:] for w in seg.get('waits', []) + seg.get('before', []) +
                     seg.get('title', []) + seg.get('options', []) +
                     ([seg['entry']] if seg.get('entry') else [])
                     if isinstance(w, str)}
        names = {f[:-4] for f in os.listdir(cp.FRAGMENTS) if f.endswith('.txt')}
        self.assertEqual(used, names)
        self.assertIn(cp.MASH, cp.fragment('@rensyu'))
        with self.assertRaises(cp.CheckpointError):
            cp.fragment('@no_such_fragment')
        with self.assertRaises(cp.CheckpointError):
            cp.fragment('rensyu')
        r = dict(id='x', file='f', date='95/4/30', segment=dict(
            kind='week', before=['@olh']))
        rec = bytearray(alb.RECORD_SIZE)
        rec[4:9] = bytes((0, 95, 4, 30, alb.weekday(95, 4, 30)))
        text = cp.segment(r, rec)
        # the help tour runs on the loaded planner, before its icon is clicked
        self.assertLess(text.index('wait olh_menu'), text.index('wait planner_icon'))
        self.assertGreater(text.index('wait olh_menu'), text.index('wait album_loaded'))
        r = dict(id='y', file='f', date='95/4/30', segment=dict(
            kind='event', waits=['@rensyu'], end_wait=['now_step=0x12', 900, '']))
        text = cp.segment(r, rec)
        self.assertIn('wait seg_end now_step=0x12 within 900\n', text)
        self.assertNotIn('wait week_end', text)

    def test_title_and_entry_fragments(self):
        """`title` plays on the album list before the load; `entry`
        replaces the name entry's three letters"""
        rec = bytearray(alb.RECORD_SIZE)
        rec[4:9] = bytes((0, 95, 4, 30, alb.weekday(95, 4, 30)))
        r = dict(id='x', file='f', date='95/4/30', segment=dict(
            kind='week', title=['@album_copy_delete']))
        cp.validate(r)
        text = cp.segment(r, rec)
        order = [text.index(w) for w in ('wait album_list ', 'wait copy_from ',
                                         'wait list_after_delete ', 'wait load_mode ',
                                         'wait album_loaded ')]
        self.assertEqual(order, sorted(order))
        self.assertEqual(text.count('wait album_list '), 1)
        # options: on the title menu, before the Continue press
        r = dict(id='o', file='f', date='95/4/30', base='cleared', segment=dict(
            kind='week', options=['@option_config'], title=['@album_copy_delete']))
        cp.validate(r)
        text = cp.segment(r, rec)
        order = [text.index(w) for w in ('wait title_menu ', 'wait opt_cfg ',
                                         'wait title_again ', 'wait album_list ',
                                         'wait copy_from ', 'wait album_loaded ')]
        self.assertEqual(order, sorted(order))
        r = dict(id='y', file='f', date='95/4/4', base='template', segment=dict(
            kind='name_entry', entry='@name_ent_keyboard'))
        text = cp.segment(r, rec)
        self.assertIn('wait given_again ', text)
        self.assertNotIn('# a\n', text)
        self.assertLess(text.index('wait album_loaded '), text.index('wait surname_entry '))
        self.assertLess(text.index('wait schedule_ready '), text.index('wait planner_icon '))
        with self.assertRaises(cp.CheckpointError):
            cp.validate(dict(id='z', date='95/4/30', segment=dict(
                kind='week', title=['@no_such_fragment'])))

    def test_reservation_word(self):
        # date_save.txt: the game's own entry for Shiori, Sun 16 Apr 95, the park
        self.assertEqual(cp.reservation_word(dict(girl=0, place=0, day='95/4/16')), 0x20104BE0)
        w = cp.reservation_word(dict(girl=7, place=17, day='96/8/4'))
        self.assertEqual((w & 0x1F, w >> 25 & 0xF, w >> 29 & 1), (17, 7, 1))
        r = dict(id='x', date='95/10/15', segment=dict(kind='date'),
                 reserve=dict(girl=0, place=0, day='95/10/22'))
        cp.validate(r)
        for bad in (dict(r, reserve=dict(girl=0, place=0, day='95/10/21')),    # a Saturday
                    dict(r, reserve=dict(girl=0, place=0, day='95/10/15')),    # not later
                    dict(r, reserve=dict(girl=0, place=21, day='95/10/22')),
                    dict(r, choice=2), dict(id='x', date='95/10/15', segment=dict(kind='date'))):
            with self.assertRaises(cp.CheckpointError):
                cp.validate(bad)

    def test_date_segment_clicks_the_heart(self):
        r = dict(id='x', file='f', date='95/10/15', segment=dict(kind='date'), choice=1,
                 reserve=dict(girl=0, place=0, day='95/10/29'))
        rec = bytearray(alb.RECORD_SIZE)
        text = cp.segment(r, rec)
        self.assertIn('now_month:1=10 now_day:1=29 within %d ' % (2 * cp.WEEK_FRAMES), text)
        self.assertIn('right*12', text)         # date_save.txt: the heart at (-114,+14)
        self.assertIn('down*38', text)
        self.assertIn(cp.DATE_MASH[1], text)

    def test_menu3_answer_reads_the_rows(self):
        text = cp.menu3_answer(0, dict(at='1/2', value=1), cp.MASH)
        self.assertIn('now_sub_step=1 now_sub_sub_step=2 menu_on:4=1', text)
        # menu3_select: row 0 junban, row 1 menu3_select_tbl0, row 2 menu3_select_tbl1
        for sym, down in (('junban', 15), ('menu3_select_tbl0', 20), ('menu3_select_tbl1', 26)):
            self.assertIn(f'{sym}:1=1\n  +20 left+up*95\n  +100 right+down*37\n  +42 down*{down}', text)
        # rows at y 48/64/80, 16 high (menu3_init): the hand lands inside each
        for k, (_, down) in enumerate(cp.MENU3_ROWS):
            y = -100 + 3 * 37 + 3 * down
            self.assertTrue(0 < y - (48 + 16 * k) < 16, (k, y))
        self.assertIn('check answer_0 serifu_no:4=1', text)

    def test_konami_code_is_the_tables_sequence(self):
        text = cp.konami_code('0/45', cp.MASH)
        presses = [ln.split()[1] for ln in text.splitlines() if ln.startswith('+')]
        self.assertEqual(presses, list(cp.KONAMI))
        self.assertTrue(text.splitlines()[1].startswith('+100 '))   # after the mash's held press
        if os.path.exists(cp.EXE):
            words = struct.unpack('<18I', cp.exe_reader()(0x800BA198, 72))
            mask = dict(up=0x1000, down=0x4000, left=0x8000, right=0x2000, cross=0x40, circle=0x20)
            want = [w for w in words if w]
            self.assertEqual(want, [mask[b] for b in cp.KONAMI])

    def test_spot_field(self):
        rec = bytearray(alb.RECORD_SIZE)
        alb.set_field(rec, 'spot.11', '1')
        self.assertEqual(rec[140 + 92 + 11], 1)
        with self.assertRaises(alb.AlbumError):
            alb.set_field(rec, 'spot.16', '1')

    def test_long_mash_and_light_trace(self):
        r = dict(id='x', file='f', date='95/10/22', mash='long', light_trace=True,
                 segment=dict(kind='event', waits=[['t', 'now_step=0x5b', 100, True]]))
        text = cp.segment(r, bytearray(alb.RECORD_SIZE))
        self.assertIn('mash ' + cp.LONG_MASH, text)
        self.assertNotIn('mash ' + cp.MASH, text)
        self.assertIn('trace ' + cp.ENDING_TRACE + '\n', text)
        self.assertLessEqual(len(cp.LONG_MASH.split('/')[0].split(',')), 16)   # trace.c's cycle cap
        # icon 14's spot: right 28, down 43 from the corner (ICON_COL / ICON_ROW)
        self.assertIn('right+down*28,down*15', cp.LONG_MASH)

    @unittest.skipUnless(os.path.exists(cp.EXE), 'needs disks/SLPS_000.65')
    def test_zyotai(self):
        rec = bytearray(alb.RECORD_SIZE)
        g = alb.FIELDS['girl_tbl'][0]
        self.assertEqual(cp.zyotai(rec, 0), (4, 4))                  # gauges 0
        struct.pack_into('<hh', rec, g + 2, 128, 0)
        struct.pack_into('<h', rec, g + 6, 128)
        self.assertEqual(cp.zyotai(rec, 0), (0x80, 0))               # "best"

    def test_icon_click_targets_the_icon(self):
        text = cp.icon_click(5, 'p')
        self.assertIn('right*4', text)          # spring_events.txt: icon 5 at (-138,-10)
        self.assertIn('down*30', text)
        self.assertIn('now_icon:1=5', text)

    @unittest.skipUnless(os.path.exists(cp.EXE), 'needs disks/SLPS_000.65')
    def test_ending_model(self):
        rec = bytearray(alb.RECORD_SIZE)
        g = alb.FIELDS['girl_tbl'][0]
        self.assertEqual(cp.ending_model(rec)[0], 13)               # nobody: Yoshio
        rec[g + 56 * 12 + 15] = 70                                    # denwa_cnt
        rec[alb.FIELDS['ev_flag'][0] + 0x4F] = 2
        self.assertEqual(cp.ending_model(rec)[0], 12)               # Rei
        rec[g + 56 * 12 + 15] = 69                                    # one call short
        self.assertEqual(cp.ending_model(rec)[0], 13)
        # Mio: met, gauges, liked stats, 4 dates -> 500+; 3 dates -> score 0
        for p in ('bun', 'ri', 'gei'):
            alb.set_field(rec, 'para_' + p, '148')
        for f, v in (('flags', '2'), ('tokimeki', '110'), ('yuukou', '110')):
            alb.set_field(rec, f'girl.mio.{f}', v)
        alb.set_field(rec, 'girl.mio.spot.3', '4')
        self.assertEqual(cp.ending_model(rec)[0], 1)
        alb.set_field(rec, 'girl.mio.spot.3', '3')
        self.assertEqual(cp.ending_model(rec)[0], 13)


class FrameCompareTest(unittest.TestCase):
    def write(self, path, rows):
        with open(path, 'w') as fh:
            for r in rows:
                fh.write(' '.join(f'{k}={v}' for k, v in r.items()) + '\n')

    def row(self, frame, g, step=0x34, buf='00', seed='01020304'):
        return dict(frame=frame, now_step=f'{step:02x}', now_sub_step='01', now_sub_sub_step='00',
                    frame_counter=g.to_bytes(4, 'little').hex(), rand_seed=seed,
                    schedule_buf=buf)

    def test_aligned_on_the_game_frame(self):
        with tempfile.TemporaryDirectory() as d:
            # the port is one game frame ahead (the title offset); the same
            # state follows, then the planner's schedule differs
            pc = [self.row(100 + i, 50 + i) for i in range(5)] + [self.row(105, 55, 0x30, 'ff')]
            ps = [self.row(200 + i, 49 + i) for i in range(5)] + [self.row(205, 54, 0x30, 'fe')]
            self.write(os.path.join(d, 'x.pc.trace'), pc)
            self.write(os.path.join(d, 'x.ps1.trace'), ps)
            rep = dict(pc=dict(album_loaded=dict(game=50)), retail=dict(album_loaded=dict(game=49)))
            out = cs.frame_compare(d, 'x', rep)
            self.assertEqual(out['status'], 'diverged')
            self.assertEqual(out['rel'], 5)
            self.assertEqual(out['fields'], ['schedule_buf'])
            self.assertEqual(out['cls'], 'rand:schedule')
            self.assertEqual(out['gate_fields'], [])
            # negative control: aligned on the raw frame counter instead, the
            # one-frame offset makes the comparison start out of step
            rep['retail']['album_loaded']['game'] = 50
            self.assertNotEqual(cs.frame_compare(d, 'x', rep).get('rel'), 5)

    def test_state_with_different_seeds_is_rand(self):
        with tempfile.TemporaryDirectory() as d:
            pc = [self.row(100 + i, 50 + i) for i in range(4)]
            ps = [self.row(200 + i, 50 + i) for i in range(4)]
            pc[3]['schedule_buf'] = 'ff'
            ps[3]['schedule_buf'] = 'fe'
            ps[3]['now_step'] = '53'
            pc[3]['now_step'] = '53'
            ps[3]['rand_seed'] = 'cd140000'
            pc[3]['rand_seed'] = 'ce140000'
            self.write(os.path.join(d, 'x.pc.trace'), pc)
            self.write(os.path.join(d, 'x.ps1.trace'), ps)
            rep = dict(pc=dict(album_loaded=dict(game=50)), retail=dict(album_loaded=dict(game=50)))
            self.assertEqual(cs.frame_compare(d, 'x', rep)['cls'], 'rand:state')
            ps[3]['rand_seed'] = 'ce140000'
            self.write(os.path.join(d, 'x.ps1.trace'), ps)
            self.assertEqual(cs.frame_compare(d, 'x', rep)['cls'], 'state')

    def test_a_torn_last_row_is_dropped(self):
        # a port that aborts mid-write (stack smashing in end_12_rei) leaves
        # half a row; earlier rows must still be read, a torn middle row not
        import trace_compare as tc
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't')
            with open(p, 'w') as fh:
                fh.write('frame=1 now_step=34\nframe=2 now_step=34\nframe=3 calendar=006203')
            self.assertEqual([r['frame'] for r in tc.parse_trace(p)], [1, 2])
            with open(p, 'w') as fh:
                fh.write('frame=1 now_step=34\nframe=2 calendar=006\nframe=3 now_step=34\n')
            with self.assertRaises(ValueError):
                tc.parse_trace(p)

    def test_a_dropped_frame_is_timing(self):
        with tempfile.TemporaryDirectory() as d:
            # the reference drops a VBlank at game frame 3; the icon differs at 4
            pc = [self.row(100 + i, 50 + i) for i in range(6)]
            ps = [self.row(200 + i + (i >= 3), 50 + i) for i in range(6)]
            ps[4]['now_step'] = '35'
            self.write(os.path.join(d, 'x.pc.trace'), pc)
            self.write(os.path.join(d, 'x.ps1.trace'), ps)
            rep = dict(pc=dict(album_loaded=dict(game=50)), retail=dict(album_loaded=dict(game=50)))
            out = cs.frame_compare(d, 'x', rep)
            self.assertEqual((out['rel'], out['cls']), (4, 'timing'))
            self.assertEqual(out['lag_first']['rel'], 3)


if __name__ == '__main__':
    unittest.main()
