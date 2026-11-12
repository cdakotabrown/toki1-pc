#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PC-vs-PS1 scripted run comparison: the PC equivalent of the reference
emulator's turbo scene runs.

Both sides run the same scripted pad input and write one line per frame with
the same game globals (scene step / sub-step / sub-sub-step, now_girl, the
frame counters, the pad words, ...).  The differ aligns the two traces on the
GAME's own frame counter (frame_counter, 0x800EF858, incremented once per
main-loop iteration by the matched _main) and reports the first frame and
field on which they disagree.

  PC side   pc/hal/trace.c (PC_TRACE, PC_INPUT, PC_TURBO, PC_TRACE_FRAMES)
  PS1 side  the reference emulator's regtest host with the local patches
            (-trace / -tracereg, added 2026-10-15): REFEMU_REGTEST, default
            local/refemu/regtest (pc/tools/refemu.py; not distributed)

Script file (shared by both sides; game-frame keyed, PSX button layout):
    # press Cross at game frame 400, Start at 1200
    hold 2
    400:cross
    1200:4000
    1260:start+cross

Usage:
    python3 pc/tools/trace_compare.py run  --script S --frames 3000 --out DIR [--scene N]
    python3 pc/tools/trace_compare.py pc   --script S --frames N --out pc.trace [--scene N]
    python3 pc/tools/trace_compare.py ps1  --script S --frames N --out ps1.trace [--state F.state]
    python3 pc/tools/trace_compare.py diff pc.trace ps1.trace [--ignore pad_data,pad_trig]

PS1 host frames vs game frames: the regtest schedules input by emulated
(host) frame, the script by game frame.  `ps1` calibrates iteratively: it
runs with the presses scheduled so far, reads from the trace the host frame
at which frame_counter reaches each press's game frame, schedules the next
press there, and repeats until every press is placed (a press changes the
game's path, so later presses can only be placed after earlier ones are in).
--scene N on the PC side starts at scene N as the port's SCENE= does; there is
no PS1 equivalent, so comparisons start from power-on (or a --state).
"""
import argparse
import os
import re
import signal
import sys
import tempfile

from safe_process import FILE_LIMIT, lines, run, terminate
import refemu

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input, spell  # noqa: E402
REGTEST = refemu.regtest()
DISC = os.environ.get('TOKI_DISC', str(disc_input('disks/Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue')))
PC_BIN = os.path.join(ROOT, 'pc/build/tokimeki-pc')
# The PS1 reference runs the reference emulator's Interpreter: the three CPU modes do
# not agree on interrupt timing.  During the boot movie the kernel VBlank
# event (vblank_counter_tick -> vblnk_counter) is dropped on some VBlanks,
# and how many depends on the mode -- at game frame 152, Interpreter 650,
# Recompiler 598, CachedInterpreter 534.  The interpreter checks interrupts
# per instruction, the others per block, so it is the one to trust.
REGTEST_CPU = refemu.CPU

# keep in step with pc/hal/trace.c FIELDS
FIELDS = [
    ('now_step', 0x800EF868, 1), ('now_sub_step', 0x800EF869, 1), ('now_sub_sub_step', 0x800EF86A, 1),
    ('now_girl', 0x800EF6BF, 1), ('frame_counter', 0x800EF858, 4), ('step_frame_cnt', 0x800EF85C, 4),
    ('vblnk_counter', 0x800EF854, 4), ('pad_data', 0x800EF6E0, 4), ('pad_trig', 0x800EF6E8, 4),
    ('pad_rel', 0x800EF6EC, 4), ('cd_state2', 0x800E8DA2, 1), ('disp_buf', 0x80127180, 4),
    ('step_mode', 0x800EF86D, 1), ('rand_seed', 0x80009010, 4),
    ('calendar', 0x800EE79D, 7), ('player', 0x800EE824, 0x6C),
]
EXTRA = []   # (name, address, size) from --syms: any global, by its config/symbol_names.json name


def set_syms(spec):
    """--syms name[:size],...: extra traced globals.  The PC side reads them
    by symbol name (PC_TRACE_SYMS, dlsym), the PS1 side at their retail
    address (config/symbol_names.json)."""
    EXTRA.clear()
    if not spec:
        return
    import json
    table = json.load(open(os.path.join(ROOT, spell('config/symbol_names.json'))))['symbols']
    by_name = {}
    for k, v in table.items():
        if ':' not in k and v.get('name'):
            by_name.setdefault(v['name'], (int(k, 16), v.get('size') or 4))
    for item in spec.split(','):
        name, _, size = item.partition(':')
        if name not in by_name:
            m = re.fullmatch(r'(?:D_|func_)([0-9A-Fa-f]{8})', name)
            if not m:
                sys.exit(f'--syms: {name} is not in config/symbol_names.json')
            addr, dsize = int(m.group(1), 16), 4
        else:
            addr, dsize = by_name[name]
        width = int(size, 0) if size else min(dsize, 4)
        if not 0 < width <= 256 or len(EXTRA) >= 64:
            raise ValueError('--syms supports up to 64 fields of 1-256 bytes')
        EXTRA.append((name, addr, width))


def syms_env():
    return ','.join(f'{n}:{z}' for n, _, z in EXTRA)


BUTTONS = {'select': 0x0001, 'l3': 0x0002, 'r3': 0x0004, 'start': 0x0008, 'up': 0x0010, 'right': 0x0020,
           'down': 0x0040, 'left': 0x0080, 'l2': 0x0100, 'r2': 0x0200, 'l1': 0x0400, 'r1': 0x0800,
           'triangle': 0x1000, 'circle': 0x2000, 'cross': 0x4000, 'square': 0x8000}


# ------------------------------------------------------------------ script
def parse_script(path):
    """-> (hold, [(game_frame, psx_mask)]) ; also writes a numeric copy for pc/hal/trace.c"""
    hold, presses = 2, []
    for raw in lines(path, max_bytes=1024 * 1024, max_line=4096):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('hold'):
            hold = max(1, int(line.split()[1]))
            continue
        f, m = line.split(':', 1)
        mask = 0
        for part in m.strip().split('+'):
            part = part.strip().lower()
            mask |= BUTTONS[part] if part in BUTTONS else int(part, 16)
        presses.append((int(f), mask & 0xFFFF))
    presses.sort()
    return hold, presses


def numeric_script(path, hold, presses):
    with open(path, 'w') as fh:
        fh.write(f'hold {hold}\n')
        for f, m in presses:
            fh.write(f'{f}:{m:04x}\n')


# ------------------------------------------------------------------ traces
LINE = re.compile(r'(\w+)=([0-9a-fA-F?]+)')


def parse_trace(path, max_rows=50000, max_decoded=64 * 1024 * 1024):
    """-> list of dict(frame=int, pad=int, <field>=int ...) ; hex fields decoded little-endian.
    The two limits bound memory; callers with long traces (playthrough.py
    phases) raise them explicitly."""
    rows = []
    retained = 0
    torn = None
    for line in lines(path):
        if not line.startswith('frame='):
            continue
        if torn is not None:
            raise torn
        row = {}
        try:
            for k, v in LINE.findall(line):
                if k in ('frame', 'tick'):
                    row[k] = int(v)          # tick: the regtest's global tick counter (decimal)
                elif k in ('pad', 'crashes'):
                    row[k] = int(v, 16) if k == 'pad' else int(v)
                elif '?' in v:
                    row[k] = None
                else:
                    row[k] = int.from_bytes(bytes.fromhex(v), 'little')
        except ValueError as e:
            # a process that died mid-write (an abort, a kill) leaves its
            # last row torn: dropped; a bad row before the end still fails
            torn = ValueError(f'{path}: malformed trace row ({e})')
            continue
        if not line.endswith('\n'):
            break                        # a torn final row that happened to parse
        if len(rows) >= max_rows:
            raise ValueError(f'trace exceeds {max_rows:,} rows; use shorter trace segments')
        retained += sys.getsizeof(row) + 8 + sum(
            sys.getsizeof(k) + sys.getsizeof(v) for k, v in row.items())
        if retained > max_decoded:
            raise ValueError(f'decoded trace exceeds {max_decoded >> 20} MiB; use shorter segments')
        rows.append(row)
    return rows


def by_game_frame(rows):
    """{frame_counter value: first row that shows it}"""
    out = {}
    for r in rows:
        g = r.get('frame_counter')
        if g is not None and g not in out:
            out[g] = r
    return out


# ------------------------------------------------------------------ PC
def run_pc(script, frames, out, scene=None, extra_env=None, timeout=900):
    out = os.path.abspath(out)
    hold, presses = parse_script(script)
    num = out + '.input'
    numeric_script(num, hold, presses)
    env = dict(os.environ)
    env.update({'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'PC_TURBO': '1',
                'PC_TRACE': out, 'PC_INPUT': num, 'PC_TRACE_FRAMES': str(frames),
                # Regtest observes the VBlank edge before CPU IRQ delivery.
                'PC_TRACE_PHASE': 'pre_irq'})
    if EXTRA:
        env['PC_TRACE_SYMS'] = syms_env()
    if scene is not None:
        env['SCENE'] = str(scene)
    if extra_env:
        env.update(extra_env)
    log = out + '.log'
    # A previous failed invocation must never supply the next run's trace.
    if os.path.exists(out):
        os.remove(out)
    # Keep cards/audio dumps out of the checkout and other concurrent runs.
    with tempfile.TemporaryDirectory(prefix='trace_pc_') as work:
        os.mkdir(os.path.join(work, 'pc'))
        os.symlink(os.path.join(ROOT, 'pc/assets'),
                   os.path.join(work, 'pc/assets'), target_is_directory=True)
        os.symlink(str(disc_input('disks')), os.path.join(work, 'disks'),
                   target_is_directory=True)
        r = run([PC_BIN, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
                timeout=timeout, log_path=log,
                watch_files=[(out, FILE_LIMIT, 'TRACE_LIMIT')])
    r.require_ok()
    code = r.returncode
    rows = parse_trace(out) if os.path.exists(out) else []
    if not rows:
        raise RuntimeError('native capture produced no trace records')
    last_game_frame = max(row.get('frame_counter', 0) for row in rows)
    if last_game_frame < frames:
        raise RuntimeError(f'native reached only {last_game_frame}/{frames} game frames')
    print(f'[pc] exit={code} frames traced={len(rows)} log={log}')
    return rows


# ------------------------------------------------------------------ PS1
def regtest(frames, trace_out, inputs, hold, state=None, extra=None, timeout=1800,
            limits=None):
    trace_out = os.path.abspath(trace_out)
    if not (os.path.isfile(REGTEST) and os.access(REGTEST, os.X_OK)):
        raise RuntimeError(refemu.explain('the PS1 side of a trace comparison'))
    if state:
        state = os.path.abspath(state)
    cmd = [REGTEST, '-frames', str(frames), '-console', '-cpu', REGTEST_CPU, '-trace', trace_out,
           '-inputhold', str(hold)]
    for name, addr, size in FIELDS + EXTRA:
        cmd += ['-tracereg', f'{addr:08X}', str(size), name]
    for hf, mask in sorted(inputs):
        cmd += ['-input', f'{hf}:{mask:04X}']
    if state:
        cmd += ['-loadstate', state]
    if extra:
        cmd += extra
    cmd += ['--', os.path.abspath(DISC)]
    env = dict(os.environ)
    if os.path.exists(trace_out):
        os.remove(trace_out)
    r = run(cmd, cwd='/tmp', env=env, timeout=timeout,
            log_path=trace_out + '.log',
            watch_files=[(trace_out, FILE_LIMIT, 'TRACE_LIMIT')])
    r.require_ok()
    return parse_trace(trace_out, **(limits or {})) if os.path.exists(trace_out) else []


def run_ps1(script, frames, out, state=None, boot_slack=None):
    """iterative calibration: place each game-frame press at the host frame where
    frame_counter reaches it (see module docstring)"""
    hold, presses = parse_script(script)
    inputs = []        # (host_frame, mask)
    placed = 0
    host_frames = None
    rounds = 0
    while True:
        rounds += 1
        # run long enough for the game to reach `frames` game frames: unknown
        # boot offset, so grow the host budget from what the last run showed
        budget = host_frames if host_frames else frames + (boot_slack or 2400)
        rows, gmap = [], {}   # release the previous calibration before parsing
        rows = regtest(budget, out, inputs, hold, state)
        gmap = by_game_frame(rows)
        if not gmap:
            raise RuntimeError('reference capture produced no game-frame records')
        last_g = max(gmap)
        if placed < len(presses):
            g, mask = presses[placed]
            if g in gmap:
                inputs.append((gmap[g]['frame'], mask))
                placed += 1
                print(f'[ps1] round {rounds}: press {placed}/{len(presses)} game frame {g} -> host frame {gmap[g]["frame"]}')
                continue
            if last_g < frames and last_g < g and budget < frames * 4 + 20000:
                host_frames = budget + (g - last_g) + 600
                print(f'[ps1] round {rounds}: game frame {g} not reached (last {last_g}); host budget -> {host_frames}')
                continue
            raise RuntimeError(f'reference cannot reach scripted game frame {g} '
                               f'(last {last_g}); input was not applied')
        if last_g >= frames:
            print(f'[ps1] done: {rounds} rounds, {len(inputs)} presses placed, game frames reached {last_g}, host frames {budget}')
            return rows
        if budget >= frames * 4 + 20000:
            raise RuntimeError(f'reference reached only {last_g}/{frames} game frames')
        host_frames = budget + (frames - last_g) + 600
        print(f'[ps1] round {rounds}: {last_g} game frames of {frames}; host budget -> {host_frames}')


# ------------------------------------------------------------------ diff
def transitions(gmap, keys=('now_step', 'now_sub_step', 'now_sub_sub_step')):
    out, last = [], None
    for g in sorted(gmap):
        cur = tuple(gmap[g].get(k) for k in keys)
        if cur != last:
            out.append((g, cur))
            last = cur
    return out


def fmt(v):
    if v is None:
        return '?'
    return f'{v:x}' if v > 0xFFFFFFFF else str(v)


def diff(pc_path, ps1_path, ignore=(), context=5):
    phase = None
    for line in lines(pc_path):
        if line.startswith('# observation='):
            phase = line.strip().split('=', 1)[1]
            break
        if line.startswith('frame='):
            break
    if phase is not None and phase != 'pre_irq':
        print('incompatible native observation phase; recapture with PC_TRACE_PHASE=pre_irq')
        return 2
    if phase is None:
        print('WARNING: legacy native observation phase is unverified')
    pc = by_game_frame(parse_trace(pc_path))
    ps = by_game_frame(parse_trace(ps1_path))
    fields = [n for n, _, _ in FIELDS + EXTRA if n not in ignore and n != 'frame_counter']
    # any other field both traces carry (PC_TRACE_SYMS / -tracereg of an earlier run)
    a_rows, b_rows = next(iter(pc.values()), {}), next(iter(ps.values()), {})
    fields += [k for k in a_rows if k in b_rows and k not in fields and k not in ignore
               and k not in ('frame', 'tick', 'pad', 'crashes', 'frame_counter')]
    # game frame 0 is the power-on / BIOS / init stretch on the PS1 side
    # (frame_counter is 0 until system_reset's first frame), not comparable
    common = sorted((set(pc) & set(ps)) - {0})
    if not common:
        print('no common game frames (did the PS1 side reach the game loop?)')
        return 2
    first = None
    for g in common:
        bad = [f for f in fields if pc[g].get(f) != ps[g].get(f)]
        if bad:
            first = (g, bad)
            break
    print(f'common game frames: {common[0]}..{common[-1]} ({len(common)}); pc max {max(pc)}, ps1 max {max(ps)}')
    tp, ts = transitions(pc), transitions(ps)
    print('state transitions (game frame: step/sub/subsub)  PC | PS1')
    n = max(len(tp), len(ts))
    for i in range(min(n, 60)):
        a = f'{tp[i][0]:6d}: {tp[i][1]}' if i < len(tp) else ''
        b = f'{ts[i][0]:6d}: {ts[i][1]}' if i < len(ts) else ''
        mark = '' if a.split(':', 1)[-1] == b.split(':', 1)[-1] else '   <-- differs'
        print(f'  {a:32s} | {b:32s}{mark}')
    if not first:
        print(f'IDENTICAL on {fields} over {len(common)} game frames')
        return 0
    g, bad = first
    print(f'\nFIRST DIVERGENCE at game frame {g}: {bad}')
    lo = max(common[0], g - context)
    print(f'{"g":>6s} ' + ' '.join(f'{f:>16s}' for f in bad))
    for k in range(lo, min(g + context, common[-1]) + 1):
        if k in pc and k in ps:
            print(f'{k:6d} ' + ' '.join(f'{fmt(pc[k].get(f))}/{fmt(ps[k].get(f))}' for f in bad) + '   (pc/ps1)')
    for f in bad:
        a, b = pc[g].get(f), ps[g].get(f)
        if isinstance(a, int) and isinstance(b, int) and max(a, b) > 0xFFFFFFFF:
            ab, bb = a.to_bytes(256, 'little'), b.to_bytes(256, 'little')
            offs = [i for i in range(256) if ab[i] != bb[i]]
            print(f'  {f}: bytes differ at offsets {offs[:24]}')
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pc'); p.add_argument('--script', required=True); p.add_argument('--frames', type=int, required=True)
    p.add_argument('--out', required=True); p.add_argument('--scene', type=int); p.add_argument('--syms', default='')
    p = sub.add_parser('ps1'); p.add_argument('--script', required=True); p.add_argument('--frames', type=int, required=True)
    p.add_argument('--out', required=True); p.add_argument('--state'); p.add_argument('--syms', default='')
    p = sub.add_parser('diff'); p.add_argument('pc'); p.add_argument('ps1'); p.add_argument('--ignore', default='')
    p.add_argument('--context', type=int, default=5)
    p = sub.add_parser('run'); p.add_argument('--script', required=True); p.add_argument('--frames', type=int, required=True)
    p.add_argument('--out', required=True); p.add_argument('--scene', type=int); p.add_argument('--state')
    p.add_argument('--ignore', default=''); p.add_argument('--syms', default='')
    a = ap.parse_args()
    if hasattr(a, 'frames') and not 0 < a.frames <= 40000:
        ap.error('--frames must be within 1-40000; split longer runs')
    set_syms(getattr(a, 'syms', ''))
    if a.cmd == 'pc':
        run_pc(a.script, a.frames, a.out, a.scene); return 0
    if a.cmd == 'ps1':
        run_ps1(a.script, a.frames, a.out, a.state); return 0
    if a.cmd == 'diff':
        return diff(a.pc, a.ps1, [x for x in a.ignore.split(',') if x], a.context)
    os.makedirs(a.out, exist_ok=True)
    pct, pst = os.path.join(a.out, 'pc.trace'), os.path.join(a.out, 'ps1.trace')
    run_pc(a.script, a.frames, pct, a.scene)
    run_ps1(a.script, a.frames, pst, a.state)
    return diff(pct, pst, [x for x in a.ignore.split(',') if x])


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
