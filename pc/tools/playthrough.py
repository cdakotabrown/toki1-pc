#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Scripted playthrough: drive the real game with pad input, assert milestones.

The port boots from power-on (no SCENE / SCENE_NAV, no state pokes) and sees
only the scripted pad.  Each `wait` is a milestone read from real game globals
at VBlank (pc/hal/trace.c PC_INPUT anchored lines); the presses after it are
timed from the frame it was reached, the way a player waits for the screen.
A milestone that is not reached in time ends the run with exit 3 and this tool
fails loudly, naming it.  Nothing here changes game state, except the `poke`
lines of a seeded start (pc/tools/seeded_starts.py, below), which are tagged.

Script (tracked text, pc/tools/playthrough/*.txt):

    hold 2                      default frames a press is held
    phase NAME [keep-card]      start a fresh process (power-on); the memory
                                card is new unless keep-card (a reboot)
    wait LABEL COND... [within N] [mash BUTTONS[*H][,BUTTONS[*H]...]/K]
                                COND = NAME[:SIZE]=VALUE or NAME[:SIZE]!=VALUE
                                (NAME[:SIZE]&MASK=VALUE: MASK's bits only):
                                a traced field (pc/hal/trace.c FIELDS) or any
                                symbol of config/symbol_names.json; VALUE is an
                                integer for SIZE <= 4, else hex bytes.  N is in
                                VBlanks.  mash: while the conditions fail, press
                                BUTTONS every K frames from the wait's start
                                (VBlanks on the port, host frames on the
                                reference) -- tapping through dialogue whose
                                length differs between runs (XA voice length,
                                rand-dependent events).  A comma list
                                cycles (up to 16): one press every K frames,
                                or H + 5 after one held H frames (moving the
                                hand between taps).
    check LABEL COND...         a wait that must already hold (within 2)
    +N BUTTONS[*H]              press N frames after the previous anchor/press,
                                held H frames (VBlanks on the port, host frames
                                on the reference: --input-clock); BUTTONS is
                                cross+start style names or a hex PSX mask
    point X Y                   move the hand to (X, Y) from wherever it is:
                                park it in the clamp corner (-150,-100) with
                                left+up held 80 frames, then right and down
                                held (X+150)/3 and (Y+100)/3 frames -- the
                                presses `+30 left+up*80`, `+82 right*N`,
                                `+N+2 down*M` that the hand-written scripts
                                use.  The hand moves 3 px per held frame on
                                the pointer screens (step_directional_
                                scroll_once with the d-pad doubling of
                                menu_mode 0), so X+150 and Y+100 must be
                                multiples of 3; 0 skips that leg
    branch LABEL [within N] [mash BUTTONS[*H][,...]/K]
    case LABEL COND...          a wait with outcomes: the first case whose
      ...items...               conditions hold (cases tried in order, every
    case LABEL COND...          VBlank) is the milestone reached, and its items
      ...items...               run; then the script goes on after end-branch.
    end-branch                  Branches nest.  Case labels are rand-dependent
                                milestones: the port and the reference may take
                                different cases (reported, never hidden).
    wait/check ... rand         marks a milestone whose value (day, field) the
                                game's rand decides: reported, not compared
    shot LABEL [N]              screenshot N frames later (--shots)
    poke N 0xADDR HEX           SEEDED RUNS ONLY (pc/tools/seeded_starts.py):
                                N frames after the previous anchor/press,
                                write HEX (1..256 bytes) at the PS1 RAM
                                address on both machines -- the port's
                                PC_INPUT `poke` (pc/hal/trace.c), the
                                reference's -poke FRAME:ADDR:HEX, each
                                applied before that VBlank's trace row.  It
                                must follow a wait.  A script with a poke is
                                a seeded run: report.json says
                                "mode": "seeded" and lists every poke; it is
                                never a normal-run (no state writes) result.
    end N                       end the phase N frames later
    trace NAME[:SIZE]...        also trace these globals (diagnostics)
    card PHASE NAME...          after PHASE: the card holds files NAME...
    same FIELD[:SIZE] LABEL_A LABEL_B
                                the field's bytes at milestone A equal its
                                bytes at milestone B (save/load round trip)

Usage:
    python3 pc/tools/playthrough.py pc/tools/playthrough/new_game.txt \\
        --out build/playthrough [--bin build/pt-pc/tokimeki-pc] [--shots [--vram]]
        [--retail] [--start-card CARD.mcr]

--start-card starts every phase that is not keep-card from a copy of the
given card on both machines (a checkpoint save: an album crafted with
mcr_tool.py album-set, loaded through the title's Continue).
--retail runs the same script on the reference emulator (REFEMU_REGTEST,
the retail disc, a fresh formatted -memcard per run): each milestone is
located in the reference trace, its presses are placed at the matching
host frames, and the milestone frames / traced globals are compared.
With --shots the reference is re-run to each `shot`'s host frame with
-dumpvram (shots-retail/<phase>/<label>.vram and .ppm, the displayed
320x240 rectangle) and report.json `shots` gives, per label, the fraction
of pixels that differ from the port's shot at 5-bit precision and the
mean absolute 5-bit channel difference.  The two shots are at the same
offset from the same milestone, so they can be a few frames apart in
game time (the milestone deltas).
"""
import argparse
import json
import os
import re
import shlex
import shutil
import signal
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run, terminate  # noqa: E402
import trace_compare as tc  # noqa: E402
import game_platform  # noqa: E402
import refemu  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input  # noqa: E402
# the Linux build, or the Windows exe with PC_TEST_PLATFORM=windows (run
# under Wine in a private prefix by safe_process; game_platform.py)
PC_BIN = str(game_platform.GAME)
PC_LOG_LIMIT = 512 * 1024 * 1024   # the port logs every LoadImage
# A phase can run ~40,000 frames; trace_compare's default row/memory bounds
# are sized for short segments, so a phase's trace gets explicit ones.
TRACE_LIMITS = dict(max_rows=120000, max_decoded=768 * 1024 * 1024)
BUTTONS = tc.BUTTONS
FIELD_SIZES = {n: s for n, _, s in tc.FIELDS}
FIELD_ADDRS = {n: a for n, a, _ in tc.FIELDS}


class ScriptError(Exception):
    pass


def parse_buttons(text):
    mask = 0
    for part in text.lower().split('+'):
        part = part.strip()
        if part in BUTTONS:
            mask |= BUTTONS[part]
        else:
            try:
                mask |= int(part, 16)
            except ValueError:
                raise ScriptError(f'unknown button {part!r}')
    return mask & 0xFFFF


PARK = (-150, -100)    # the hand's clamp corner (step_directional_scroll_once)
HAND_PX = 3            # px per held frame on the pointer screens


def point_presses(x, y):
    """`point X Y` -> the press items: park, then right N, then down M"""
    dx, dy = x - PARK[0], y - PARK[1]
    if not (0 <= dx <= 290 and 0 <= dy <= 200) or dx % HAND_PX or dy % HAND_PX:
        raise ScriptError(f'point {x} {y}: X+150 in 0..290 and Y+100 in 0..200, '
                          f'multiples of {HAND_PX}')
    out = [dict(kind='press', delta=30, mask=BUTTONS['left'] | BUTTONS['up'], hold=80)]
    gap = 82                    # the park's 80 held frames, then 2
    for btn, n in (('right', dx // HAND_PX), ('down', dy // HAND_PX)):
        if n:
            out.append(dict(kind='press', delta=gap, mask=BUTTONS[btn], hold=n))
            gap = n + 2
    return out


def parse_cond(tok):
    m = re.fullmatch(r'(\w+)(?::(\w+))?(?:&(\w+))?(!?=)(\S+)', tok)
    if not m:
        raise ScriptError(f'bad condition {tok!r}')
    name, size, mask, op, value = m.groups()
    size = int(size, 0) if size else FIELD_SIZES.get(name, 4)
    if mask is not None and size > 4:
        raise ScriptError(f'{tok}: a mask needs SIZE <= 4')
    if mask is not None:
        mk = int(mask, 0) & ((1 << (8 * size)) - 1)
        val = (int(value, 0) & mk).to_bytes(size, 'little')
        return dict(name=name, size=size, neg=op == '!=', value=val, mask=mk)
    if size <= 4:
        val = int(value, 0).to_bytes(size, 'little', signed=int(value, 0) < 0)
    else:
        val = bytes.fromhex(value)
        if len(val) != size:
            raise ScriptError(f'{tok}: {len(val)} bytes given for size {size}')
    return dict(name=name, size=size, neg=op == '!=', value=val)


def parse_wait_opts(w, hold, kind):
    """the wait/branch tokens after the label -> (conds tokens, within, mash, rand)"""
    within = 2 if kind == 'check' else 0
    rest = list(w)
    if 'within' in rest:
        i = rest.index('within')
        within = int(rest[i + 1])
        rest = rest[:i] + rest[i + 2:]
    mash = None
    if 'mash' in rest:
        if kind == 'check':
            raise ScriptError('check cannot mash')
        i = rest.index('mash')
        btns, _, every = rest[i + 1].partition('/')
        cycle = []
        for b in btns.split(','):
            btn, _, h = b.partition('*')
            cycle.append((parse_buttons(btn), int(h) if h else 0))
        mash = (cycle, int(every))
        if not cycle or len(cycle) > 16 or any(not m for m, _ in cycle) or mash[1] <= hold:
            raise ScriptError(f'mash {rest[i + 1]}: needs 1..16 presses and K > hold')
        rest = rest[:i] + rest[i + 2:]
    rand = 'rand' in rest
    rest = [t for t in rest if t != 'rand']
    return rest, within, mash, rand


def walk(items):
    """every item, branches' cases included (depth first)"""
    for it in items:
        yield it
        if it['kind'] == 'branch':
            for c in it['cases']:
                yield c
                yield from walk(c['items'])


def parse(path):
    """-> dict(hold, phases=[dict(name, keep, items)], cards, sames)"""
    hold, phases, cards, sames, traces = 2, [], [], [], []
    stack = []      # open branches
    with open(path) as fh:
        text = fh.read()
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        w = line.split()
        try:
            if w[0] == 'hold':
                hold = max(1, int(w[1]))
            elif w[0] == 'phase':
                if stack:
                    raise ScriptError('phase inside a branch')
                phases.append(dict(name=w[1], keep='keep-card' in w[2:], items=[]))
            elif w[0] == 'trace':
                for t in w[1:]:
                    name, _, size = t.partition(':')
                    traces.append((name, int(size, 0) if size else 4))
            elif w[0] == 'card':
                cards.append((w[1], w[2:]))
            elif w[0] == 'same':
                name, _, size = w[1].partition(':')
                sames.append((name, int(size, 0) if size else FIELD_SIZES.get(name, 4), w[2], w[3]))
            else:
                if not phases:
                    raise ScriptError('item before the first phase')
                if stack and not stack[-1]['cases'] and w[0] not in ('case', 'end-branch'):
                    raise ScriptError('a branch starts with a case')
                items = (stack[-1]['cases'][-1]['items'] if stack[-1]['cases'] else None) \
                    if stack else phases[-1]['items']
                if w[0] in ('wait', 'check'):
                    rest, within, mash, rand = parse_wait_opts(w[2:], hold, w[0])
                    conds = [parse_cond(t) for t in rest]
                    if not conds:
                        raise ScriptError('wait without conditions')
                    items.append(dict(kind='wait', label=w[1], conds=conds, within=within,
                                      mash=mash, rand=rand))
                elif w[0] == 'branch':
                    rest, within, mash, _ = parse_wait_opts(w[2:], hold, 'branch')
                    if rest:
                        raise ScriptError('branch takes its conditions on its cases')
                    b = dict(kind='branch', label=w[1], within=within, mash=mash, cases=[])
                    items.append(b)
                    stack.append(b)
                elif w[0] == 'case':
                    if not stack:
                        raise ScriptError('case outside a branch')
                    conds = [parse_cond(t) for t in w[2:] if t != 'rand']
                    if not conds:
                        raise ScriptError('case without conditions')
                    stack[-1]['cases'].append(dict(kind='case', label=w[1], conds=conds,
                                                   items=[], rand=True))
                elif w[0] == 'end-branch':
                    if not stack or len(stack[-1]['cases']) < 2:
                        raise ScriptError('end-branch needs an open branch with two cases')
                    stack.pop()
                elif w[0].startswith('+'):
                    btn, _, h = w[1].partition('*')
                    items.append(dict(kind='press', delta=int(w[0][1:]), mask=parse_buttons(btn),
                                      hold=int(h) if h else 0))
                elif w[0] == 'point':
                    items.extend(point_presses(int(w[1]), int(w[2])))
                elif w[0] == 'poke':
                    addr = int(w[2], 16) if w[2].lower().startswith('0x') else None
                    data = bytes.fromhex(w[3])
                    if addr is None or not 0x80000000 <= addr <= 0x80200000 - len(data):
                        raise ScriptError(f'poke {w[2]}: a PS1 RAM address 0x80000000..0x801FFFFF')
                    if not 1 <= len(data) <= 256:
                        raise ScriptError('poke: 1..256 bytes per line')
                    items.append(dict(kind='poke', delta=int(w[1]), addr=addr, data=data))
                elif w[0] == 'shot':
                    items.append(dict(kind='shot', label=w[1], delta=int(w[2]) if len(w) > 2 else 0))
                elif w[0] == 'end':
                    items.append(dict(kind='end', delta=int(w[1])))
                else:
                    raise ScriptError(f'unknown directive {w[0]!r}')
        except (IndexError, ValueError) as e:
            raise ScriptError(f'{path}:{n}: {e}: {raw.strip()}')
        except ScriptError as e:
            raise ScriptError(f'{path}:{n}: {e}')
    if stack:
        raise ScriptError('branch without end-branch')
    for p in phases:
        for it in p['items']:
            if it['kind'] in ('wait', 'branch'):
                break
            if it['kind'] == 'poke':
                raise ScriptError(f'phase {p["name"]}: a poke must follow a wait')
    labels = [it['label'] for p in phases for it in walk(p['items'])
              if it['kind'] in ('wait', 'branch', 'case')]
    if len(labels) != len(set(labels)):
        raise ScriptError('milestone labels must be unique')
    return dict(hold=hold, phases=phases, cards=cards, sames=sames, traces=traces)


def cond_text(c):
    v = c['value']
    value = hex(int.from_bytes(v, 'little')) if c['size'] <= 4 else v.hex()
    mask = f"&{hex(c['mask'])}" if c.get('mask') is not None else ''
    return f"{c['name']}:{c['size']}{mask}{'!=' if c['neg'] else '='}{value}"


def flatten(items, out=None):
    """the item tree as PC_INPUT's linear program: a branch is a `branch`
    item followed by one `case` item per case (each naming the index its
    items start at); every case's items end with a `goto` past the branch."""
    if out is None:
        out = []
    for it in items:
        if it['kind'] != 'branch':
            out.append(it)
            continue
        out.append(dict(it, ncases=len(it['cases'])))
        slots = []
        for c in it['cases']:
            slot = dict(kind='case', label=c['label'], conds=c['conds'], target=None)
            out.append(slot)
            slots.append(slot)
        exits = []
        for c, slot in zip(it['cases'], slots):
            slot['target'] = len(out)
            flatten(c['items'], out)
            g = dict(kind='goto', target=None)
            out.append(g)
            exits.append(g)
        for g in exits:
            g['target'] = len(out)
    return out


INPUT_CLOCK = 'vblank'   # --input-clock: how the port counts +N (pc/hal/trace.c)
AUDIO_MIX = 'game'       # --audio-mix: where the port mixes the SPU (see run_pc)
# --start-card: a phase without keep-card starts from a copy of this 128 KiB
# card image on both machines instead of a blank formatted card (a
# checkpoint save, pc/tools/mcr_tool.py album-set; docs/pc-port/COVERAGE-PLAN.md)
START_CARD = None
PROFILE = None   # --profile: the port's run profile (pc/hal/profile.h); None = parity
RUNNER = []      # --runner: a launcher before the binary ('wine' for the Windows build)
SETTINGS = {}    # --set NAME=VALUE: profile switches / settings for the port
INSTALLED = False  # --installed: the binary finds its own game data (a player install)


def mash_text(mash):
    """PC_INPUT's `MASK[*H],.../K` (a single press: the old `MASK/K`)"""
    cycle, every = mash
    return ','.join(f'{m:04x}' + (f'*{h}' if h else '') for m, h in cycle) + f'/{every}'


def mash_presses(mash, start, end, hold):
    """the reference's inputs for a mash after START up to END: press i of
    the cycle K frames after the previous one, or H + 5 after a press held
    H frames when that is longer (pc/hal/trace.c), held H frames
    (consecutive -input frames, as a held `+N` press is placed)"""
    cycle, every = mash
    out = []
    f, i = start + every, 0
    while f < end:
        m, h = cycle[i % len(cycle)]
        out += [(f + j, m) for j in range(0, max(1, (h or hold) - hold + 1))]
        f += max(every, h + 5)          # h 0 (the script's hold): every K
        i += 1
    return out


def numeric_phase(phase, hold, shots):
    # presses are placed by VBlank after their milestone on both sides: the
    # reference's -input counts host frames, so the port's presses count
    # VBlanks too (`clock vblank`); `--input-clock game` restores game frames
    out = [f'hold {hold}', f'clock {INPUT_CLOCK}']
    for it in flatten(phase['items']):
        k = it['kind']
        if k == 'branch':
            out.append(f"branch {it['label']} {it['ncases']}"
                       + (f" within {it['within']}" if it['within'] else '')
                       + (f" mash {mash_text(it['mash'])}" if it.get('mash') else ''))
        elif k == 'case':
            out.append(f"case {it['label']} {it['target']} " + ' '.join(cond_text(c) for c in it['conds']))
        elif k == 'goto':
            out.append(f"goto {it['target']}")
        elif k == 'wait':
            out.append(f"wait {it['label']} " + ' '.join(cond_text(c) for c in it['conds'])
                       + (f" within {it['within']}" if it['within'] else '')
                       + (f" mash {mash_text(it['mash'])}" if it.get('mash') else ''))
        elif k == 'press':
            out.append(f"+{it['delta']}:{it['mask']:04x}" + (f"*{it['hold']}" if it['hold'] else ''))
        elif k == 'shot':
            out.append(f"shot {it['label']} {it['delta']}" if shots else f"+{it['delta']}:0000*1")
        elif k == 'end':
            out.append(f"end {it['delta']}")
        elif k == 'poke':
            out.append(f"poke {it['delta']} 0x{it['addr']:08X} {it['data'].hex()}")
    return '\n'.join(out) + '\n'


def seeded_pokes(script):
    """every poke item of the script (a seeded run when non-empty)"""
    return [dict(phase=p['name'], delta=it['delta'], addr=f"0x{it['addr']:08X}",
                 bytes=len(it['data']))
            for p in script['phases'] for it in walk(p['items']) if it['kind'] == 'poke']


def poke_args(pokes, upto=None):
    """the reference's -poke FRAME:ADDR:HEX arguments (frames <= upto)"""
    out = []
    for f, addr, data in pokes:
        if upto is None or f <= upto:
            out += ['-poke', f'{f}:{addr:08X}:{data.hex()}']
    return out


def trace_syms(script):
    """extra traced globals: every condition/compare name that is not a fixed field"""
    names = {}
    for p in script['phases']:
        for it in walk(p['items']):
            for c in it.get('conds', ()):
                if c['name'] not in FIELD_SIZES:
                    names[c['name']] = max(names.get(c['name'], 0), c['size'])
    for name, size in script['traces']:
        names[name] = max(names.get(name, 0), size)
    for name, size, _, _ in script['sames']:
        if name not in FIELD_SIZES:
            names[name] = max(names.get(name, 0), size)
    return names


REACHED = re.compile(r'\[TRACE\] PC_INPUT wait (\S+) reached: game frame (\d+) vblank (\d+)')
BRANCHED = re.compile(r'\[TRACE\] PC_INPUT branch (\S+) -> (\S+)')
BRANCHES = {}   # side -> {branch label: case label}


def rand_labels(script):
    """milestones the game's rand decides: every case, and waits marked rand"""
    return {it['label'] for p in script['phases'] for it in walk(p['items'])
            if it['kind'] == 'case' or (it['kind'] == 'wait' and it.get('rand'))}
MISSED = re.compile(r'\[TRACE\] PC_INPUT wait (\S+) NOT reached')


def read_card(path):
    """{file name: 8 KiB block} of a .mcr image (single-block files)"""
    with open(path, 'rb') as fh:
        data = fh.read()
    files = {}
    for i in range(1, 16):
        frame = data[i * 128:(i + 1) * 128]
        if frame[0] == 0x51:
            name = frame[0x0A:0x1E].split(b'\0', 1)[0].decode('ascii', 'replace')
            files[name] = data[i * 8192:(i + 1) * 8192]
    return files


def row_at(rows, vblank):
    for r in rows:
        if r['frame'] >= vblank:
            return r
    return rows[-1] if rows else None


FOLDS = {}     # phase -> fault-fold recoveries ([FOLD] total=)
SHOT_JOBS = 4        # --shot-jobs: reference shot re-runs at once
MAX_FRAMES = 40000   # --max-frames: the port's PC_TRACE_FRAMES (game frames per phase)


def run_pc(script, out, binary, shots, timeout):
    """-> (milestones {label: dict(phase, game, vblank, fields)}, failures)"""
    syms = trace_syms(script)
    milestones, failures = {}, []
    work = os.path.join(out, 'pc-work')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, 'pc'))
    if not INSTALLED:
        os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
        os.symlink(str(disc_input('disks')), os.path.join(work, 'disks'))
    for phase in script['phases']:
        name = phase['name']
        if not phase['keep']:
            shutil.rmtree(os.path.join(work, 'saves'), ignore_errors=True)
            if START_CARD:
                os.makedirs(os.path.join(work, 'saves'))
                shutil.copy(START_CARD, os.path.join(work, 'saves/card1.mcr'))
        num = os.path.join(out, f'{name}.input')
        with open(num, 'w') as fh:
            fh.write(numeric_phase(phase, script['hold'], shots))
        trace = os.path.join(out, f'{name}.pc.trace')
        log = os.path.join(out, f'{name}.pc.log')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_TRACE=trace, PC_TRACE_PHASE='pre_irq', PC_INPUT=num,
                   PC_TRACE_FRAMES=str(MAX_FRAMES), TOKI_INPUT_DEVICE='pad')
        if PROFILE:
            # set after the TOKI_ strip above: the only profile input the port sees
            env['TOKI_PROFILE'] = PROFILE
        env.update(SETTINGS)
        if syms:
            env['PC_TRACE_SYMS'] = ','.join(f'{n}:{s}' for n, s in syms.items())
        if AUDIO_MIX == 'game':
            # The game-thread mix (pc/hal/parity.c with no anchor: nothing is
            # captured): SPU mixing and libsnd's voice state follow the
            # emulated clock as on the reference.  The SDL thread's mix runs
            # on the host clock, and the sequencer's interrupt time with it
            # differs run to run (timing round 6; pc/tools/build_compare.py).
            env.update(PC_PARITY_DIR=os.path.join(work, 'parity'), PC_PARITY_FRAMES='1')
        if shots:
            env['PC_SHOT_DIR'] = os.path.join(os.path.abspath(out), 'shots', name)
            if shots == 'vram':      # each shot also writes LABEL.vram / LABEL.ram
                env.update(PC_SHOT_VRAM='1', PC_PARITY_RAM='1')
            os.makedirs(env['PC_SHOT_DIR'], exist_ok=True)
        print(f'[pc] phase {name}: running', flush=True)
        if RUNNER and os.path.basename(RUNNER[0]).startswith('wine'):
            env.setdefault('WINEDEBUG', '-all')   # Wine's own diagnostics stay out of the log
        # --installed: no asset argument and no pc/assets in the working
        # folder -- the binary resolves its data root itself (portable
        # install: <exe dir>/assets, checked against its manifest)
        args = [binary] if INSTALLED else [binary, os.path.join(ROOT, 'pc/assets')]
        r = run(RUNNER + args, cwd=work, env=env,
                timeout=timeout, log_path=log, max_output=PC_LOG_LIMIT)
        with open(log, errors='replace') as fh:
            text = fh.read()
        rows = tc.parse_trace(trace, **TRACE_LIMITS) if os.path.exists(trace) else []
        if not re.search(r'\[TRACE\] PC_INPUT \S+: \d+ presses, \d+ anchored items', text):
            failures.append(f'phase {name}: the port did not load the input script (log {log})')
            break
        for m in REACHED.finditer(text):
            label, g, vb = m.group(1), int(m.group(2)), int(m.group(3))
            row = row_at(rows, vb) or {}
            milestones[label] = dict(phase=name, game=g, vblank=vb, row=row)
            print(f'[pc]   {label:20s} game frame {g:6d}  vblank {vb:6d}', flush=True)
        for m in BRANCHED.finditer(text):
            BRANCHES.setdefault('pc', {})[m.group(1)] = m.group(2)
            print(f'[pc]   branch {m.group(1)} -> {m.group(2)}', flush=True)
        for m in MISSED.finditer(text):
            failures.append(f'phase {name}: milestone {m.group(1)} NOT reached')
        expected = [it['label'] for it in phase['items'] if it['kind'] in ('wait', 'branch')]
        missing = [l for l in expected if l not in milestones]
        if r.reason or r.returncode != 0:
            failures.append(f'phase {name}: process ended {r.reason or r.returncode} (log {log})')
        if missing and not any(l in f for f in failures for l in missing):
            failures.append(f'phase {name}: milestones not reached: {" ".join(missing)}')
        # pc/src/main.c's sign-extension fault fold: not a crash, counted
        folds = [int(x) for x in re.findall(r'\[FOLD\] total=(\d+)', text)]
        sites = len(re.findall(r'^\[FOLD\] site ', text, re.M))
        FOLDS[name] = max(folds + [sites])
        print(f'[pc] phase {name}: fault folds {FOLDS[name]} ({sites} site(s))', flush=True)
        if FOLDS[name]:
            # a sign-extended PS1 address reached a dereference
            # (pc/tools/adapt_ps1_addresses.py missed a path; log names it)
            failures.append(f'phase {name}: {FOLDS[name]} fault-fold recoveries (log {log})')
        crashes = max((row.get('crashes', 0) for row in rows), default=0)
        if crashes:
            failures.append(f'phase {name}: {crashes} recovered crash(es) in the trace')
        card = os.path.join(work, 'saves/card1.mcr')
        if os.path.exists(card):
            shutil.copy(card, os.path.join(out, f'{name}.card1.mcr'))
        if failures:
            break
    return milestones, failures


def check_post(script, out, milestones, failures, prefix='pc'):
    for phase, names in script['cards']:
        path = os.path.join(out, f'{phase}.card1.mcr' if prefix == 'pc' else f'{phase}.retail.mcd')
        if not os.path.exists(path):
            failures.append(f'card after {phase}: no card image')
            continue
        files = read_card(path)
        lacking = [n for n in names if n not in files]
        print(f'[{prefix}] card after {phase}: {sorted(files)}')
        if lacking:
            failures.append(f'card after {phase}: missing {lacking}')
    for name, size, a, b in script['sames']:
        if a not in milestones or b not in milestones:
            continue
        va, vb = milestones[a]['row'].get(name), milestones[b]['row'].get(name)
        ok = va is not None and va == vb
        print(f'[{prefix}] same {name} at {a} / {b}: {"OK" if ok else "DIFFERS"}')
        if not ok:
            failures.append(f'{name} differs between {a} and {b}: {va!r} / {vb!r}')


# ---------------------------------------------------------------- retail
def holds(row, conds):
    for c in conds:
        v = row.get(c['name'])
        if v is None:
            return False
        if isinstance(v, int) and c.get('mask') is not None:
            v &= c['mask']
        eq = v.to_bytes(c['size'], 'little') == c['value'] if isinstance(v, int) else False
        if eq == c['neg']:
            return False
    return True


# The game displays VRAM (0,0)-(320,240) whatever activeBuff says: on the
# port's shots the scanout equals that rectangle (96.5-100% of pixels; the
# y=256 half matches 5%), so the reference shot is cut there too.
DISPLAY_Y = 0


def vram_ppm(vram, y0, path):
    """the 320x240 BGR555 rectangle at (0, y0) of a 1024x512 u16le VRAM
    dump as a binary PPM (5-bit channels << 3, the port's shift3)."""
    rows = []
    for y in range(240):
        line = bytearray()
        o = ((y0 + y) * 1024) * 2
        for x in range(320):
            p = vram[o + 2 * x] | (vram[o + 2 * x + 1] << 8)
            line += bytes(((p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3))
        rows.append(bytes(line))
    with open(path, 'wb') as fh:
        fh.write(b'P6\n320 240\n255\n' + b''.join(rows))


def retail_shots(out, phase, start_card, inputs, shot_frames, hold, timeout, ram=False,
                 pokes=()):
    """Re-run the phase's final inputs to each shot's host frame with
    -dumpvram: shots-retail/<phase>/<label>.vram and .ppm (the displayed
    rectangle, DISPLAY_Y).  ram (--vram): also <label>.ram (-dumpram) and
    <label>.sav (-savestate, the GTE registers for lit_retail_check.py)."""
    from concurrent.futures import ThreadPoolExecutor
    d = os.path.join(out, 'shots-retail', phase)
    os.makedirs(d, exist_ok=True)

    def one(item):
        # npokes: the seeded pokes before the shot in script order (a shot
        # just ahead of a poke on the same frame sees the RAM before it, as
        # on the port, where seq_step runs the items in order)
        label, host, npokes = item if len(item) == 3 else (item[0], item[1], len(pokes))
        with tempfile.TemporaryDirectory() as tmp:
            card = os.path.join(tmp, 'card.mcd')
            shutil.copy(start_card, card)
            vram = os.path.join(d, label + '.vram')
            extra = ['-memcard', card, '-dumpvram', vram] + poke_args(pokes[:npokes], host)
            if ram:
                extra += ['-dumpram', os.path.join(d, label + '.ram'),
                          '-savestate', os.path.join(d, label + '.sav')]
            tc.regtest(host + 1, os.path.join(tmp, 't.trace'),
                       [i for i in inputs if i[0] <= host], hold,
                       extra=extra, timeout=timeout, limits=TRACE_LIMITS)
        with open(vram, 'rb') as fh:
            vram_ppm(fh.read(), DISPLAY_Y, os.path.join(d, label + '.ppm'))
        print(f'[ps1]   shot {label} at host frame {host}', flush=True)
    # SHOT_JOBS (--shot-jobs, default PLAYTHROUGH_SHOT_JOBS or 4): how many
    # regtest shot re-runs at once; 1 keeps a suite at --jobs N within N
    # emulator processes
    with ThreadPoolExecutor(SHOT_JOBS) as ex:
        list(ex.map(one, shot_frames))


def read_ppm(path):
    with open(path, 'rb') as fh:
        data = fh.read()
    parts = data.split(b'\n', 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]


def shot_diff(pc_path, ps1_path):
    """pixels that differ at 5-bit precision (the PS1's colour depth) and
    the mean absolute 5-bit channel difference; None on a size mismatch."""
    w1, h1, a = read_ppm(pc_path)
    w2, h2, b = read_ppm(ps1_path)
    if (w1, h1) != (w2, h2):
        return None
    n = w1 * h1
    diff = total = 0
    for i in range(n):
        o = 3 * i
        d = [abs((a[o + k] >> 3) - (b[o + k] >> 3)) for k in range(3)]
        if any(d):
            diff += 1
            total += sum(d)
    return dict(differing=round(diff / n, 6), mad=round(total / (3 * n), 4))


def run_retail(script, out, timeout, shots=False):
    """Place each milestone's presses on the reference.  -> (milestones, failures)

    Every reference run starts from power-on with the card as it was at the
    start of the phase (a run writes the card; a re-run must not see its own
    writes).  A milestone is the first traced frame, at or after the previous
    press is released, where its conditions hold; the presses after it are
    placed by its host frame (game and host frames advance together while the
    game waits for input)."""
    syms = trace_syms(script)
    tc.set_syms(','.join(f'{n}:{s}' for n, s in syms.items()))
    milestones, failures = {}, []
    card = os.path.join(out, 'retail.mcd')
    for phase in script['phases']:
        name = phase['name']
        if (not phase['keep'] or not os.path.exists(card)) and START_CARD:
            shutil.copy(START_CARD, card)
        elif not phase['keep'] or not os.path.exists(card):
            with open(card, 'wb') as fh:
                fh.write(bytes(128 * 1024))
            run([sys.executable, os.path.join(ROOT, 'pc/tools/mcr_tool.py'), 'format', card]).require_ok()
        start_card = os.path.join(out, f'{name}.retail-start.mcd')
        shutil.copy(card, start_card)
        trace = os.path.join(out, f'{name}.ps1.trace')
        cache = {}
        pokes = []          # (host frame, addr, bytes): a seeded run's state

        def reference(budget, inputs):
            key = (budget, tuple(inputs), tuple(pokes))
            if key not in cache:
                cache.clear()
                shutil.copy(start_card, card)
                print(f'[ps1]   regtest {budget} host frames, {len(inputs)} inputs'
                      + (f', {len(pokes)} seeded pokes' if pokes else ''), flush=True)
                cache[key] = tc.regtest(budget, trace, list(inputs), script['hold'],
                                        extra=['-memcard', card] + poke_args(pokes),
                                        timeout=timeout, limits=TRACE_LIMITS)
            return cache[key]

        inputs, ready, budget = [], 0, 4000
        shot_frames = []
        prog = flatten(phase['items'])
        i = 0
        anchored = False
        end_host = None
        while i < len(prog):
            it = prog[i]
            k = it['kind']
            if k == 'goto':
                i = it['target']
                continue
            if k == 'poke' and not anchored:
                failures.append(f'retail phase {name}: a poke before the first milestone')
                return milestones, failures
            if k == 'poke':
                # the same frame as the port's: N VBlanks after the cursor,
                # before that frame's trace row (regtest ApplyPokes)
                cursor_h += it['delta']
                pokes.append((cursor_h, it['addr'], it['data']))
                print(f'[ps1]   seeded poke 0x{it["addr"]:08X} {len(it["data"])} bytes '
                      f'at host frame {cursor_h}', flush=True)
                i += 1
                continue
            if k == 'case' or (k in ('press', 'shot', 'end') and not anchored):
                i += 1
                continue
            if k == 'press':
                cursor_h += it['delta']
                h = it['hold'] or script['hold']
                for j in range(0, max(1, h - script['hold'] + 1)):
                    inputs.append((cursor_h + j, it['mask']))
                ready = cursor_h + h
                i += 1
                continue
            if k == 'shot':
                cursor_h += it['delta']
                shot_frames.append((it['label'], cursor_h, len(pokes)))
                i += 1
                continue
            if k == 'end':
                end_host = cursor_h + it['delta']
                i += 1
                continue
            # a wait, or a branch: the first of its cases to hold
            if anchored:
                budget = max(budget, ready + 1200)
            if k == 'wait':
                alts = [(it['label'], it['conds'], i + 1)]
            else:
                alts = [(c['label'], c['conds'], c['target'])
                        for c in prog[i + 1:i + 1 + it['ncases']]]
            hit = None
            mash = []
            while hit is None:
                if it.get('mash'):
                    # a press every K host frames from the wait's start, up to
                    # the budget; the ones at or after the hit are dropped below
                    # (they cannot change any earlier frame)
                    mash = mash_presses(it['mash'], ready, budget, script['hold'])
                rows = reference(budget, sorted(inputs + mash))
                for r in rows:
                    if r['frame'] < ready:
                        continue
                    alt = next((a for a in alts if holds(r, a[1])), None)
                    if alt is not None:
                        hit = r
                        break
                if hit is None:
                    lim = it['within'] or 20000
                    if budget - ready > lim + 600:
                        failures.append(f'retail phase {name}: milestone {it["label"]} NOT reached '
                                        f'(host frames {ready}..{budget})')
                        return milestones, failures
                    budget += max(600, min(lim, 3000))
            inputs.extend(m for m in mash if m[0] < hit['frame'])
            g = hit['frame_counter']
            labels = [alt[0]] if k == 'wait' else [it['label'], alt[0]]
            for lab in labels:
                milestones[lab] = dict(phase=name, game=g, vblank=hit['frame'], row=hit)
                print(f'[ps1]  {lab:20s} game frame {g:6d}  host frame {hit["frame"]:6d}', flush=True)
            if k == 'branch':
                BRANCHES.setdefault('retail', {})[it['label']] = alt[0]
                print(f'[ps1]   branch {it["label"]} -> {alt[0]}', flush=True)
            cursor_h = hit['frame']
            ready = cursor_h
            anchored = True
            i = alt[2]
        if anchored:
            budget = max(budget, ready + 1200)
        reference(max(budget, end_host or 0), inputs)
        shutil.copy(card, os.path.join(out, f'{name}.retail.mcd'))
        if shots and shot_frames:
            retail_shots(out, name, start_card, inputs, shot_frames, script['hold'], timeout,
                         ram=shots == 'vram', pokes=pokes)
    return milestones, failures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('script')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/playthrough'))
    ap.add_argument('--bin', default=PC_BIN)
    ap.add_argument('--runner', default='',
                    help='a program the port binary is started through, e.g. '
                         '`--runner wine` with the caller\'s own WINEPREFIX. Without '
                         'it a .exe --bin (or PC_TEST_PLATFORM=windows) runs under Wine '
                         'in a private prefix per phase (docs/pc-port/WINDOWS-BUILD.md)')
    ap.add_argument('--shots', action='store_true', help='write PC_INPUT `shot` screenshots')
    ap.add_argument('--vram', action='store_true',
                    help='with --shots: also the VRAM (u16le, the reference -dumpvram '
                         'layout), the 2 MB PS1 RAM and the GTE registers at each shot '
                         '(port: .ram/.gte; reference: .ram/.sav)')
    ap.add_argument('--timeout', type=int, default=900, help='seconds per phase')
    ap.add_argument('--retail', action='store_true', help='also run the script on the reference emulator (pc/tools/refemu.py)')
    ap.add_argument('--input-clock', choices=('vblank', 'game'), default='vblank',
                    help='what the port\'s +N counts after a milestone: VBlanks, as the '
                         'reference counts host frames (default), or game frames (the '
                         'behaviour before 2026-10-31; a frame that overruns then shifts '
                         'the port\'s presses against the reference\'s)')
    ap.add_argument('--audio-mix', choices=('game', 'host'), default='game',
                    help='where the port mixes audio: on the game thread by the emulated '
                         'clock, deterministic run to run (default), or on the SDL '
                         'thread by the host clock (the behaviour before timing round 6)')
    ap.add_argument('--start-card', metavar='MCR',
                    help='start each phase that is not keep-card from a copy of this '
                         '128 KiB card image (port saves/card1.mcr and the reference '
                         '-memcard alike) instead of a blank formatted card')
    ap.add_argument('--profile', choices=('parity', 'enhanced'),
                    help='the port\'s run profile (TOKI_PROFILE; pc/hal/profile.h). '
                         'Default: none set, i.e. parity. enhanced is not comparable '
                         'with --retail (docs/pc-port/ENHANCED-MODE.md)')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                    help='a profile switch or setting for the port (TOKI_TEXT_AUTO=1, '
                         'GAME_CPU_COST=0, ...); repeatable, applied after the env strip')
    ap.add_argument('--installed', action='store_true',
                    help='run --bin as a player install: no asset argument, no pc/assets or '
                         'disks/ in the work folder; the binary finds its own game data '
                         '(pc/tools/fresh_install.py)')
    ap.add_argument('--max-frames', type=int, default=40000,
                    help='game frames a phase may run on the port before it exits '
                         '(PC_TRACE_FRAMES; default %(default)s)')
    ap.add_argument('--shot-jobs', type=int,
                    default=int(os.environ.get('PLAYTHROUGH_SHOT_JOBS', '4')),
                    help='reference shot re-runs at once (--retail --shots)')
    a = ap.parse_args()
    global INPUT_CLOCK, AUDIO_MIX, START_CARD, MAX_FRAMES, PROFILE, SETTINGS, SHOT_JOBS, RUNNER, INSTALLED
    INSTALLED = a.installed
    RUNNER = shlex.split(a.runner)
    SHOT_JOBS = max(1, a.shot_jobs)
    MAX_FRAMES = a.max_frames
    PROFILE = a.profile
    for kv in a.set:
        if '=' not in kv:
            ap.error(f'--set {kv}: expected NAME=VALUE')
        k, v = kv.split('=', 1)
        SETTINGS[k] = v
    INPUT_CLOCK = a.input_clock
    AUDIO_MIX = a.audio_mix
    if a.start_card:
        if os.path.getsize(a.start_card) != 128 * 1024:
            print(f'{a.start_card}: not a 128 KiB card image')
            return 2
        START_CARD = os.path.abspath(a.start_card)
    try:
        script = parse(a.script)
    except ScriptError as e:
        print(f'script error: {e}')
        return 2
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    milestones, failures = run_pc(script, a.out, os.path.abspath(a.bin),
                                 'vram' if a.shots and a.vram else a.shots, a.timeout)
    if not failures:
        check_post(script, a.out, milestones, failures)
    report = dict(script=os.path.relpath(a.script, ROOT), binary=os.path.abspath(a.bin),
                  pc={k: dict(phase=v['phase'], game=v['game'], vblank=v['vblank'])
                      for k, v in milestones.items()},
                  failures=failures, folds=dict(FOLDS),
                  branches={'pc': dict(BRANCHES.get('pc', {}))})
    seeded = seeded_pokes(script)
    if seeded:
        # a seeded start (pc/tools/seeded_starts.py): state written at one
        # defined point on both machines; never a normal-run result
        report.update(mode='seeded', seeded_pokes=seeded)
        print(f'[seeded] SEEDED RUN: {len(seeded)} poke line(s), '
              f'{sum(p["bytes"] for p in seeded)} bytes', flush=True)
    if a.retail:
        refemu.require('--retail')
        rm, rf = run_retail(script, a.out, a.timeout, shots='vram' if a.shots and a.vram else a.shots)
        report['retail'] = {k: dict(phase=v['phase'], game=v['game'], host=v['vblank'])
                            for k, v in rm.items()}
        if not rf:
            check_post(script, a.out, rm, rf, prefix='ps1')
        report['retail_failures'] = rf
        cmp = {}
        for k, v in rm.items():
            if k in milestones:
                pr, rr = milestones[k]['row'], v['row']
                diff = sorted(f for f in set(pr) & set(rr)
                              if f not in ('frame', 'tick', 'crashes', 'frame_counter',
                                           'vblnk_counter', 'step_frame_cnt', 'pad_data',
                                           'pad_trig', 'pad_rel', 'disp_buf', 'rand_seed')
                              and pr[f] != rr[f])
                cmp[k] = dict(pc_game=milestones[k]['game'], retail_game=v['game'],
                              delta=milestones[k]['game'] - v['game'], differing_fields=diff,
                              rand=k in rand_labels(script))
                print(f'[cmp] {k:20s} pc {milestones[k]["game"]:6d} retail {v["game"]:6d} '
                      f'delta {milestones[k]["game"] - v["game"]:+5d}  differing: {",".join(diff) or "-"}')
        report['comparison'] = cmp
        # rand-dependent milestones (cases, `rand` waits): the branch each
        # side took, and the milestones only one side reached -- reported,
        # never a failure (the game's rand decided them)
        rl = rand_labels(script)
        report['rand_milestones'] = sorted(rl)
        report['branches'] = {side: dict(b) for side, b in BRANCHES.items()}
        for b in sorted(set(BRANCHES.get('pc', {})) | set(BRANCHES.get('retail', {}))):
            pcb, rb = BRANCHES.get('pc', {}).get(b, '-'), BRANCHES.get('retail', {}).get(b, '-')
            print(f'[branch] {b:20s} pc {pcb:20s} retail {rb:20s} '
                  f'{"same" if pcb == rb else "DIFFERS (rand-dependent)"}')
        for k in sorted(rl):
            on = (k in milestones, k in rm)
            if on == (True, True):
                print(f'[rand] {k:22s} reached on both (pc {milestones[k]["game"]}, '
                      f'retail {rm[k]["game"]})')
            elif any(on):
                print(f'[rand] {k:22s} reached on {"pc" if on[0] else "retail"} only')
        if a.shots:
            sd = {}
            for phase in script['phases']:
                for it in phase['items']:
                    if it['kind'] != 'shot':
                        continue
                    pcp = os.path.join(a.out, 'shots', phase['name'], it['label'] + '.ppm')
                    psp = os.path.join(a.out, 'shots-retail', phase['name'], it['label'] + '.ppm')
                    if os.path.exists(pcp) and os.path.exists(psp):
                        sd[it['label']] = shot_diff(pcp, psp)
                        print(f'[shot] {it["label"]:20s} {sd[it["label"]]}')
            report['shots'] = sd
    with open(os.path.join(a.out, 'report.json'), 'w') as fh:
        json.dump(report, fh, indent=2)
    for f in failures:
        print(f'FAIL: {f}')
    for f in report.get('retail_failures', ()):
        print(f'FAIL (retail): {f}')
    if failures or report.get('retail_failures'):
        return 1
    print(f'PASS{" (SEEDED RUN)" if seeded else ""}: {len(milestones)} milestones '
          f'({os.path.join(a.out, "report.json")})')
    return 0


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
