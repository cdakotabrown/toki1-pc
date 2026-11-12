#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Explorer: seeded random, menu-aware pad input on the port, looking for faults.

Port only, no reference.  Each run boots the port from power-on,
optionally plays a fixed
prefix through the game's own screens (a new game to the first school day,
or the title's Continue -> album 1 on a crafted card), then presses random
buttons for the rest of its frame budget.  Nothing here changes game state:
the input is a pc/tools/playthrough.py script (PC_INPUT), the only fixture
is the memory card the game itself loads.

Starts: `title`, `newgame`, `album:DATE` (an album crafted here), and
`card:ID` -- a checkpoint card of pc/tools/checkpoints.py (`build` first;
`cards` = every checkpoint, `cards:GROUP` one group): the segment's own
Continue -> album 1 lines up to `wait album_loaded`, then random input
from the loaded planner where the segment would play its scripted week.
`--resume` keeps the runs an earlier invocation finished into --out.

    python3 pc/tools/explorer.py --seeds 1-16 --starts title,newgame,album:95/7/22 \\
        --frames 100000 --jobs 4 --out build/explore [--bin B] [--reach]
    python3 pc/tools/explorer.py --seeds 1-3 --starts cards --frames 50000 --jobs 3 \\
        --reach --resume --bin build/coverage/tokimeki-reach --out build/explore-reach
    python3 pc/tools/explorer.py --list-starts
    python3 pc/tools/explorer.py --seeds 3 --starts newgame --frames N   # the same run again

A run is also replayed by hand: PC_INPUT=<out>/runs/<job>/explore.input with
the card in saves/ (album starts: <out>/cards/), PC_PARITY_DIR set (any
directory; the game-thread mix) and PC_TRACE_FRAMES=<frames>; or
`playthrough.py <out>/runs/<job>/explore.txt` (--retail for the reference,
title / newgame starts: the reference's card is a fresh one).

Input (generate()): a stream of player "actions" drawn from one
random.Random per (start, seed): confirm taps (circle) most often, cancel
(cross), aims (park the hand in the clamp corner, then one of the planner's
icons -- the forced icons 14 and 4 weighted up --, a pointer menu's line,
a cell of the girl menu or anywhere, then circle), the hand cursor/menu
cursor moved by a held direction (the hand moves 3 px per held VBlank)
usually followed by a confirm, a slow sweep of circles along one row of
TAIIKU's cheering-cue seats, d-pad taps,
START (takes the defaults on entry screens), the other face and shoulder
buttons rarely, bursts of confirm taps (dialogue), short pauses between
actions and occasionally a long idle (voice lines, the game's own timers).
Never START+SELECT or the shoulder chords; the run is never ended early
by its input.  Times are VBlanks after the previous press (`clock vblank`,
as the playthroughs).

Same build + same start + same seed => the same run: the input is a pure
function of (start, seed), the audio is mixed on the game thread
(PC_PARITY_DIR with no anchor, as playthrough.py's default) and nothing
else the game reads depends on the host.  --check-determinism runs a job
twice and compares the per-VBlank traces.

Detected (per run, reported per distinct site across runs):
  crash     a [CRASH] recovery (SIGSEGV/SIGBUS/SIGFPE, and SIGALRM: the 5 s
            per-frame watchdog, pc/hal/watchdog.c) or death by a signal
  fold      a [FOLD] site (a sign-extended PS1 address reached a dereference)
  ubsan     a `runtime error:` report (run a UBSan build:
            build_compare.py --variant ... --cflags=-fsanitize=undefined)
  hang      frame_counter stops while VBlanks go on for --hang-vblanks, or
            the process writes no trace row for --stall-sec of wall time
  stuck     one (now_step, sub, sub-sub) for --stuck-vblanks VBlanks while
            the input keeps pressing
  exit      the port ended with a non-zero status before its budget
            (a prefix milestone not reached: exit 3, reported as `setup`)

Each finding records start, seed, game frame and VBlank, the script that
reproduces it (runs/<job>/explore.txt: playthrough.py format, and its
numeric PC_INPUT explore.input), and with --repro a save state ~600 game
frames before it (a second run with PC_SAVESTATE, which also confirms the
finding reproduces; the toolkit runs without address randomisation, so a
finding that depends on the host layout can fail to repeat there -- the
report says so).

Reach (--reach, a reach-instrumented binary: audit_ps1_wide_access.py
--build-reach): every run appends the functions it entered to
runs/<job>/reach.list; the union is <out>/reach.list (one function name per
line, sorted -- the plain format coverage tools union).

Output: <out>/findings.json, <out>/findings.md (one row per distinct site),
<out>/runs/<job>/{explore.txt, explore.input, run.log, scenes.txt, result.json}.
The full per-VBlank trace is kept only for runs with a finding
(trace.txt.gz, the last 20,000 rows).
"""
import argparse
import gzip
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playthrough as pt  # noqa: E402

ROOT = pt.ROOT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import decomp_key, disc_input  # noqa: E402
B = pt.BUTTONS

# ------------------------------------------------------------------ starts

# new_game.txt's first phase up to the first school day's schedule screen
# (title -> album 1 -> system file -> names -> birthday -> prologue)
NEWGAME_UNTIL = 'wait schedule_ready'

# A crafted album (pc/tools/tokimeki_album.py over the card new_game.txt
# writes) loaded through the title's Continue: pc/tools/scenario.py's path.
ALBUMS = {
    '95/5/15': [],
    '95/7/22': ['club=tennis'],
    '95/12/24': ['club=baseball', 'girl.shiori.flags=2', 'girl.shiori.tokimeki=60'],
    '96/2/14': ['club=brass', 'girl.shiori.flags=2', 'girl.mio.flags=2', 'girl.saki.flags=2'],
    '96/8/10': ['club=soccer', 'para_tai=120', 'para_bun=120'],
    '97/6/1': ['club=literature', 'para_bun=300'],
    '97/10/20': ['club=computer', 'girl.yuina.flags=2'],
    '98/2/20': ['club=tennis', 'girl.shiori.flags=2', 'girl.shiori.tokimeki=128',
                'girl.shiori.yuukou=128', 'para_tai=300', 'para_bun=300', 'para_ri=300'],
}


# A checkpoint card (pc/tools/checkpoints.py build): the segment's own
# Continue -> album 1 lines up to `wait album_loaded`, then random input
# from the loaded planner instead of the segment's scripted week.
CHECKPOINTS = os.path.join(ROOT, 'build/checkpoints')


_INDEX = {}


def checkpoint_index(cdir=None):
    """the checkpoints.py index, read once per run (a `checkpoints.py build`
    of some ids rewrites it with those only)"""
    path = os.path.join(cdir or CHECKPOINTS, 'index.json')
    if path not in _INDEX:
        if not os.path.exists(path):
            return []
        with open(path) as fh:
            _INDEX[path] = json.load(fh)
    return _INDEX[path]


def start_names():
    return (['title', 'newgame'] + ['album:' + d for d in ALBUMS] +
            ['card:' + c['id'] for c in checkpoint_index()])


def expand_starts(text):
    """`all`, `cards` (every built checkpoint), `cards:GROUP` (one
    checkpoints.py group) or a comma list of start names"""
    if text == 'all':
        return start_names()
    out = []
    for s in (s for s in text.split(',') if s):
        if s == 'cards' or s.startswith('cards:'):
            grp = s.split(':', 1)[1] if ':' in s else None
            out += ['card:' + c['id'] for c in checkpoint_index()
                    if grp is None or c['group'] == grp]
        else:
            out.append(s)
    return out


def checkpoint_entry(cid):
    for c in checkpoint_index():
        if c['id'] == cid:
            return c
    # a `checkpoints.py build ID...` (coverage_suite.py run does one)
    # rewrites index.json with those ids only; the cards stay built
    d = os.path.join(CHECKPOINTS, cid)
    if os.path.exists(os.path.join(d, 'segment.txt')) and os.path.exists(os.path.join(d, 'card.mcr')):
        return dict(id=cid, card=os.path.join(d, 'card.mcr'), segment=os.path.join(d, 'segment.txt'))
    raise SystemExit('no checkpoint %s in %s (pc/tools/checkpoints.py build)' % (cid, CHECKPOINTS))


def checkpoint_prefix(segment_text):
    """the segment script's load: its lines after `phase` through the first
    `wait album_loaded`, without checks, shots or trace settings"""
    out, on = [], False
    for raw in segment_text.splitlines():
        line = raw.split('#', 1)[0].strip()
        if line.startswith('phase '):
            on = True
            continue
        if not on or not line or line.startswith(('shot', 'check', 'trace', 'hold')):
            continue
        out.append(line)
        if line.startswith('wait album_loaded'):
            return out
    raise SystemExit('checkpoint segment: no `wait album_loaded`')


def newgame_prefix():
    out = []
    with open(os.path.join(ROOT, 'pc/tools/playthrough/new_game.txt')) as fh:
        lines = fh.read().splitlines()
    on = False
    for raw in lines:
        line = raw.split('#', 1)[0].strip()
        if line.startswith('phase new_game'):
            on = True
            continue
        if not on or not line or line.startswith(('shot', 'check')):
            continue
        out.append(line)
        if line.startswith(NEWGAME_UNTIL):
            return out
    raise SystemExit('new_game.txt: no %r' % NEWGAME_UNTIL)


def prefix_for(start):
    if start == 'title':
        return []
    if start == 'newgame':
        return newgame_prefix()
    if start.startswith('album:'):
        import scenario
        return [l.split('#', 1)[0].strip() for l in scenario.LOAD_ALBUM_1.splitlines() if l.strip()]
    if start.startswith('card:'):
        with open(checkpoint_entry(start.split(':', 1)[1])['segment']) as fh:
            return checkpoint_prefix(fh.read())
    raise SystemExit('unknown start %r (--list-starts)' % start)


def album_card(start, out, binary, timeout):
    """the card for an album: start (scenario.py's template + album edits)"""
    import scenario
    date = start.split(':', 1)[1]
    if date not in ALBUMS:
        raise SystemExit('no album %s (known: %s)' % (date, ' '.join(ALBUMS)))
    # a reach binary without REACH_LIST_FILE logs every overlay call (OVL
    # trace mode) and crawls; the template run gets a throwaway list
    os.makedirs(os.path.join(out, 'scenarios'), exist_ok=True)
    os.environ['REACH_LIST_FILE'] = os.path.join(out, 'scenarios', 'template.reach')
    try:
        card, ram = scenario.ensure_template(os.path.join(out, 'scenarios'), binary, timeout)
    finally:
        del os.environ['REACH_LIST_FILE']
    dest = os.path.join(out, 'cards', date.replace('/', '-') + '.mcr')
    if not os.path.exists(dest):
        sc = dict(album=['date=' + date] + ALBUMS[date])
        scenario.build_card(sc, card, ram, scenario.symbols(binary), dest)
    return dest


# ------------------------------------------------------------------- input

DIRS = ['up', 'down', 'left', 'right']

# the planner's icon centres (the raster sweep of the hand):
# columns x -138 / -114 / -90 (icon 14: -66), rows y -43 / -28 / -10
# / 14 / 29; row 1 has no middle icon.  Index = icon number.
PLANNER_ICONS = ([(x, -43) for x in (-138, -114, -90)] + [(-138, -28), (-90, -28)] +
                 [(x, y) for y in (-10, 14) for x in (-138, -114, -90)] +
                 [(x, 29) for x in (-138, -114, -90, -66)])
# A forced week (exam, school trip, Koshien: icon 14; club camp, culture
# festival: icon 4) leaves one icon that leaves the planner
# (step34_init2: icon_can_use_clear(1) then icon_can_use_set(14 or 4)).
# The hand decides the icon (icon_cursor_move -> menu_check -> now_icon),
# icon_select_check then needs circle on an enabled now_icon.  Drawn
# uniformly, icon 14 came up 0 times in 104 aims of a 50,000-frame run
# (card-date_nozomi_bowling, 96/12/8: icons 11..14 enabled, the run sat on
# the planner); the weights give 14 and 4 a third of the centre aims.
PLANNER_WEIGHTS = [1] * 15
PLANNER_WEIGHTS[14] = 5
PLANNER_WEIGHTS[4] = 2
# girl_menu_set / girl_menu_select (the New Year's shrine, the school trip,
# holiday_switch0): girl i at x (i/3)*64-128, y (i%3)*16+48, a met girl's
# width in girl_menu_select_tbl0.  Four columns: card-g09_megumi sat on
# 53/02/0a with girls 0, 8 and 11 met (widths 28, 42, 42); the pointer-menu
# aim (x -126..-9) never reaches columns 2 and 3.
GIRL_MENU = [((i // 3) * 64 - 128 + 12, (i % 3) * 16 + 56) for i in range(12)]
# taiiku_ouen_cue (TAIIKU's relay, event 13; race3 case 1 waits for a hit
# with no time limit): circle with the hand on the runner's seat, at least
# 41 race frames after the previous evaluated circle (D_801493FF).  Seat s
# = row s/10, column s%10; rows are y bands -48..-1 / -64..-49 / -80..-65,
# column i covers x -170+32i..-140+32i (row 1: +16).
CUE_ROWS = [(-24, 0), (-56, 16), (-72, 0)]
DIAG = ['up+left', 'up+right', 'down+left', 'down+right']
PARK = (-150, -100)


def generate(start, seed, vblanks):
    """-> the random part of the script: playthrough `+N BUTTONS[*H]` lines
    covering at least `vblanks` VBlanks.  A pure function of its arguments."""
    rng = random.Random('explore:%s:%d' % (start, seed))
    out, t = [], 0
    last = [2]      # the previous press's hold: `+N` counts from its START

    def press(gap, buttons, hold=None):
        """`gap` VBlanks after the previous press is released (and after
        any idle added to t since)"""
        nonlocal t
        out.append('+%d %s%s' % (last[0] + gap + idle[0], buttons, '*%d' % hold if hold else ''))
        idle[0] = 0
        last[0] = hold or 2
        t += gap + (hold or 2)
    idle = [0]

    def park(gap):
        # the top-left clamp corner (step_directional_scroll_once: the hand
        # moves csr_speed = 3 px per held VBlank, clamped to -150..140 /
        # -100..100).  100 VBlanks: 80 (the playthroughs' park, 240 px)
        # leaves the hand short of the corner after an aim right of x -100
        press(gap, 'left+up', 100)

    def to(x, y):
        """from the parked corner to (x, y), 3 px per held VBlank"""
        nx, ny = max(0, (x - PARK[0]) // 3), max(0, (y - PARK[1]) // 3)
        if nx:
            press(rng.randint(2, 6), 'right', nx)
        if ny:
            press(rng.randint(2, 6), 'down', ny)

    while t < vblanks:
        gap = rng.randint(6, 40) if rng.random() < 0.7 else rng.randint(40, 240)
        r = rng.random()
        if r < 0.25:                       # confirm
            press(gap, 'circle')
        elif r < 0.32:                     # cancel
            press(gap, 'cross')
        elif r < 0.50:
            # aim the hand as the playthroughs do: park it in the top-left
            # clamp corner, then a target and decide.  A free random walk
            # spent 36,000 VBlanks right of the menus on one night schedule;
            # with aims at 11 % of the actions a run made one about every
            # 2,000 VBlanks and the stuck screens (GEKO's walk-home menu2,
            # 89/0/0x16: 0 pointer aims in 36,000 VBlanks) waited on them.
            park(gap)
            aim = rng.random()
            if aim < 0.35:
                # the planner's icon grid (x -150..-54, y -46..32)
                if rng.random() < 0.6:
                    x, y = rng.choices(PLANNER_ICONS, PLANNER_WEIGHTS)[0]
                    to(x, y)
                else:
                    press(rng.randint(2, 6), 'right', rng.randint(1, 32))
                    press(rng.randint(2, 6), 'down', rng.randint(16, 46))
            elif aim < 0.75:
                if rng.random() < 0.5:
                    # the pointer menus' lines (menu_sel / menu2 / menu3:
                    # x -128..-8, y 48..90 = right 8..47, down 49..64)
                    press(rng.randint(2, 6), 'right', rng.randint(8, 47))
                    press(rng.randint(2, 6), 'down', rng.randint(49, 64))
                else:
                    to(*rng.choice(GIRL_MENU))
            else:
                press(rng.randint(2, 6), 'right', rng.randint(1, 40) if rng.random() < 0.7
                      else rng.randint(1, 100))
                press(rng.randint(2, 6), 'down', rng.randint(1, 70))
            press(rng.randint(4, 20), 'circle')
        elif r < 0.58:                     # move the hand / a cursor, then maybe confirm
            d = rng.choice(DIRS) if rng.random() < 0.75 else rng.choice(DIAG)
            press(gap, d, rng.choice((1, 2, 3, 5, 8, 12, 20, 30, 45, 70)))
            if rng.random() < 0.6:
                press(rng.randint(4, 20), 'circle')
        elif r < 0.60:
            # rapid tapping (the festival's ball race moves on taps)
            k = rng.randint(3, 8)
            for i in range(rng.randint(20, 120)):
                press(gap if i == 0 else k, 'circle')
        elif r < 0.615:
            # a slow sweep along one row of the cheering cue's seats: a
            # circle on each column, 45..60 VBlanks apart (the cue counts
            # 41 frames from the previous evaluated press; card-m96_06
            # sat 36,000 VBlanks on 72/03/01 under the faster taps)
            y, off = rng.choice(CUE_ROWS)
            park(gap)
            to(PARK[0], y)
            x = PARK[0]
            for i in range(10):
                tx = max(PARK[0], -155 + off + 32 * i)
                n = (tx - x) // 3
                if n > 0:
                    press(rng.randint(2, 6), 'right', n)
                    x += 3 * n
                press(rng.randint(45, 60), 'circle')
        elif r < 0.70:                     # menu cursor taps
            d = rng.choice(DIRS)
            for _ in range(rng.randint(1, 4)):
                press(gap if _ == 0 else rng.randint(6, 16), d)
            if rng.random() < 0.5:
                press(rng.randint(4, 20), 'circle')
        elif r < 0.75:                     # START (entry screens: take the defaults)
            press(gap, 'start')
        elif r < 0.79:                     # the other face buttons
            press(gap, rng.choice(('triangle', 'square')))
        elif r < 0.81:                     # shoulders / select, one at a time
            press(gap, rng.choice(('l1', 'r1', 'l2', 'r2', 'select')))
        elif r < 0.92:                     # tap through dialogue
            k = rng.randint(15, 60)
            for i in range(rng.randint(3, 25)):
                press(gap if i == 0 else k, 'circle')
        elif r < 0.985:                    # wait a while
            n = rng.randint(120, 900)
            t += n
            idle[0] += n
        else:                              # a long idle (timers, voice, movies)
            n = rng.randint(900, 4000)
            t += n
            idle[0] += n
    return out


def script_text(start, seed, frames):
    """the whole run as a playthrough.py script (the reproducer)"""
    lines = ['# pc/tools/explorer.py start=%s seed=%d frames=%d' % (start, seed, frames),
             'hold 2', 'phase explore']
    lines += prefix_for(start)
    # VBlanks >= game frames (a frame that overruns takes two); 4x covers
    # the movies and the slow scenes
    lines += generate(start, seed, frames * 4)
    return '\n'.join(lines) + '\n'


def numeric_input(text, path_txt, path_input):
    with open(path_txt, 'w') as fh:
        fh.write(text)
    script = pt.parse(path_txt)
    with open(path_input, 'w') as fh:
        fh.write(pt.numeric_phase(script['phases'][0], script['hold'], False))


# ------------------------------------------------------------------ one run

ROW = re.compile(r'frame=(\d+) pad=([0-9a-f]+) .*?now_step=(\w\w) now_sub_step=(\w\w) '
                 r'now_sub_sub_step=(\w\w) .*?frame_counter=(\w{8}).* crashes=(\d+)')
CRASH = re.compile(r'^\[CRASH\] sig=(\d+) pc=\S+ sym=(\S+) fault_addr=(\S+) scene=(\d+) sub=(\d+)', re.M)
FOLD = re.compile(r'^\[FOLD\] site (\S+) value=(\S+) scene=(\d+) sub=(\d+)', re.M)
UBSAN = re.compile(r'^(\S+?):(\d+):(\d+): runtime error: (.*)$', re.M)
MARK = re.compile(r'^\[MARK\] game frame (\d+) vblank (\d+) step (\S+)', re.M)
PREFIX = re.compile(r'\[TRACE\] PC_INPUT wait (\S+) reached: game frame (\d+) vblank (\d+)')


def le32(h):
    return int.from_bytes(bytes.fromhex(h), 'little')


class Watch:
    """the per-VBlank trace, read as it grows: scene log, hang/stuck checks"""

    def __init__(self, a, keep_rows=20000):
        self.a = a
        self.rows = deque(maxlen=keep_rows)
        self.scenes = []           # (vblank, frame_counter, 'ss/ss/ss')
        self.fc = -1
        self.fc_since = 0
        self.key = None
        self.key_since = 0
        self.presses_in_key = 0
        self.crashes = 0
        self.crash_rows = []        # (vblank, frame_counter, scene)
        self.vblank = 0
        self.problem = None
        self.last_pad = 0

    def row(self, line):
        m = ROW.match(line)
        if not m:
            return
        self.rows.append(line)
        vb, pad = int(m.group(1)), int(m.group(2), 16)
        key = '%s/%s/%s' % (m.group(3), m.group(4), m.group(5))
        fc, crashes = le32(m.group(6)), int(m.group(7))
        self.vblank = vb
        if fc != self.fc:
            self.fc, self.fc_since = fc, vb
        if key != self.key:
            self.scenes.append((vb, fc, key))
            self.key, self.key_since, self.presses_in_key = key, vb, 0
        if pad and not self.last_pad:
            self.presses_in_key += 1
        self.last_pad = pad
        if crashes > self.crashes:
            self.crash_rows.append((vb, fc, key))
            self.crashes = crashes
        if self.problem:
            return
        if vb - self.fc_since > self.a.hang_vblanks:
            self.problem = dict(kind='hang', site='frame_counter stopped in %s' % key,
                                vblank=self.fc_since, frame=fc, scene=key,
                                detail='no game frame for %d VBlanks' % (vb - self.fc_since))
        elif vb - self.key_since > self.a.stuck_vblanks and self.presses_in_key >= 20:
            self.problem = dict(kind='stuck', site='scene %s' % key, vblank=self.key_since,
                                frame=fc, scene=key,
                                detail='%d VBlanks, %d presses' % (vb - self.key_since,
                                                                  self.presses_in_key))
        elif self.crashes > self.a.max_crashes:
            self.problem = dict(kind='crash-storm', site='scene %s' % key,
                                vblank=vb, frame=fc, scene=key, detail='%d recoveries' % self.crashes)


def run_env(a, work, inp, trace, extra=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'REACH_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               PC_TRACE=trace, PC_TRACE_PHASE='pre_irq', PC_INPUT=inp,
               PC_TRACE_FRAMES=str(a.frames), TOKI_INPUT_DEVICE='pad',
               PC_MARK_EVERY='60',
               # deterministic: the game-thread audio mix (playthrough.py)
               PC_PARITY_DIR=os.path.join(work, 'parity'), PC_PARITY_FRAMES='1',
               UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=0')
    if a.reach:
        # never run a reach binary without a list (OVL trace mode crawls)
        env['REACH_LIST_FILE'] = os.path.join(work, 'reach.discard')
    env.update(extra or {})
    env.update(dict(kv.split('=', 1) for kv in getattr(a, 'set', None) or []))
    return env


def prepare_work(a, job, work):
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, 'pc'))
    os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
    os.symlink(str(disc_input('disks')), os.path.join(work, 'disks'))
    if job['start'].startswith(('album:', 'card:')):
        os.makedirs(os.path.join(work, 'saves'))
        shutil.copy(job['card'], os.path.join(work, 'saves/card1.mcr'))


def execute(a, job, work, env, log_path, trace, budget_frames):
    """run the port, watching its trace -> (returncode, Watch, reason)"""
    w = Watch(a)
    with open(log_path, 'wb') as log:
        proc = subprocess.Popen([a.bin, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        pos, part, last_growth, reason = 0, b'', time.monotonic(), None
        t0 = time.monotonic()
        while True:
            rc = proc.poll()
            got = b''
            if os.path.exists(trace):
                with open(trace, 'rb') as fh:
                    fh.seek(pos)
                    got = fh.read()
                pos += len(got)
            if got:
                last_growth = time.monotonic()
                data = part + got
                lines = data.split(b'\n')
                part = lines.pop()
                for line in lines:
                    w.row(line.decode('ascii', 'replace'))
            if rc is not None:
                break
            if w.problem:
                reason = w.problem['kind']
            elif time.monotonic() - last_growth > a.stall_sec:
                reason = 'stall'
                w.problem = dict(kind='hang', site='no trace row for %d s (scene %s)' % (a.stall_sec, w.key),
                                 vblank=w.vblank, frame=w.fc, scene=w.key,
                                 detail='process stopped writing VBlank rows')
            elif time.monotonic() - t0 > a.timeout:
                reason = 'timeout'
            elif os.path.getsize(log_path) > a.max_log:
                reason = 'log-limit'
            if reason:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
                rc = proc.returncode
                break
            time.sleep(0.5)
    return rc, w, reason


def scan_log(text):
    """-> findings from the merged log (crashes, folds, UBSan), each with
    the game frame of the [MARK] before it"""
    marks = [(m.start(), int(m.group(1)), int(m.group(2)), m.group(3)) for m in MARK.finditer(text)]
    starts = [m[0] for m in marks]
    import bisect

    def when(off):
        i = bisect.bisect_right(starts, off) - 1
        return (marks[i][1], marks[i][2], marks[i][3]) if i >= 0 else (0, 0, '?')
    out = []
    for m in CRASH.finditer(text):
        fr, vb, sc = when(m.start())
        sig = int(m.group(1))
        kind = 'watchdog' if sig == signal.SIGALRM else 'crash'
        site = m.group(2).split('+')[0] if kind == 'crash' else 'scene %s' % sc
        if kind == 'crash' and site.startswith('pc='):
            # an unnamed pc (a wild jump, a libc leaf; its address moves with
            # ASLR): key on the caller -- the handler's RA line just before,
            # else the first named game frame of the backtrace after
            before = text[max(0, m.start() - 400):m.start()].splitlines()
            ra = [l for l in before if l.startswith('[CRASH] caller via RA: ')]
            bt = re.search(r'tokimeki-pc\((\w+)\+0x[0-9a-f]+\)', text[m.end():m.end() + 2000])
            if ra and not ra[-1].split(': ', 1)[1].startswith('0x'):
                site = 'via ' + ra[-1].split(': ', 1)[1].split('+')[0]
            elif bt:
                site = 'via ' + bt.group(1)
        out.append(dict(kind=kind, site=site, frame=fr, vblank=vb, scene=sc,
                        detail='sig %d at %s fault %s' % (sig, m.group(2), m.group(3))))
    for m in FOLD.finditer(text):
        fr, vb, sc = when(m.start())
        out.append(dict(kind='fold', site=m.group(1), frame=fr, vblank=vb, scene=sc,
                        detail='value %s' % m.group(2)))
    for m in UBSAN.finditer(text):
        fr, vb, sc = when(m.start())
        f = m.group(1)
        if os.path.isabs(f) and decomp_key(f) != os.path.normpath(f):
            f = decomp_key(f)                    # a toki1-decomp file
        elif os.path.isabs(f) and f.startswith(ROOT + os.sep):
            f = os.path.relpath(f, ROOT)
        f = f[f.find('pc/') if 'pc/' in f else (f.find('src/') if 'src/' in f else 0):]
        msg = re.sub(r'0x[0-9a-f]+', '0x..', m.group(4))
        out.append(dict(kind='ubsan', site='%s:%s:%s' % (f, m.group(2), m.group(3)), frame=fr,
                        vblank=vb, scene=sc, detail=msg[:200], cat=ubsan_class(f, msg)))
    return out


def ubsan_class(path, msg):
    """'<check> <layer>': the UBSan check and where the code lives, the
    triage key (matched game C reproduces the console's own accesses; the
    PC layers are this port's code)"""
    if 'insufficient space for an object' in msg:
        check = 'object-size'
    elif 'out of bounds' in msg:
        check = 'index'
    elif 'shift' in msg:
        check = 'shift'
    elif 'overflow' in msg:
        check = 'overflow'
    elif 'null pointer' in msg:
        check = 'null'
    elif 'misaligned' in msg:
        check = 'alignment'
    elif 'not a valid value' in msg:
        check = 'invalid-value'
    else:
        check = 'other'
    if path.startswith('src/main/pc-port') or path.startswith('pc/src/overlay_inits'):
        layer = 'pc-port'
    elif path.startswith('pc/src/overlay_'):
        layer = 'overlay'
    elif path.startswith('src/main/'):
        layer = 'main'
    else:
        layer = 'pc'
    return '%s %s' % (check, layer)


def run_job(a, job):
    name = job['name']
    d = os.path.join(a.out, 'runs', name)
    done = os.path.join(d, 'result.json')
    if a.resume and os.path.exists(done):
        # --resume: a run this --out already finished (same start, seed and
        # budget) is read back, not repeated
        with open(done) as fh:
            res = json.load(fh)
        if res.get('budget', a.frames) == a.frames:
            return res
    work = os.path.join(d, 'work')
    prepare_work(a, job, work)
    txt, inp = os.path.join(d, 'explore.txt'), os.path.join(d, 'explore.input')
    numeric_input(script_text(job['start'], job['seed'], a.frames), txt, inp)
    trace, log = os.path.join(d, 'trace.txt'), os.path.join(d, 'run.log')
    extra = {}
    if a.reach:
        rl = os.path.join(d, 'reach.list')
        if os.path.exists(rl):
            os.remove(rl)
        extra['REACH_LIST_FILE'] = rl
    t0 = time.monotonic()
    rc, w, reason = execute(a, job, work, run_env(a, work, inp, trace, extra), log, trace, a.frames)
    wall = time.monotonic() - t0
    with open(log, errors='replace') as fh:
        text = fh.read()
    found = scan_log(text)
    # a crash's exact frame: the trace row whose crash count rose
    crashes = [f for f in found if f['kind'] in ('crash', 'watchdog')]
    for f, (vb, fc, sc) in zip(crashes, w.crash_rows):
        f.update(vblank=vb, frame=fc, scene=sc)
    if w.problem:
        found.append(dict(w.problem))
    prefix = {m.group(1): int(m.group(2)) for m in PREFIX.finditer(text)}
    setup = None
    if 'NOT reached' in text:
        setup = re.search(r'PC_INPUT wait (\S+) NOT reached', text).group(1)
    if rc is not None and rc < 0 and reason is None:
        found.append(dict(kind='signal', site='signal %d' % -rc, frame=w.fc, vblank=w.vblank,
                          scene=w.key, detail='the process died'))
    elif rc not in (0, None) and reason is None and not setup:
        found.append(dict(kind='exit', site='exit %d' % rc, frame=w.fc, vblank=w.vblank,
                          scene=w.key, detail=''))
    with open(os.path.join(d, 'scenes.txt'), 'w') as fh:
        for vb, fc, key in w.scenes:
            fh.write('%d %d %s\n' % (vb, fc, key))
    res = dict(name=name, start=job['start'], seed=job['seed'], card=job.get('card'),
               budget=a.frames, rc=rc, reason=reason, wall=round(wall, 1),
               frames=w.fc, vblanks=w.vblank, prefix_done=bool(prefix) or job['start'] == 'title',
               setup_failed=setup, scenes=sorted({k for _, _, k in w.scenes}), findings=found)
    if found or setup:
        with gzip.open(os.path.join(d, 'trace.txt.gz'), 'wt') as fh:
            fh.write('\n'.join(w.rows) + '\n')
    if os.path.exists(trace):
        os.remove(trace)
    shutil.rmtree(os.path.join(work, 'parity'), ignore_errors=True)
    if a.repro and found:
        res['repro'] = repro(a, job, d, found)
    with open(os.path.join(d, 'result.json'), 'w') as fh:
        json.dump(res, fh, indent=1)
    print('[explore] %-22s %-12s game frames %6d  vblanks %6d  %5.0fs  findings %d%s' % (
        name, reason or ('exit %s' % rc), w.fc, w.vblank, wall, len(found),
        '  (setup: %s not reached)' % setup if setup else ''), flush=True)
    return res


def repro(a, job, d, found):
    """re-run to just past the first finding with a save state ~600 game
    frames before it; -> dict(state, reproduced)"""
    first = min(found, key=lambda f: f['frame'])
    at = max(1, first['frame'] - 600)
    state = os.path.join(d, 'before.tks')
    work = os.path.join(d, 'work-repro')
    prepare_work(a, job, work)
    trace, log = os.path.join(d, 'repro.trace'), os.path.join(d, 'repro.log')

    class A2:
        pass
    a2 = A2()
    a2.__dict__.update(vars(a))
    a2.frames = first['frame'] + 120
    env = run_env(a2, work, os.path.join(d, 'explore.input'), trace,
                  dict(PC_SAVESTATE='%d:%s' % (at, state)))
    rc, w, reason = execute(a2, job, work, env, log, trace, a2.frames)
    with open(log, errors='replace') as fh:
        again = scan_log(fh.read())
    if w.problem:
        again.append(w.problem)
    same = [f for f in again if f['kind'] == first['kind'] and f['site'] == first['site']]
    shutil.rmtree(work, ignore_errors=True)
    out = dict(state=state if os.path.exists(state) else None, state_frame=at,
               finding='%s %s' % (first['kind'], first['site']),
               reproduced=bool(same), repro_frame=same[0]['frame'] if same else None)
    if out['state'] and os.path.exists(trace):
        out.update(resume_input(a2, job, d, state, at, trace, log))
    if os.path.exists(trace):
        os.remove(trace)
    return out


def press_masks(script_txt, anchor):
    """{VBlank: PSX mask} of the random section of explore.txt, placed as
    pc/hal/trace.c places `+N BUTTONS[*H]` items (the cursor moves to each
    press's start; a press is held H VBlanks, default `hold`)"""
    masks, cur, hold, on = {}, anchor, 2, False
    with open(script_txt) as fh:
        lines = [l.split('#', 1)[0].strip() for l in fh]
    last_wait = max([i for i, l in enumerate(lines) if l.startswith('wait ')] or [-1])
    for i, l in enumerate(lines):
        if l.startswith('hold '):
            hold = int(l.split()[1])
        if i <= last_wait or not l.startswith('+'):
            continue
        delta, buttons = l.split()
        btn, _, h = buttons.partition('*')
        cur += int(delta[1:])
        m = pt.parse_buttons(btn)
        for v in range(cur, cur + (int(h) if h else hold)):
            masks[v] = masks.get(v, 0) | m
    return masks


def trace_cols(path):
    out = []
    with open(path, errors='replace') as fh:
        for line in fh:
            m = ROW.match(line)
            if m:
                out.append((int(m.group(1)), m.group(2), m.group(3), m.group(4), m.group(5),
                            le32(m.group(6))))
    return out


def resume_input(a2, job, d, state, at, trace, log):
    """resume.input: the run's presses after the save state as absolute
    VBlank lines counted from the load (a state restores frame_counter but
    not the input script's VBlank clock or its anchors), checked by loading
    the state and comparing every VBlank row with the uninterrupted run"""
    with open(log, errors='replace') as fh:
        text = fh.read()
    waits = [int(m.group(3)) for m in PREFIX.finditer(text)]
    masks = press_masks(os.path.join(d, 'explore.txt'), waits[-1] if waits else 0)
    rows = trace_cols(trace)
    saved = [r[0] for r in rows if r[5] >= at]
    if not saved:
        return {}
    v_save = saved[0] + 1
    ref = {r[0]: r[1:] for r in rows}
    path = os.path.join(d, 'resume.input')
    for shift in (0, -1, 1, -2, 2):
        base = v_save + shift
        with open(path, 'w') as fh:
            fh.write('# explorer resume: PC_LOADSTATE=before.tks PC_INPUT=resume.input '
                     '(VBlank 0 = the load)\nhold 1\nclock vblank\n')
            for v in sorted(masks):
                if v >= base and masks[v]:
                    fh.write('%d:%04x\n' % (v - base, masks[v]))
        work = os.path.join(d, 'work-resume')
        prepare_work(a2, job, work)
        rtrace = os.path.join(d, 'resume.trace')
        env = run_env(a2, work, path, rtrace, dict(PC_LOADSTATE=state))
        execute(a2, job, work, env, os.path.join(d, 'resume.log'), rtrace, a2.frames)
        got = trace_cols(rtrace) if os.path.exists(rtrace) else []
        shutil.rmtree(work, ignore_errors=True)
        if os.path.exists(rtrace):
            os.remove(rtrace)
        if not got:
            continue
        got = [r for r in got if r[5] >= at and base + r[0] in ref]
        same = bool(got) and all(ref[base + r[0]] == r[1:] for r in got)
        if same:
            return dict(resume_input=path, resume_exact=True, resume_rows=len(got))
    return dict(resume_input=path, resume_exact=False)


# ------------------------------------------------------------------ report

def summarise(a, results):
    sites = {}
    for r in results:
        for f in r['findings']:
            k = (f['kind'], f['site'])
            s = sites.setdefault(k, dict(kind=f['kind'], site=f['site'], runs=[], first=None, detail=f['detail'], cat=f.get('cat', '')))
            s['runs'].append(r['name'])
            if s['first'] is None or f['frame'] < s['first']['frame']:
                s['first'] = dict(run=r['name'], start=r['start'], seed=r['seed'], frame=f['frame'],
                                  vblank=f['vblank'], scene=f['scene'], repro=r.get('repro'))
    for s in sites.values():
        s['runs'] = sorted(set(s['runs']))
    out = sorted(sites.values(), key=lambda s: (s['kind'], s['site']))
    with open(os.path.join(a.out, 'findings.json'), 'w') as fh:
        json.dump(dict(bin=a.bin, frames=a.frames, runs=results, sites=out), fh, indent=1)
    total = sum(r['frames'] for r in results)
    lines = ['# Explorer findings', '',
             'binary `%s`, %d runs, %d game frames in all (budget %d per run)' % (
                 a.bin, len(results), total, a.frames), '',
             '| kind | site | runs | first (start, seed, game frame, scene) | detail |',
             '| --- | --- | --- | --- | --- |']
    for s in out:
        f = s['first']
        lines.append('| %s | `%s` | %d | %s s%d @%d %s | %s |' % (
            (s['kind'] + ' ' + s['cat']).strip(), s['site'], len(s['runs']), f['start'], f['seed'], f['frame'], f['scene'],
            s['detail'].replace('|', '/')))
    setup = [r['name'] for r in results if r['setup_failed']]
    if setup:
        lines += ['', 'setup failures (prefix milestone not reached): ' + ', '.join(setup)]
    with open(os.path.join(a.out, 'findings.md'), 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    if a.reach:
        names = set()
        for r in results:
            p = os.path.join(a.out, 'runs', r['name'], 'reach.list')
            if os.path.exists(p):
                with open(p) as fh:
                    names |= {l.strip() for l in fh if l.strip()}
        old = os.path.join(a.out, 'reach.list')
        if os.path.exists(old):
            with open(old) as fh:
                names |= {l.strip() for l in fh if l.strip()}
        with open(old, 'w') as fh:
            fh.write('\n'.join(sorted(names)) + '\n')
        print('[explore] reach: %d functions in %s' % (len(names), old))
    print('[explore] %d runs, %d game frames; %d distinct finding sites -> %s' % (
        len(results), total, len(out), os.path.join(a.out, 'findings.md')))
    for s in out:
        f = s['first']
        print('  %-11s %-50s runs %2d  first %s s%d frame %d scene %s' % (
            s['kind'], s['site'][:50], len(s['runs']), f['start'], f['seed'], f['frame'], f['scene']))
    return out


def replay(a):
    """--replay RUNDIR: the recorded run again (triage), trace kept"""
    d = os.path.abspath(a.replay)
    with open(os.path.join(d, 'result.json')) as fh:
        res = json.load(fh)
    a.bin = os.path.abspath(a.bin)
    job = dict(start=res['start'], seed=res['seed'], name=res['name'])
    if res.get('card'):
        job['card'] = res['card']
    elif job['start'].startswith('album:'):
        job['card'] = os.path.join(os.path.dirname(os.path.dirname(d)), 'cards',
                                   job['start'].split(':', 1)[1].replace('/', '-') + '.mcr')
    work = os.path.join(d, 'work-replay')
    prepare_work(a, job, work)
    trace = os.path.join(d, 'replay.trace')
    if os.path.exists(trace):
        # a stale trace from an earlier replay is read as this run's
        os.remove(trace)
    extra = {}
    if a.syms:
        extra['PC_TRACE_SYMS'] = a.syms
    if a.from_state:
        extra['PC_LOADSTATE'] = os.path.join(d, 'before.tks')
    if a.reach:
        extra['REACH_LIST_FILE'] = os.path.join(d, 'replay.reach')
        if os.path.exists(extra['REACH_LIST_FILE']):
            os.remove(extra['REACH_LIST_FILE'])
    a.stuck_vblanks = a.hang_vblanks = 1 << 30
    rc, w, reason = execute(a, job, work, run_env(a, work, os.path.join(d, 'explore.input'), trace, extra),
                            os.path.join(d, 'replay.log'), trace, a.frames)
    print('[explore] replay %s: rc %s, game frame %d, vblank %d; %s' % (
        job['name'], rc, w.fc, w.vblank, trace))
    shutil.rmtree(work, ignore_errors=True)
    return 0


def parse_seeds(text):
    out = []
    for part in text.split(','):
        if '-' in part:
            lo, hi = part.split('-')
            out += range(int(lo), int(hi) + 1)
        elif part:
            out.append(int(part))
    return out


def check_determinism(a, job):
    """run one job twice with full traces -> True if every VBlank row agrees"""
    rows = []
    for k in range(2):
        d = os.path.join(a.out, 'determinism', '%s-%d' % (job['name'], k))
        work = os.path.join(d, 'work')
        prepare_work(a, job, work)
        txt, inp = os.path.join(d, 'explore.txt'), os.path.join(d, 'explore.input')
        numeric_input(script_text(job['start'], job['seed'], a.frames), txt, inp)
        trace = os.path.join(d, 'trace.txt')
        execute(a, job, work, run_env(a, work, inp, trace), os.path.join(d, 'run.log'), trace, a.frames)
        with open(trace) as fh:
            rows.append([l for l in fh if l.startswith('frame=')])
    n = min(len(rows[0]), len(rows[1]))
    for i in range(n):
        if rows[0][i] != rows[1][i]:
            print('[explore] determinism: %s differs at row %d' % (job['name'], i))
            return False
    print('[explore] determinism: %s identical, %d / %d rows' % (job['name'], len(rows[0]), len(rows[1])))
    return len(rows[0]) == len(rows[1])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('--bin', default=pt.PC_BIN)
    ap.add_argument('--seeds', default='1-4', help='e.g. 1-16 or 3,7,9')
    ap.add_argument('--starts', default='title,newgame,album:95/7/22',
                    help='comma-separated (--list-starts); `all` for every start, `cards` '
                         'for every built checkpoint, `cards:GROUP` for one group')
    ap.add_argument('--frames', type=int, default=60000, help='game frames per run (from power-on)')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/explore'))
    ap.add_argument('--reach', action='store_true', help='set REACH_LIST_FILE (a reach binary)')
    ap.add_argument('--repro', action='store_true', help='re-run findings with a save state before them')
    ap.add_argument('--hang-vblanks', type=int, default=10800, help='frame_counter stopped this long = hang')
    ap.add_argument('--stuck-vblanks', type=int, default=36000, help='one scene this long with input = stuck')
    ap.add_argument('--stall-sec', type=int, default=90, help='no trace row this long (wall) = hang')
    ap.add_argument('--max-crashes', type=int, default=50, help='end a run after this many recoveries')
    ap.add_argument('--max-log', type=int, default=256 << 20)
    ap.add_argument('--timeout', type=int, default=7200, help='seconds per run')
    ap.add_argument('--resume', action='store_true',
                    help='keep the runs of --out that finished with this --frames')
    ap.add_argument('--list-starts', action='store_true')
    ap.add_argument('--check-determinism', action='store_true',
                    help='run the first job twice and compare the traces')
    ap.add_argument('--print-script', action='store_true', help='print the first job\'s script and exit')
    ap.add_argument('--replay', metavar='RUNDIR',
                    help='re-run one recorded run (its explore.input and card) to --frames, '
                         'from its before.tks with --from-state; keeps RUNDIR/replay.trace')
    ap.add_argument('--from-state', action='store_true')
    ap.add_argument('--syms', default='', help='with --replay: PC_TRACE_SYMS (name:size,...)')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                    help='an environment setting for every run (TOKI_LANG=en, PC_SWEEP_DIR=...)')
    a = ap.parse_args(argv)
    if a.replay:
        return replay(a)
    if a.list_starts:
        print('\n'.join(start_names()))
        return 0
    a.out, a.bin = os.path.abspath(a.out), os.path.abspath(a.bin)
    starts = expand_starts(a.starts)
    for s in starts:
        prefix_for(s)
    jobs = [dict(start=s, seed=n, name='%s-s%d' % (s.replace(':', '-').replace('/', '-'), n))
            for s in starts for n in parse_seeds(a.seeds)]
    if a.print_script:
        sys.stdout.write(script_text(jobs[0]['start'], jobs[0]['seed'], a.frames))
        return 0
    os.makedirs(a.out, exist_ok=True)
    for j in jobs:
        if j['start'].startswith('album:'):
            j['card'] = album_card(j['start'], a.out, a.bin, 900)
        elif j['start'].startswith('card:'):
            j['card'] = checkpoint_entry(j['start'].split(':', 1)[1])['card']
    if a.check_determinism:
        return 0 if check_determinism(a, jobs[0]) else 1
    with ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda j: run_job(a, j), jobs))
    summarise(a, results)
    return 0


if __name__ == '__main__':
    sys.exit(main())
