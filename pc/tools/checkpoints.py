#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checkpoint card saves and their playthrough segments (coverage phases 2/3).

A checkpoint is an album save on a memory card, crafted from a save the game
itself wrote, plus a short playthrough script (a "segment") that starts by
loading it through the title's Continue.  The port and the reference
(`playthrough.py --retail --start-card`) load the same bytes the same way;
nothing in a run pokes state or jumps scenes.

The recipes are tracked (pc/tools/checkpoints/*.json); the cards and the
generated scripts are not -- they derive from YOUR assets, under --out:

    python3 pc/tools/checkpoints.py list                  # every recipe
    python3 pc/tools/checkpoints.py show ID               # the crafted values
    python3 pc/tools/checkpoints.py build [ID|GROUP ...]  # cards + segments
        [--out build/checkpoints] [--bin pc/build/tokimeki-pc]

`build` first makes the bases (once, cached in --out/base/):

  template.mcr  the pre-name-entry album (95/4/4) that new_game.txt's first
                phase writes (scenario.ensure_template), and schedule.ram, the
                PS1 RAM on that night: it holds the calendar pictures that
                `date=` reads the month tables (cal) and the season from;
  played.mcr    the album date_save.txt saves on the night of its date:
                after name entry, three weeks of play, a date with Shiori --
                names, birthday, home places and gauges the game set itself.

then, per recipe, --out/<id>/card.mcr (album 1 rewritten) and
--out/<id>/segment.txt (a playthrough.py script).

Recipe fields (one JSON object per checkpoint):

  id        unique name (also the segment's phase name)
  base      "played" (default), "template", or "cleared" (made on first
            use: the card end_00_shiori_sys leaves on the port, its system
            file written after the clear: the ending girl's mascot_flag)
  date      YY/MM/DD -- the night planner's date.  The game saves on the
            night schedule (step 52), which runs on a Sunday (schedule_make
            plans Monday..Sunday from the day after now_day; schedule_init4
            advances the date when the week starts), so a natural save is
            dated a Sunday.  date= also sets youbi, pass_days, the month's
            cal table and the exact season (tokimeki_album.season_at).
  profile   "typical" (default) or "none": the progression below applied
            for the date before `set`
  activity  the week's activity icon the segment clicks on the planner
            (0 humanities, 1 science, 2 art, 3 sports, 4 club, 5 misc.
            knowledge, 6 looks, 7 rest); also written to the record
  set       {FIELD: VALUE} tokimeki_album fields applied last
  segment   {"kind": "week", "weeks": N}  N weeks (default 1), to the night
            {"kind": "event", "waits": [[LABEL, CONDS, WITHIN, RAND(, MASH)], ...]}
                    milestones on the way, then the next night after them;
                    a wait given as "@NAME" inlines the tracked script
                    fragment pc/tools/checkpoints/play/NAME.txt (playthrough.py
                    lines: the scripted input for a menu, a battle or a
                    minigame the press cycle cannot play; `{MASH}` in it is
                    the press cycle below).  "end_wait": [CONDS, WITHIN,
                    MASH] replaces the closing wait for the next night
                    (a segment that ends elsewhere: the title, a
                    minigame's result)
            "before": ["@NAME", ...] (week and event segments) inlines
                    fragments on the loaded planner, before its icon is
                    clicked (a screen the planner opens and returns from:
                    the on-line help)
            {"kind": "ending", "girl": G | "rand": [G, ...]}  to graduation
                    (step 99), the ending's girl (now_girl), and on to the
                    staff roll / save / title; "to_title": true goes on
                    through the staff roll, saves album 1 on ETC's save
                    screen (step 0xc4; save_sys_set writes the system
                    file) and on to the title
            {"kind": "name_entry"}  the template card: name entry, prologue,
                    the first week; "entry": "@NAME" plays the entry
                    screens by a fragment (surname screen to
                    schedule_ready) instead of NAME_ENTRY
            "options": ["@NAME", ...] (any kind) inlines fragments on the
                    title menu before Continue (the system config, step
                    32); each ends back on the title menu
            "title": ["@NAME", ...] (any kind) inlines fragments on the
                    title's album list before the load (the album
                    screens: copy, delete); each ends back on the list
            {"kind": "date", "waits": [...] (optional)}  the reserved date
                    (needs `reserve`): the week to the date's Sunday, the
                    holiday screen's heart icon (icon 9, holiday_icon_check
                    case 9: reserved_date_check > 0 -> sub-step 254 ->
                    machiawase_set -> DATE, step 80), the date with its
                    choices (DATE_MASH, `choice` picks the menu line), its
                    milestones (e.g. the date event, step 94), home to the
                    night planner with the reservation used up;
                    "answers": [{"at": "S/SS", "value": V}, ...] answers
                    the three-row menu (menu3_select) at that sub-step /
                    sub-sub-step with the row whose serifu_no is V;
                    "konami": "S/SS" enters the Konami command there
                    (konami_code)
  reserve   {"girl": G, "place": P, "day": "YY/MM/DD"}: a reservation as
            tel_dialog20 (state 32) writes it -- place bits 0..4, year,
            month, day, girl bits 25..28, active bit 29; written to slot 0
            (reserved.0).  The day must be a Sunday after `date`.  A date
            day's holiday screen offers the date on icon 9 only: any other
            icon stands her up (TEL step 67 sub 1, the reservation is
            cleared), which is what the old ev_date_shiori segment did.
  choice    (date segments) the menu line the date's choices take: 0 (the
            default) or 1 (DATE_MASH)
  mash      (event segments) "default" (MASH) or "long" (LONG_MASH: also takes
            the planner's forced icon 14, for waits over many weeks)
  light_trace  trace the lighter row (ENDING_TRACE): a long segment stays
            under the reference's 64 MiB trace cap
  max_frames  the port's frame budget (default by kind, MAX_FRAMES)
  mood      "likes" or "best": `build` checks get_g_zyotai_s/h of the
            reserved girl (or `mood_girl`) on the crafted record
            (check_reservation)
  covers    inventory ids / notes this checkpoint is for
  expect_ending  (ending recipes) the ending the scoring model predicts;
            `build` refuses a recipe whose crafted state predicts otherwise

The "typical" profile (plausible values for a player on that date):

  - stats rise linearly from the played save (95/4/23) to a balanced
    graduation-level player (PROFILE_END), stress stays 25..35;
  - the option menu_mode 1 written to the record (menus by the d-pad).
    It has no effect: menu_mode (0x800EF7F2) is outside the block
    save_game_restore writes (it is the system file's option), so a
    loaded checkpoint keeps the pointer menus (menu_mode 0, traced
    2026-11-01) and scripted menus aim the hand (playthrough.py `point`);
  - the tennis club (joined in April 95), club points 3 per week, level
    (bukatu_rec[club].b low nibble) one per 40 points, up to 10;
  - Shiori: gauges rising, one ordinary date per ten weeks (spot 1);
  - Yuko met from 95/7/2 (her 雑学 stages), Mio from 96/1/14, Yumi on
    96/4/4 (the calendar's debut), each with a few points and a small bomb.

The ending recipes are checked against the scoring model (tables read from
disks/SLPS_000.65); see ending_model().
"""
import argparse
import datetime
import glob
import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tokimeki_album as alb  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input  # noqa: E402
RECIPES = os.path.join(HERE, 'checkpoints')
FRAGMENTS = os.path.join(RECIPES, 'play')
PC_BIN = os.path.join(ROOT, 'pc/build/tokimeki-pc')
EXE = str(disc_input('disks/SLPS_000.65'))

STAT_KEYS = ('tai', 'bun', 'ri', 'gei', 'un', 'zat', 'you', 'kon', 'str')
PLAYED_DATE = (95, 4, 23)
# a balanced player at graduation (98/2/22): every stat but stress raised
# by roughly 0.6..0.9 a week from the played save
PROFILE_END = dict(tai=160, bun=150, ri=140, gei=125, un=150, zat=125, you=140, kon=110, str=30)
END_DATE = (98, 2, 22)
TYPICAL_CLUB = 'tennis'

# The planner's activity icons, measured on the port (a raster sweep of the
# hand over the planner with now_icon traced): rows y -46..-40, -34..-22,
# -16..-4, 8..20, 26..32; columns x -150..-126, -126..-102, -102..-78.
# Row 0 holds 0 1 2, row 1 holds 3 _ 4 (the middle slot is empty), row 2
# 5 6 7, row 3 8 9 10, row 4 11 12 13; with icon_num 15 (exam week, school
# trip) icon 14 is a fourth column on row 4, x -78..-54.  The hand parks at
# (-150,-100) and moves 3 px per held frame.  Icon k -> (row, column) ->
# (right N, down M).
ICON_COL = (4, 12, 20, 28)
ICON_ROW = (19, 24, 30, 38, 43)
ICON_POS = {0: (0, 0), 1: (0, 1), 2: (0, 2), 3: (1, 0), 4: (1, 2), 5: (2, 0), 6: (2, 1), 7: (2, 2),
            9: (3, 1), 14: (4, 3)}
# the holiday screen's date (heart) icon: row 3, column 1 (date_save.txt's
# (-114,+14)); holiday_icon_check case 9
DATE_ICON = 9


def forced_icon(rec, activity):
    """the icon the planner leaves usable on the night of REC, or None when
    the activities are free: step34_init2 (src/main/matched/step34_init2.c)
    reads tomorrow's calendar event m = cal[33 + day] (screen_w+102 +
    now_day - 1) and the event a week on, cal[38 + day] (+107)"""
    day, cal = rec[7], rec[12:140]
    club = alb.get_field(rec, 'basyo_pack') >> 12
    kind = alb.get_field(rec, 'basyo_pack') >> 8 & 0xF
    m = cal[33 + day] if 33 + day < 128 else 0
    if m == 5:
        return 14                       # exam week
    if m == 8 and club != 11:
        return 4                        # club summer week
    if 38 + day < 128 and cal[38 + day] == 12 and club < 6:
        return 7 if kind == 3 else 4    # culture club's festival week
    if m == 11:
        return 14                       # school trip
    if alb.get_field(rec, 'byouki_state') & 1:
        return activity if activity in (0, 1, 2) else 0
    if kind == 3:
        return 7
    return None

# state traced on both machines every frame for the per-frame comparison
# (on top of trace_compare.FIELDS: steps, calendar, player, ...).  The
# reference's trace is capped at 64 MiB (safe_process.FILE_LIMIT), so a row
# stays under 1 KB: girls 0..1, the reservations, the week's schedule, the
# planner icon and the activity word.  The full restored state is compared
# once, from the RAM dumps of the album_loaded shot (coverage_suite.py).
STATE_TRACE = ('girl_tbl:112 reserved_date:32 schedule_buf:64 schedule_cursor:1 now_icon:1 '
               'sched_icon:1')
# an ending segment runs ~4x longer (the last week, the ending, the staff
# roll): a lighter row
ENDING_TRACE = 'reserved_date:32 schedule_cursor:1 now_icon:1 sched_icon:1'
MAX_FRAMES = {'ending': 120000}     # the port's frame budget per segment kind

LOAD = """\
wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000
+30 start
wait album_list now_step=0x13 now_sub_step=2 now_sub_sub_step=2 within 3000
+30 left+up*80              # park the hand (-150,-100)
+82 right*24                # the load button
+26 down*9
+11 circle
wait load_mode now_step=0x13 now_sub_step=2 now_sub_sub_step=1 within 600
wait load_pick now_step=0x13 now_sub_step=2 now_sub_sub_step=2 within 600
+40 circle                  # album 1 (card_file_read)
"""

ALBUM_LIST = 'wait album_list now_step=0x13 now_sub_step=2 now_sub_sub_step=2 within 3000'


TITLE_MENU = 'wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000'


def load_text(title=(), options=()):
    """LOAD, with the fragments OPTIONS played on the title menu first (the
    system config, step 32; each ends back on the title menu, 0x12/2/1)
    and TITLE on the album list (the album screens: copy, delete; each
    ends back on the album list, 0x13/2/2), from which the load goes on"""
    text = LOAD.rstrip()
    if title:
        head, tail = text.split(ALBUM_LIST + '\n')
        text = '\n'.join([head + ALBUM_LIST] + [fragment(ref) for ref in title]) + '\n' + tail
    if options:
        head, tail = text.split(TITLE_MENU + '\n')
        text = '\n'.join([head + TITLE_MENU] + [fragment(ref) for ref in options]) + '\n' + tail
    return text


NAME_ENTRY = """\
wait surname_entry now_step=0x14 now_sub_step=1 now_sub_sub_step=1 within 1200
+30 left+up*80              # park (-150,-100)
+82 right*13                # x=-111: column 0
+15 down*20                 # y=-40: row 0
+22 circle                  # a
+10 right*5                 # x=-96: column 1
+10 circle                  # i
+10 start
wait given_entry now_step=0x14 now_sub_step=1 now_sub_sub_step=3 within 600
+20 right*6                 # column 2
+10 circle                  # u
+10 right*5                 # column 3
+10 circle                  # e
+10 start
wait nick_entry now_step=0x14 now_sub_step=1 now_sub_sub_step=5 within 600
+20 right*5                 # column 4
+10 circle                  # o
+10 start
wait birthday_entry now_step=0x14 now_sub_step=1 now_sub_sub_step=7 within 600
+30 start                   # name_ent_hizuke_rand: a birthday from the kernel rand
wait confirm now_step=0x14 now_sub_step=1 now_sub_sub_step=9 within 600
+30 start
wait fuji_entry now_step=0x14 now_sub_step=2 now_sub_sub_step=1 within 600
+30 start
wait fuji_confirm now_step=0x14 now_sub_step=2 now_sub_sub_step=3 within 600
+30 start
wait prologue now_step=0x15 now_sub_step=0 now_sub_sub_step=1 within 600
+120 start
wait schedule_ready now_step=0x34 now_sub_step=1 within 1200
"""

WEEK_FRAMES = 16000       # VBlanks one week may take with every event tapped through
# The taps that carry a week through its events (one press every 45
# frames, or H + 5 after a press held H frames):
#  - circle with the hand on icon 5 (-138,-10): picks 雑学 on a Sunday's
#    holiday screen and on the next night's planner, advances dialogue;
#  - down, circle, up: meant for the d-pad menus of menu_mode 1, which
#    the profile writes to the record but the load does not restore (see
#    the profile above): with the pointer menus the pair moves the hand
#    3 px down and back;
#  - the hand-only choice menus (menu_sel / the "taku" menus: GEKO talks,
#    the entrance exam) read the pointer, and each opens with the hand
#    somewhere of its own (GEKO 89: (-129,+28)), so the cycle parks it in
#    the clamp corner (-150,-100) and taps at (-39,+59), measured on the
#    port: GEKO menu_sel line 0 is y 50..62 (line 1 68..74), the exam's
#    rows y 53..65 / 68..80, all from x -129/-120 to 120), then at
#    (-39,+74) (the next line: GEKO 68..74, the exam 68..80, the sports
#    festival's event menu 68..80 / 86..98, spring_events.txt); x -39 is
#    right of every planner/holiday icon, so there the taps hit nothing;
#  - park again and back to icon 5.  The cycle needs no start position:
#    the event waits begin right after the planner click (a season
#    message lasts two frames), and one turn of the cycle puts the hand
#    on icon 5.
MASH = ('circle,down,circle,up,circle,'
        'left+up*95,right+down*37,down*16,circle,down*5,circle,'
        'left+up*95,right+down*4,down*26,circle/45')
# A cycle for an event that may take many weeks (a rand debut): MASH without
# its d-pad turn, plus a tap on icon 14 (-66,+29), the one the planner leaves
# on the night before an exam week or the school trip (forced_icon); on any
# other planner or holiday screen nothing is there.
LONG_MASH = ('circle,'
             'left+up*95,right+down*37,down*16,circle,down*5,circle,'
             'left+up*95,right+down*28,down*15,circle,'
             'left+up*95,right+down*4,down*26,circle/45')
MASHES = {'default': MASH, 'long': LONG_MASH}
ENDING_FRAMES = 60000     # the ending, its staff roll and the save
DATE_FRAMES = 30000       # a date: meeting, the places, the walk home
# A date's choices (`choice`): 0 is MASH (the d-pad's selected line, then the
# pointer's line 0 at (-39,+59) before line 1 at (-39,+74)); 1 walks the
# d-pad selection down one line before the tap and taps the pointer's line
# 1 first, so every menu of the date takes its second line.
DATE_MASH = (MASH,
             'down,circle,up,circle,'
             'left+up*95,right+down*37,down*21,circle,up*5,circle,'
             'left+up*95,right+down*4,down*26,circle/45')


class CheckpointError(Exception):
    pass


# ------------------------------------------------------------------ dates
def ymd(text):
    y, m, d = (int(x) for x in text.split('/'))
    return y, m, d


def as_date(t):
    return datetime.date(1900 + t[0], t[1], t[2])


def from_date(d):
    return d.year - 1900, d.month, d.day


def add_days(t, n):
    return from_date(as_date(t) + datetime.timedelta(n))


def month_start_night(year, month):
    """the planning night (a Sunday) whose week holds the 1st of YY/MM:
    the last Sunday strictly before the 1st"""
    first = datetime.date(1900 + year, month, 1)
    back = (first.weekday() + 1) % 7 or 7      # days back to the Sunday before
    return from_date(first - datetime.timedelta(back))


# ------------------------------------------------------------------ profile
def frac(date):
    """0 at the played save, 1 at graduation"""
    a, b, c = as_date(PLAYED_DATE), as_date(END_DATE), as_date(date)
    return max(0.0, min(1.0, (c - a).days / (b - a).days))


def lerp(a, b, t):
    return int(round(a + (b - a) * t))


def girl_flags(rec, g):
    return struct.unpack_from('<I', rec, alb.FIELDS['girl_tbl'][0] + 56 * g + 12)[0]


def typical(rec, date, start):
    """the "typical" profile's fields for DATE -> {field: value} (strings);
    START: the played save's own values"""
    t = frac(date)
    weeks = max(0, (as_date(date) - as_date(PLAYED_DATE)).days // 7)
    out = {}
    for k in STAT_KEYS:
        v = lerp(start['para_' + k], PROFILE_END[k], t)
        if k == 'str':
            v = 25 + (weeks * 7) % 11       # 25..35, wanders week to week
        out['para_' + k] = v
    out['club'] = TYPICAL_CLUB
    out['menu_mode'] = 1        # not restored by the load (see the docstring)
    pts = 3 * weeks
    out['bukatu.%s' % TYPICAL_CLUB] = pts
    out['bukatu.%s.b' % TYPICAL_CLUB] = min(10, pts // 40)
    # Shiori: gauges up, an ordinary date every ten weeks
    out['girl.shiori.tokimeki'] = lerp(start['g0_tok'], 70, t)
    out['girl.shiori.yuukou'] = lerp(start['g0_yuu'], 80, t)
    out['girl.shiori.spot.1'] = weeks // 10
    met = alb.FLAG_MET

    def girl(name, since, tok, yuu, bomb, dates):
        if date < since:
            return
        g = alb.GIRLS.index(name)
        out[f'girl.{name}.flags'] = girl_flags(rec, g) | met | alb.FLAG_PHONE
        s = frac(since)
        u = 0.0 if t <= s else (t - s) / (1 - s)
        out[f'girl.{name}.tokimeki'] = lerp(tok[0], tok[1], u)
        out[f'girl.{name}.yuukou'] = lerp(yuu[0], yuu[1], u)
        out[f'girl.{name}.bomb'] = lerp(bomb[0], bomb[1], u)
        out[f'girl.{name}.spot.2'] = lerp(0, dates, u)
    girl('yuko', (95, 7, 2), (10, 35), (50, 70), (5, 25), 2)
    girl('mio', (96, 1, 14), (5, 25), (50, 65), (5, 20), 1)
    girl('yumi', (96, 4, 4), (0, 20), (50, 62), (0, 15), 1)
    return out


def start_values(rec):
    g0 = alb.FIELDS['girl_tbl'][0]
    v = {('para_' + k): alb.get_field(rec, 'para_' + k) for k in STAT_KEYS}
    v['g0_tok'] = struct.unpack_from('<h', rec, g0 + 2)[0]
    v['g0_yuu'] = struct.unpack_from('<h', rec, g0 + 6)[0]
    return v


# ------------------------------------------------------------------ recipes
def load_recipes(path=RECIPES):
    out = []
    seen = set()
    for f in sorted(glob.glob(os.path.join(path, '*.json'))):
        with open(f) as fh:
            doc = json.load(fh)
        group = doc['group']
        for r in doc['checkpoints']:
            r = dict(r)
            r['group'] = group
            r['file'] = os.path.relpath(f, ROOT)
            if r['id'] in seen:
                raise CheckpointError(f'{f}: duplicate id {r["id"]}')
            seen.add(r['id'])
            validate(r)
            out.append(r)
    return out


def validate(r):
    for k in ('id', 'date', 'segment'):
        if k not in r:
            raise CheckpointError(f'{r.get("id")}: no {k}')
    y, m, d = ymd(r['date'])
    if r.get('base', 'played') == 'played' and (y, m, d) < PLAYED_DATE:
        raise CheckpointError(f'{r["id"]}: a played-base date before {PLAYED_DATE}')
    kind = r['segment']['kind']
    if kind not in ('week', 'event', 'ending', 'name_entry', 'date'):
        raise CheckpointError(f'{r["id"]}: segment kind {kind}')
    if kind != 'name_entry' and alb.weekday(y, m, d) != 6:
        raise CheckpointError(f'{r["id"]}: {r["date"]} is not a Sunday (the planning night)')
    if 'activity' in r and r['activity'] not in ICON_POS:
        raise CheckpointError(f'{r["id"]}: activity {r["activity"]}')
    seg = r['segment']
    for w in (seg.get('waits', []) + seg.get('before', []) + seg.get('title', []) +
              seg.get('options', []) +
              ([seg['entry']] if seg.get('entry') else [])):
        if isinstance(w, str):
            fragment(w)         # raises when the file is missing
    res = r.get('reserve')
    if kind == 'date' and not res:
        raise CheckpointError(f'{r["id"]}: a date segment needs `reserve`')
    if res:
        dy = ymd(res['day'])
        if alb.weekday(*dy) != 6 or dy <= (y, m, d):
            raise CheckpointError(f'{r["id"]}: reserve day {res["day"]} is not a later Sunday')
        if not 0 <= res['place'] <= 20 or not 0 <= res['girl'] <= 11:
            raise CheckpointError(f'{r["id"]}: reserve girl/place out of range')
    if r.get('choice', 0) not in (0, 1):
        raise CheckpointError(f'{r["id"]}: choice {r["choice"]}')


def fragment(ref):
    """"@NAME" -> the text of pc/tools/checkpoints/play/NAME.txt, with
    `{MASH}` replaced by the press cycle"""
    if not ref.startswith('@'):
        raise CheckpointError(f'a string wait must be "@NAME", not {ref!r}')
    path = os.path.join(FRAGMENTS, ref[1:] + '.txt')
    if not os.path.exists(path):
        raise CheckpointError(f'{ref}: no fragment {os.path.relpath(path, ROOT)}')
    with open(path) as fh:
        text = fh.read()
    return f'# {ref} ({os.path.relpath(path, ROOT)})\n' + text.replace('{MASH}', MASH).rstrip()


def reservation_word(res):
    """the reserved_date u32 tel_dialog20 (state 32) writes for RES"""
    y, m, d = ymd(res['day'])
    return alb.reserved_date(res['girl'], y, m, d, res['place'])


def craft(r, bases):
    """-> (card bytes, record) for recipe R"""
    base = r.get('base', 'played')
    with open(bases[base], 'rb') as fh:
        card = bytearray(fh.read())
    rec = alb.read_record(card, 1)
    if r['segment']['kind'] == 'name_entry':
        return card, rec        # the template as the game wrote it
    date = ymd(r['date'])
    fields = {}
    if r.get('profile', 'typical') == 'typical':
        fields.update(typical(rec, date, start_values(rec)))
    if 'activity' in r:
        act = alb.get_field(rec, 'activity')
        fields['activity'] = (act & ~0xF) | r['activity']
    for k, v in fields.items():
        alb.set_field(rec, k, str(v))
    if 'reserve' in r:
        alb.set_field(rec, 'reserved.0', str(reservation_word(r['reserve'])))
    for k, v in r.get('set', {}).items():
        if isinstance(v, str) and v.startswith('|') and k == 'mouse_pad_st':
            # "|0x..." ORs bits into the SpotFlags word (screen_w+3940:
            # the magazine and season bits tel_spot_menu_set reads)
            v = alb.get_field(rec, k) | int(v[1:], 0)
        elif isinstance(v, str) and v.startswith('|'):
            # "|0x..." ORs bits into a girl's flag word (met, phone, counters)
            _, who, field = k.split('.')
            g = alb.GIRLS.index(who) if who in alb.GIRLS else int(who, 0)
            if field != 'flags':
                raise CheckpointError(f'{r["id"]}: {k}: "|" applies to girl flags '
                                      'and mouse_pad_st only')
            v = girl_flags(rec, g) | int(v[1:], 0)
        alb.set_field(rec, k, str(v))
    alb.apply_date(rec, *date, ram=bases['ram'], symbols=bases['syms'], keep=set(r.get('set', {})))
    alb.write_record(card, 1, rec)
    return card, rec


# ------------------------------------------------------------------ segments
def icon_click(k, label):
    n, m = ICON_COL[ICON_POS[k][1]], ICON_ROW[ICON_POS[k][0]]
    return (f'+30 left+up*80              # park\n'
            f'+82 right*{n}\n'
            f'+{n + 2} down*{m}\n'
            f'wait {label} now_icon:1={k} within {m + 30}\n'
            f'+8 circle\n')


# menu3_init (src/main/matched/menu3_init.c): three rows at x -128, y 48 /
# 64 / 80, 252 x 16; menu3_select stores the picked row's value in serifu_no:
# row 0 junban, row 1 menu3_select_tbl0, row 2 menu3_select_tbl1 (the order
# the date dealt).  A player reads the rows and picks the answer; the script
# reads the same three bytes.  From the clamp corner (-150,-100), right+down
# *37 is (-39,+11); then down*15 / *20 / *26 is the row's middle.
MENU3_ROWS = (('junban', 15), ('menu3_select_tbl0', 20), ('menu3_select_tbl1', 26))


def menu3_answer(i, a, mash):
    """script lines answering the date's three-row menu at sub-step / sub-sub-
    step A['at'] ("S/SS") with the row whose value is A['value']"""
    sub, ssub = (int(x, 0) for x in a['at'].split('/'))
    v = a['value']
    out = [f'wait menu_{i} now_step=0x50 now_sub_step={sub} now_sub_sub_step={ssub} menu_on:4=1 '
           f'within {DATE_FRAMES // 2} mash {mash}',
           f'branch row_{i} within 30']
    for k, (sym, down) in enumerate(MENU3_ROWS):
        out += [f'case row{k}_{i} {sym}:1={v}',
                '  +20 left+up*95',
                '  +100 right+down*37',
                f'  +42 down*{down}']
    out += ['end-branch',
            f'wait answered_{i} now_sub_sub_step!={ssub} within 900 mash circle/20',
            f'check answer_{i} serifu_no:4={v}']
    return '\n'.join(out)


# konami_command_check (src/main/matched/konami_command_check.c, run by
# pad_scan every frame) matches pad_data against the 18 words at 0x800BA198:
# up, 0, up, 0, down, 0, down, 0, left, 0, right, 0, left, 0, right, 0,
# cross, circle -- each within 16 frames of the last match (other values are
# skipped, not reset) -- and raises date2_menu_select_tbl1 for ~240 frames.
KONAMI = ('up', 'up', 'down', 'down', 'left', 'right', 'left', 'right', 'cross', 'circle')


def konami_code(at, mash):
    """script lines entering the Konami command at a date's sub-step /
    sub-sub-step AT ("S/SS"): at the karaoke (place 11) during the greeting
    (date_main0, sub-sub-steps 44..47) it sets date_pattern 2, and the date
    then sings (DATE2, step 92; date_event_sched)"""
    sub, ssub = (int(x, 0) for x in at.split('/'))
    out = [f'wait konami_at now_step=0x50 now_sub_step={sub} now_sub_sub_step={ssub} '
           f'within {DATE_FRAMES // 2} mash {mash}']
    # the first press waits out the mash's last held press (up to 95 frames)
    out += [f'+{100 if i == 0 else 6} {b}' for i, b in enumerate(KONAMI)]
    out.append('wait konami date2_menu_select_tbl1:1=1 within 60')
    return '\n'.join(out)


def calendar_hex(rec):
    return bytes(rec[4:9]).hex() + bytes(rec[10:12]).hex()


def segment(r, rec):
    """the playthrough.py script text for recipe R (crafted record REC)"""
    seg = r['segment']
    kind = seg['kind']
    act = r.get('activity', 5)
    out = [f'# checkpoint {r["id"]} ({r["file"]}): generated by pc/tools/checkpoints.py',
           f'# covers: {", ".join(r.get("covers", [])) or "-"}',
           'hold 2', 'trace ' + (ENDING_TRACE if kind == 'ending' or r.get('light_trace')
                                 else STATE_TRACE),
           f'phase {r["id"]}', load_text(seg.get('title', []), seg.get('options', []))]
    if kind == 'name_entry':
        out.append('wait album_loaded now_step=0x14 now_sub_step=1 now_sub_sub_step=1 within 1800')
        if seg.get('entry'):
            # the entry screens played by a fragment (it starts on the
            # surname screen, album_loaded, and ends at schedule_ready)
            out.append(fragment(seg['entry']))
        else:
            out.append(NAME_ENTRY.split('\n', 1)[1].rstrip())
        out.append('check first_day calendar:7=005f0404010000')
        out.append(icon_click(act, 'planner_icon').rstrip())
        out.append('wait week_start now_step!=0x34 within 600')
        out.append(f'check activity sched_icon:1&0xF={act}')
        out.append(f'wait week_end now_step=0x34 now_sub_step=1 now_month:1=4 now_day:1=9 '
                   f'within {WEEK_FRAMES} mash {MASH}')
        out.append('shot week_end 30')
        out.append('end 60')
        return '\n'.join(out) + '\n'
    out.append('wait album_loaded now_step=0x34 now_sub_step=1 within 1800')
    out.append(f'check loaded_date calendar:7={calendar_hex(rec)}')
    out.append('shot album_loaded 30')
    for ref in seg.get('before', []):
        # on the loaded planner, before its icon is clicked (the OLH help
        # spot, a screen the planner opens and returns from)
        out.append(fragment(ref))
    forced = forced_icon(rec, act)
    if forced is not None:
        act = forced
    out.append(icon_click(act, 'planner_icon').rstrip())
    # a special week (school trip, club camp, festival) answers the click
    # with a message first: tap through it
    out.append('wait week_start now_step!=0x34 within 1200 mash circle/45')
    if forced is None:
        out.append(f'check activity sched_icon:1&0xF={act}')
    date = ymd(r['date'])
    if kind == 'week':
        weeks = seg.get('weeks', 1)
        for w in range(1, weeks + 1):
            y, m, d = add_days(date, 7 * w)
            lab = 'week_end' if w == weeks else f'week_{w}'
            # a later week: the mash's tap on icon 5 starts it (雑学)
            out.append(f'wait {lab} now_step=0x34 now_sub_step=1 now_month:1={m} now_day:1={d} '
                       f'within {WEEK_FRAMES} mash {MASH}')
        out.append('shot week_end 30')
        out.append('end 60')
    elif kind == 'event':
        last = date
        week_mash = MASHES[r.get('mash', 'default')]
        for w in seg['waits']:
            if isinstance(w, str):
                out.append(fragment(w))
                continue
            lab, conds, within, rnd = w[:4]
            mash = w[4] if len(w) > 4 else week_mash
            out.append(f'wait {lab} {conds} within {within} mash {mash}' + (' rand' if rnd else ''))
        nights = seg.get('until')
        if seg.get('end_wait'):
            conds, within, mash = seg['end_wait']
            out.append(f'wait seg_end {conds} within {within}' + (f' mash {mash}' if mash else ''))
        elif nights:
            y, m, d = ymd(nights)
            out.append(f'wait week_end now_step=0x34 now_sub_step=1 now_month:1={m} now_day:1={d} '
                       f'within {WEEK_FRAMES * 2} mash {MASH}')
        else:
            out.append(f'wait week_end now_step=0x34 now_sub_step=1 within {WEEK_FRAMES} '
                       f'mash {week_mash} rand')
        out.append('shot week_end 30')
        out.append('end 60')
        del last
    elif kind == 'ending':
        out.append(f'wait ending now_step=0x63 within {WEEK_FRAMES * 2} mash {MASH}')
        girls = seg.get('rand')
        if girls:
            out.append('branch decided within 1200')
            for g in girls:
                out.append(f'case ending_{alb.GIRLS[g]} now_step=0x63 now_girl:1={g}')
            out.append('end-branch')
        else:
            g = seg['girl']
            out.append(f'wait decided now_step=0x63 now_girl:1={g} within 1200')
        if seg.get('to_title'):
            # on through the staff roll to ETC's save screen (save_file_slot,
            # 0xc4/1/2: menu 1, the album slots, a pointer menu:
            # save_file_slot_init puts slot i at x (i/5)*32-124, y
            # (i%5)*16-64, 32 x 16): circle with the hand on album 1 (slot
            # menu_csr + 1 == now_album) saves it, and save_sys_set writes
            # the system file with mascot_flag[] (the ending girl's flag set:
            # get_option_count, the OPTION extras' rows 1 and 2).  Then the
            # fade, step 17 (which resets RAM), the title.  `build` checks
            # the flag on the card (ensure_cleared).
            out.append(f'wait staff_roll now_step=0xc2 within {ENDING_FRAMES} mash {MASH}')
            out.append(f'wait save_slots now_step=0xc4 now_sub_step=1 now_sub_sub_step=2 '
                       f'within {ENDING_FRAMES} mash circle/45')
            out.append('+30 left+up*100')
            out.append('point -108 -55')
            out.append('wait slot_album1 cursor_cw_x:2=-108 cursor_cw_y:2=-55 menu_csr:1=0 '
                       'within 600')
            out.append('+4 circle')
            out.append(f'wait title now_step=0x12 within {ENDING_FRAMES} mash circle/45')
            out.append('end 600')
        else:
            out.append(f'branch after_ending within {ENDING_FRAMES} mash {MASH}')
            out.append('case staff_roll now_step=0xc2')
            out.append('case ending_save now_step=0xc4')
            out.append('case title now_step=0x12')
            out.append('end-branch')
            out.append('end 120')
    elif kind == 'date':
        y, m, d = ymd(r['reserve']['day'])
        weeks = max(1, (as_date((y, m, d)) - as_date(date)).days // 7)
        out.append(f'wait date_day now_step=0x46 now_sub_step=1 now_month:1={m} now_day:1={d} '
                   f'within {WEEK_FRAMES * weeks} mash {MASH}')
        out.append('check date_reserved reserved_date!=0')
        out.append('shot date_day 30')
        out.append(icon_click(DATE_ICON, 'date_icon').rstrip())
        out.append('wait date_start now_step=0x50 within 1500 mash circle/45')
        mash = DATE_MASH[r.get('choice', 0)]
        if 'konami' in seg:
            out.append(konami_code(seg['konami'], mash))
        for i, a in enumerate(seg.get('answers', [])):
            out.append(menu3_answer(i, a, mash))
        for w in seg.get('waits', []):
            lab, conds, within, rnd = w[:4]
            out.append(f'wait {lab} {conds} within {within} mash {w[4] if len(w) > 4 else mash}'
                       + (' rand' if rnd else ''))
        out.append(f'wait date_home now_step=0x34 now_sub_step=1 within {DATE_FRAMES} mash {mash}')
        out.append('shot date_home 30')
        out.append('end 60')
    return '\n'.join(out) + '\n'


# ------------------------------------------------------------------ ending model
def exe_reader():
    with open(EXE, 'rb') as fh:
        data = fh.read()
    t0 = struct.unpack_from('<I', data, 0x18)[0]

    def rb(addr, n):
        o = addr - t0 + 0x800
        return data[o:o + n]
    return rb


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def girl_percents(rec, rb=None):
    """-> (yuukou %, tokimeki %) per girl 0..12: get_h_yuukou /
    get_h_tokimeki over a crafted record (the
    gauge times the girl's weight, plus the tokimeki stat modifiers)"""
    rb = rb or exe_reader()

    def sbs(a, n):
        return list(struct.unpack('%db' % n, rb(a, n)))
    ystr = list(rb(0x800BA6A0, 13))
    tw = list(rb(0x800BA600, 13))
    tmask = list(rb(0x800BA610, 13))
    tmode = list(rb(0x800BA620, 13))
    mod = [sbs(0x800BA630 + 8 * g, 8) for g in range(13)]
    thr = list(rb(0x800BA698, 7))
    g0 = alb.FIELDS['girl_tbl'][0]
    par = {k: alb.get_field(rec, 'para_' + k) for k in STAT_KEYS}

    def gs(g, off):
        return struct.unpack_from('<h', rec, g0 + 56 * g + off)[0]
    tok = [gs(g, 2) for g in range(13)]
    yuu = [gs(g, 6) for g in range(13)]

    def yuukou_pct(g):
        r = cdiv(yuu[g] * ystr[g], 100)
        return 100 if r >= 101 else (0 if r < 0 else r)

    def toki_pct(g):
        v = cdiv(tok[g] * tw[g], 100)
        mask, mode = tmask[g], tmode[g]
        t = thr
        lims = {1: [t[0] + 50, t[1] + 35, t[2] + 20, t[3] + 20, t[4] + 5, t[5] + 5, t[6]],
                2: t[:7],
                3: [t[0] + 50, t[1] + 50, t[2] + 50, t[3] + 20, t[4] + 20, t[5] + 20, t[6] + 20],
                4: [t[0] + 10, t[1] + 10, t[2] + 10, t[3] + 10, t[4], t[5], t[6]],
                5: [t[0] + 20, t[1] + 20, t[2] + 20, t[3] + 20, t[4], t[5] - 5, t[6]],
                6: [t[0], t[1], t[2], t[3], t[4] - 5, t[5] - 5, t[6] - 5]}.get(mode)
        for b, p in enumerate(('bun', 'ri', 'gei', 'un', 'zat', 'you')):
            if lims and mask & (1 << (b + 1)):
                s = par[p]
                v += next((mod[g][k] for k in range(7) if lims[k] < s), mod[g][7])
        return 100 if v >= 101 else (0 if v < 0 else v)
    return [yuukou_pct(g) for g in range(13)], [toki_pct(g) for g in range(13)]


def zyotai(rec, g):
    """-> (get_g_zyotai_s, get_g_zyotai_h) of girl G (src/main/matched/
    get_g_zyotai_{s,h}.c): s & 0x7F < 2 is "she likes you", s == 0x80 with
    h == 0 the date events' "best" mood"""
    yp, tp = girl_percents(rec)
    y, t = yp[g], tp[g]
    if y < 10:
        s = 4
    elif y < 30:
        s = 3
    elif y < 50:
        s = 3 if t < 50 else 1
    elif y < 80:
        s = 2 if t < 50 else 1
    else:
        s = 0x82 if t < 50 else 0x80
    h = 4 if y < 10 else 3 if y < 50 else 2 if t < 50 else 1 if t < 80 else 0
    return s, h


def ending_model(rec):
    """-> (now_girl, scores): the scoring pipeline (validated on 240+
    emulator states) over a crafted record.  Miharu (a dice roll) is
    reported as 13 (the fallback)."""
    rb = exe_reader()

    def sbs(a, n):
        return list(struct.unpack('%db' % n, rb(a, n)))
    grp = list(rb(0x800BA6B0, 400))
    gp_thr = list(rb(0x800BA840, 8))
    w = sbs(0x800BA848, 12)
    t1, t2 = list(rb(0x800BA854, 12)), list(rb(0x800BA860, 12))
    t3, t4 = sbs(0x800BA86C, 16), sbs(0x800BA87C, 16)
    g0 = alb.FIELDS['girl_tbl'][0]
    par = {k: alb.get_field(rec, 'para_' + k) for k in STAT_KEYS}

    def gs(g, off):
        return struct.unpack_from('<h', rec, g0 + 56 * g + off)[0]
    val = [gs(g, 10) for g in range(13)]
    flags = [list(rec[g0 + 56 * g + 20:g0 + 56 * g + 56]) for g in range(13)]
    met = [girl_flags(rec, g) >> 1 & 1 for g in range(13)]
    byouki = alb.get_field(rec, 'basyo_pack') >> 8 & 0xF
    denwa = rec[g0 + 56 * 12 + 15]
    ev4f = rec[alb.FIELDS['ev_flag'][0] + 0x4F]
    yp, tp = girl_percents(rec, rb)

    def yuukou_pct(g):
        return yp[g]

    def toki_pct(g):
        return tp[g]
    icon = [grp[min(yuukou_pct(g) // 5, 19) + 20 * min(toki_pct(g) // 5, 19)] for g in range(11)]
    sc = [0] * 11
    for k in range(1, 11):
        sc[k] = icon[k] * 4 + val[k] * w[k]
    sc[2] += t4[cdiv(min(par['str'], 140), 10)]
    order = ('kon', 'you', 'zat', 'un', 'gei', 'ri', 'bun')
    for j in range(1, 11):
        for i in range(7):
            for tt in (t1, t2):
                if tt[j] >> (7 - i) & 1:
                    sc[j] += t3[min(cdiv(par[order[i]], 10), 14)]
    for i in range(1, 11):
        s = sum(flags[i][1:36])
        sc[i] = sc[i] + 10 * s if s >= 4 else 0
    s0 = 0
    if icon[0] >= 85 and not byouki and all(
            par[k] >= gp_thr[i] for i, k in enumerate(('bun', 'ri', 'gei', 'un', 'zat', 'you', 'kon'))):
        if sum(flags[0][1:36]) >= 8 and val[0] < 51:
            s0 = 785
    sc[0] = s0
    if sc[0] < 500 and denwa >= 70 and ev4f == 2:
        return 12, sc
    best, now = 499, 13
    for i in range(11):
        if met[i] and best < sc[i]:
            now, best = i, sc[i]
    return now, sc


# ------------------------------------------------------------------ bases
def ensure_bases(out, binary, timeout=1800):
    """--out/base/{template.mcr, schedule.ram, played.mcr}"""
    import scenario
    import subprocess
    d = os.path.join(out, 'base')
    os.makedirs(d, exist_ok=True)
    tpl, ram, played = (os.path.join(d, n) for n in ('template.mcr', 'schedule.ram', 'played.mcr'))
    if not (os.path.exists(tpl) and os.path.exists(ram)):
        card, sram = scenario.ensure_template(os.path.join(out, 'scenario'), binary, timeout)
        shutil.copy(card, tpl)
        shutil.copy(sram, ram)
    if not os.path.exists(played):
        pt_out = os.path.join(d, 'date_save')
        print('[checkpoints] making the played base (date_save.txt on the port)', flush=True)
        r = subprocess.run([sys.executable, os.path.join(HERE, 'playthrough.py'),
                            os.path.join(HERE, 'playthrough/date_save.txt'), '--bin', binary,
                            '--out', pt_out], stdout=subprocess.DEVNULL, timeout=timeout)
        src = os.path.join(pt_out, 'date.card1.mcr')
        if r.returncode != 0 or not os.path.exists(src):
            raise CheckpointError(f'date_save.txt failed ({pt_out}/report.json)')
        with open(src, 'rb') as fh:
            rec = alb.read_record(bytearray(fh.read()), 1)
        got = (alb.get_field(rec, 'year'), alb.get_field(rec, 'month'), alb.get_field(rec, 'day'))
        if got != PLAYED_DATE:
            raise CheckpointError(f'the played base is dated {got}, not {PLAYED_DATE}: '
                                  'date_save.txt took another path; update PLAYED_DATE')
        shutil.copy(src, played)
    with open(ram, 'rb') as fh:
        ram_bytes = fh.read()
    return dict(template=tpl, played=played, ram=ram_bytes, syms=scenario.symbols(binary))


def select(recipes, names):
    if not names:
        return recipes
    out = [r for r in recipes if r['id'] in names or r['group'] in names]
    missing = set(names) - {r['id'] for r in out} - {r['group'] for r in out}
    if missing:
        raise CheckpointError(f'unknown checkpoint or group: {" ".join(sorted(missing))}')
    return out


def check_reservation(r, rec):
    """a reserved girl must be one the player could have phoned (met, number
    known: flags bits 1 and 3, the phone book's test), and a recipe's `mood`
    ("likes": get_g_zyotai_s & 0x7F < 2; "best": s == 0x80 and h == 0, the
    date events' condition) must hold on the crafted record for the reserved
    girl (or `mood_girl`, default Shiori)"""
    res = r.get('reserve')
    if res:
        f = girl_flags(rec, res['girl'])
        if f & (alb.FLAG_MET | alb.FLAG_PHONE) != alb.FLAG_MET | alb.FLAG_PHONE:
            raise CheckpointError(f'{r["id"]}: girl {res["girl"]} not met with her number '
                                  f'(flags 0x{f:x})')
    mood = r.get('mood')
    if mood:
        g = r.get('mood_girl', res['girl'] if res else 0)
        s, h = zyotai(rec, g)
        ok = (s & 0x7F) < 2 if mood == 'likes' else (s == 0x80 and h == 0)
        if not ok:
            raise CheckpointError(f'{r["id"]}: mood {mood} does not hold (zyotai s 0x{s:x}, h {h}, '
                                  f'percents {[p[g] for p in girl_percents(rec)]})')


CLEARED_FROM = 'end_00_shiori_sys'
SYS_MASCOT = 20         # mascot_flag[0..15] in the system record (save_work_restore)


def system_record(card):
    """the system file's record (slot 0, BISLPS-000650): the first of the
    two copies whose checksum holds (save_check)"""
    blk = alb.find_file(card, 'BISLPS-000650')
    data = card[blk * alb.BLOCK:(blk + 1) * alb.BLOCK]
    for off in alb.RECORD_OFFSETS:
        rec = bytearray(data[off:off + alb.RECORD_SIZE])
        if struct.unpack_from('<I', rec, 0)[0] == alb.checksum(rec) and any(rec[4:]):
            return rec
    raise CheckpointError('the system file has no valid record')


def ensure_cleared(out, binary, bases, timeout=3600):
    """--out/base/cleared.mcr: the card CLEARED_FROM's segment leaves on the
    port -- an ending played on through ETC's save screen: save_sys_set
    writes the system file with the ending girl's mascot_flag (measured
    2026-11-01: mascot_flag[0] after Shiori's ending; get_option_count, the
    OPTION extras' rows 1 and 2).  opt_flag is not set on this
    route: etc_rec_collect runs in step 145, which etc_roll_main enters on
    pad bit 22 (the mouse's right button) during the staff roll"""
    import subprocess
    path = os.path.join(out, 'base', 'cleared.mcr')
    if os.path.exists(path):
        return path
    r = [x for x in load_recipes() if x['id'] == CLEARED_FROM][0]
    d = os.path.join(out, 'base', 'cleared')
    os.makedirs(d, exist_ok=True)
    card, rec = craft(r, bases)
    with open(os.path.join(d, 'card.mcr'), 'wb') as fh:
        fh.write(card)
    with open(os.path.join(d, 'segment.txt'), 'w') as fh:
        fh.write(segment(r, rec))
    print(f'[checkpoints] making the cleared base ({CLEARED_FROM} on the port)', flush=True)
    res = subprocess.run([sys.executable, os.path.join(HERE, 'playthrough.py'),
                          os.path.join(d, 'segment.txt'), '--bin', binary, '--out', d,
                          '--start-card', os.path.join(d, 'card.mcr'),
                          '--max-frames', str(r.get('max_frames', MAX_FRAMES['ending']))],
                         stdout=subprocess.DEVNULL, timeout=timeout)
    src = os.path.join(d, f'{CLEARED_FROM}.card1.mcr')
    if res.returncode != 0 or not os.path.exists(src):
        raise CheckpointError(f'{CLEARED_FROM} failed ({d}/report.json)')
    with open(src, 'rb') as fh:
        sysrec = system_record(bytearray(fh.read()))
    if not any(sysrec[SYS_MASCOT:SYS_MASCOT + 14]):
        raise CheckpointError(f'{src}: the system file has no mascot_flag after the clear')
    shutil.copy(src, path)
    return path


def build(recipes, out, binary):
    bases = ensure_bases(out, binary)
    if any(r.get('base') == 'cleared' for r in recipes):
        bases['cleared'] = ensure_cleared(out, binary, bases)
    index = []
    for r in recipes:
        card, rec = craft(r, bases)
        if 'expect_ending' in r:
            got, sc = ending_model(rec)
            if got != r['expect_ending']:
                raise CheckpointError(f'{r["id"]}: the model predicts ending {got} '
                                      f'(scores {sc}), not {r["expect_ending"]}')
        check_reservation(r, rec)
        d = os.path.join(out, r['id'])
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'card.mcr'), 'wb') as fh:
            fh.write(card)
        with open(os.path.join(d, 'segment.txt'), 'w') as fh:
            fh.write(segment(r, rec))
        index.append(dict(id=r['id'], group=r['group'], date=r['date'],
                          card=os.path.join(d, 'card.mcr'), segment=os.path.join(d, 'segment.txt'),
                          covers=r.get('covers', []),
                          max_frames=r.get('max_frames',
                                           MAX_FRAMES.get(r['segment']['kind'], 40000))))
        print(f'[checkpoints] {r["id"]:24s} {r["date"]:9s} {r["segment"]["kind"]}')
    with open(os.path.join(out, 'index.json'), 'w') as fh:
        json.dump(index, fh, indent=1)
    return index


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=('list', 'show', 'build'))
    ap.add_argument('names', nargs='*', help='checkpoint ids or groups (default: all)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/checkpoints'))
    ap.add_argument('--bin', default=PC_BIN)
    a = ap.parse_args(argv)
    try:
        recipes = select(load_recipes(), a.names)
        if a.cmd == 'list':
            for r in recipes:
                print(f'{r["group"]:8s} {r["id"]:24s} {r["date"]:9s} {r["segment"]["kind"]:10s} '
                      f'{", ".join(r.get("covers", []))}')
            return 0
        if a.cmd == 'show':
            bases = ensure_bases(os.path.abspath(a.out), os.path.abspath(a.bin))
            for r in recipes:
                card, rec = craft(r, bases)
                path = os.path.join(a.out, 'show.mcr')
                with open(path, 'wb') as fh:
                    fh.write(card)
                print(f'== {r["id"]}')
                import subprocess
                subprocess.run([sys.executable, os.path.join(HERE, 'mcr_tool.py'), 'album-show', path, '1'])
                now, sc = ending_model(rec)
                print(f'  ending model: now_girl {now} ({alb.GIRLS[now]}), scores {sc}')
            return 0
        build(recipes, os.path.abspath(a.out), os.path.abspath(a.bin))
        return 0
    except (CheckpointError, alb.AlbumError) as e:
        print(f'checkpoints: {e}')
        return 1


if __name__ == '__main__':
    sys.exit(main())
