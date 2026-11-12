#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the CPU time retail spends in each main-binary function, for the
port's game-code CPU cost model (pc/hal/cpu_cost.c).

The port runs game code natively in no emulated time.  On the console the
same code takes CPU time, and a frame whose work overruns the VBlank costs
one more VBlank (vblank_function skips/waits VSync on GsGetVcount), so the
port's frame_counter runs ahead.  The model charges, at each instrumented
function's ENTRY, the mean time retail spends from that function's entry to
the next function entry ("entry gap"), excluding interrupt exceptions.

Why entry gaps: if both sides enter the same functions in the same order,
charging each entry the time to the next one reproduces retail's time
between consecutive entries, and its placement, by construction.  A
function retail runs whose port implementation is not instrumented (the
HAL's SDK models: VSync, DrawSync, CdControl, LoadImage, BIOS thunks ...)
is still probed here: its entry ends the previous gap, its own gap goes
unused, and the HAL charges its time from its own measured model (except
the pure-CPU SDK functions it replaces, ABSORB below).  The mean is kept per
function, over all scenes and per scene (now_step), since the same function
walks different data in each scene; contexts within a scene are averaged.

  run      run the reference with entry probes on every main-binary function
           (config/symbol_names.json, scope main) and on the exception
           vector / the kernel's two ReturnFromException exits; inputs are
           replayed from a playthrough phase's reference trace (pad column)
  analyze  per-function entry-gap statistics from probe traces -> TSV
  emit     the TSV -> pc/hal/cpu_cost_table.c
  merge    add another TSV's rows for functions the first has none for
  emit-boot  the boot's TSV -> pc/hal/cpu_cost_boot_table.c (BOOT_ROWS)

The committed table (pc/tools/cpu_costs.tsv) is both phases of
pc/tools/playthrough/new_game.txt (`playthrough.py --retail` writes the
reference traces and starting cards):

  python3 pc/tools/measure_cpu_costs.py run --ref build/pt/new_game.ps1.trace \\
      --card build/pt/new_game.retail-start.mcd --frames 6100 --out build/cost/new_game.probe
  python3 pc/tools/measure_cpu_costs.py run --ref build/pt/reboot.ps1.trace \\
      --card build/pt/reboot.retail-start.mcd --frames 6273 --out build/cost/reboot.probe
  python3 pc/tools/measure_cpu_costs.py analyze build/cost/new_game.probe \\
      build/cost/reboot.probe --out pc/tools/cpu_costs.tsv
  python3 pc/tools/measure_cpu_costs.py emit pc/tools/cpu_costs.tsv

The boot (game frame 0: _main's init, from its entry to the first main-loop
frame) is not in those means (`analyze` starts at game frame 1).  Its rows,
for the functions only the boot enters, come from the new_game probe trace
cut at frame_counter 1 (2026-10-31; the port charges _main's init since
then, pc/src/main.c BOOT_CPU_COST):

  python3 pc/tools/measure_cpu_costs.py analyze build/cost/boot.probe \
      --lo 0 --hi 0 --out build/cost/boot.tsv
  python3 pc/tools/measure_cpu_costs.py merge pc/tools/cpu_costs.tsv build/cost/boot.tsv

The boot's own means (every function it enters, every call counted, not
only the functions no later frame enters) are a second table the port
uses while frame_counter is 0 (cpu_cost.c BOOT_ROWS, 2026-10-31 timing
round 5): a function the boot enters once (_spu_setInTransfer, 39 ticks
there) costs that, not its in-game mean (5,674, followed by a VAB wait).

  cp build/cost/boot.tsv pc/tools/cpu_costs_boot.tsv
  python3 pc/tools/measure_cpu_costs.py analyze-hal build/cost/boot.probe \
      --lo 0 --hi 0 --out pc/tools/cpu_costs_boot_hal.tsv
  python3 pc/tools/measure_cpu_costs.py emit-boot pc/tools/cpu_costs_boot.tsv \
      --hal pc/tools/cpu_costs_boot_hal.tsv

(build/cost/boot.probe: a 615-frame no-input `run`-style probe from
power-on, every main-binary entry; HAL replacements charged by name, the
HAL_SDK_COST sites, take the boot's row at game frame 0 too.)

Two more tables come from probe traces run with the sequencer-return probe
(@seqret, 2026-10-31: new_game phase 1 and date_save phase 1):

  analyze-ovl  entry gaps by CALLING function ('@ovl': overlay code) ->
               pc/tools/cpu_costs_ovl.tsv; `emit --ovl` adds the rows that
               differ from the scene / all-scene mean (cpu_cost.c ticks_for)
  analyze-irq  the libsnd sequencer tick inside each VBlank exception and
               the rest of the exception -> pc/tools/cpu_costs_irq.tsv;
               `emit-irq` -> pc/hal/cpu_cost_irq_table.c

  python3 pc/tools/measure_cpu_costs.py analyze-ovl build/cost/ng.probe \\
      build/cost/date.probe --out pc/tools/cpu_costs_ovl.tsv
  python3 pc/tools/measure_cpu_costs.py emit pc/tools/cpu_costs.tsv \\
      --ovl pc/tools/cpu_costs_ovl.tsv
  python3 pc/tools/measure_cpu_costs.py analyze-irq build/cost/ng.probe \\
      build/cost/date.probe --out pc/tools/cpu_costs_irq.tsv
  python3 pc/tools/measure_cpu_costs.py emit-irq pc/tools/cpu_costs_irq.tsv

HAL replacements of SDK functions that call other probed functions on
retail (ClearOTagR -> _otc / set_alarm / VSync(-1), DecDCTReset, StSetRing,
...; HAL_ABSORB) get rows from `analyze-hal` (their helpers entered from
inside them folded in, by the probe's caller) -> pc/tools/cpu_costs_hal.tsv,
which `emit --hal` puts in place of their plain gaps (2026-10-31: new_game
phase 1 and date_save phase 1, from power-on):

  python3 pc/tools/measure_cpu_costs.py analyze-hal build/cost/ng.probe \
      build/cost/date.probe --lo 0 --out pc/tools/cpu_costs_hal.tsv
  python3 pc/tools/measure_cpu_costs.py emit pc/tools/cpu_costs.tsv \
      --ovl pc/tools/cpu_costs_ovl.tsv --hal pc/tools/cpu_costs_hal.tsv

(each `run` ~12 minutes and ~1 GB of trace; REFEMU_REGTEST names the
reference, as for trace_compare.py; REFEMU_LIB, when set, is put on its
LD_LIBRARY_PATH -- the local/refemu/regtest wrapper sets up its own)
"""
import argparse
import bisect
import json
import os
import shutil
import subprocess
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_compare as tc  # noqa: E402
import refemu  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import spell  # noqa: E402
# Kernel exception entry and its two `jr k0; rfe` exits (SCPH-5500 kernel
# RAM image: 0x1010 is the interrupt/ReturnFromException path, 0x65CC the
# other one, seen during the BIOS boot).
EXCEPTION_PROBES = [('80000080', '@exc'), ('00001010', '@rfe'), ('000065CC', '@rfe')]
# The return address of libetc trapIntrVSync's VSync-callback call: libsnd's
# sequencer tick (SsSeqCalledTbyT, installed by _SsStart) returns here.  It
# closes the sequencer window of each VBlank exception (`analyze-irq`).
SEQ_RETURN_PROBE = ('800B43B4', '@seqret')


def probe_list():
    """(ADDR, name) of every main-binary function entry.  A recorded symbol
    strictly inside a registered C function's window (a cross-prologue
    head's body, e.g. func_8004F91C inside melt_font) is not an entry in
    the port, so it is not one here either."""
    syms = json.load(open(os.path.join(ROOT, spell('config/symbol_names.json'))))['symbols']
    reg = json.load(open(os.path.join(ROOT, spell('config/level3_functions.json'))))['functions']
    windows = sorted((int(f['address'], 16), int(f['address'], 16) + int(f['size']))
                     for f in reg if f.get('address') and f.get('size'))
    starts = [w[0] for w in windows]
    out = []
    for addr, v in syms.items():
        if v.get('kind') != 'func' or not addr.startswith('0x'):
            continue
        a = int(addr, 16)
        if not (v.get('scope') == 'main' or (v.get('scope') is None and a < 0x80132000)):
            continue
        i = bisect.bisect_left(starts, a) - 1
        if i >= 0 and windows[i][0] < a < windows[i][1]:
            continue
        out.append((f'{a:08X}', v['name']))
    return sorted(out)


def inside_names():
    """names probe_list() leaves out (to read traces probed before it did)"""
    keep = {n for _, n in probe_list()}
    syms = json.load(open(os.path.join(ROOT, spell('config/symbol_names.json'))))['symbols']
    return {v['name'] for v in syms.values() if v.get('kind') == 'func'} - keep


def reference_env():
    """the reference's environment: refemu.require, plus REFEMU_LIB on the
    library path when given"""
    refemu.require('measure_cpu_costs')
    lib = os.environ.get('REFEMU_LIB')
    if not lib:
        return dict(os.environ)
    path = os.environ.get('LD_LIBRARY_PATH')
    return dict(os.environ, LD_LIBRARY_PATH=lib + (':' + path if path else ''))


def cmd_run(a):
    env = reference_env()
    inputs = []
    for line in open(a.ref):
        if line.startswith('frame='):
            w = dict(kv.split('=', 1) for kv in line.split()[:2])
            if w['pad'] != '0000':
                inputs.append((int(w['frame']), w['pad']))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or '.', exist_ok=True)
    card = os.path.abspath(a.out) + '.mcd'
    shutil.copy(a.card, card)
    cmd = [tc.REGTEST, '-frames', str(a.frames), '-console', '-cpu', 'Interpreter',
           '-trace', os.path.abspath(a.out), '-inputhold', '1', '-memcard', card]
    for name, addr, size in tc.FIELDS:
        cmd += ['-tracereg', f'{addr:08X}', str(size), name]
    for hf, mask in inputs:
        cmd += ['-input', f'{hf}:{mask}']
    for addr, name in EXCEPTION_PROBES + [SEQ_RETURN_PROBE] + probe_list():
        cmd += ['-tracepc', addr, name]
    cmd += ['--', tc.DISC]
    with open(os.path.abspath(a.out) + '.log', 'w') as log:
        r = subprocess.run(cmd, cwd='/tmp', env=env, stdout=log, stderr=subprocess.STDOUT)
    return r.returncode


# BIOS calls the port times with its own model of the call alone (the ROM
# byte loops, src/main/pc-port/bios_trampolines.c): retail's gap after one
# runs on through the caller's code to its next call, which no port charge
# covers.  `run-boot --ret-from` probes their return addresses and
# `analyze-ret` measures that continuation per caller (2026-10-31: after
# girl_rec_init's bzero, 1,191 ticks).  strncpy / strcpy are not here: their
# models were measured to the caller's next call already.
RET_CALLEES = ('bzero', 'memcpy')


def cmd_run_boot(a):
    """a no-input reference run from power-on with every entry probed (as
    `run`), plus '@ret' probes at the return addresses of RET_CALLEES calls
    seen in an earlier probe trace (--ret-from)"""
    env = reference_env()
    cmd = [tc.REGTEST, '-frames', str(a.frames), '-console', '-cpu', 'Interpreter',
           '-trace', os.path.abspath(a.out)]
    for name, addr, size in tc.FIELDS:
        cmd += ['-tracereg', f'{addr:08X}', str(size), name]
    for addr, name in EXCEPTION_PROBES + [SEQ_RETURN_PROBE] + probe_list():
        cmd += ['-tracepc', addr, name]
    if a.ret_from:
        ras = set()
        with open(a.ret_from, errors='replace') as fh:
            for line in fh:
                if line.startswith('pc=') and line[3:].split(' ', 1)[0] in RET_CALLEES:
                    i = line.find(' ra=')
                    ras.add(line[i + 4:i + 12].upper())
        for ra in sorted(ras):
            cmd += ['-tracepc', ra, '@ret' + ra]
    cmd += ['--', tc.DISC]
    with open(os.path.abspath(a.out) + '.log', 'w') as log:
        r = subprocess.run(cmd, cwd='/tmp', env=env, stdout=log, stderr=subprocess.STDOUT)
    return r.returncode


def cmd_analyze_ret(a):
    """per (RET_CALLEES call, calling function, now_step): the mean ticks
    from its return (the '@ret' probe) to the next entry, exceptions
    excluded -> TSV rows 'name@ret step caller calls mean total'"""
    tot = defaultdict(lambda: [0, 0])
    open_calls = {}     # ra -> (name, caller, step)
    pending = None      # [key, t, excluded so far]
    depth, exc_t, g, step = 0, 0, 0, 0
    with open(a.trace, errors='replace') as fh:
        for line in fh:
            if line.startswith('frame='):
                i = line.find(' frame_counter=')
                if i >= 0:
                    g = int.from_bytes(bytes.fromhex(line[i + 15:i + 23]), 'little')
                i = line.find(' now_step=')
                if i >= 0:
                    step = int(line[i + 10:i + 12], 16)
                continue
            if not line.startswith('pc='):
                continue
            sp = line.split(' ', 2)
            lab = sp[0][3:]
            t = int(sp[1][5:])
            if lab == '@exc':
                if depth == 0:
                    exc_t = t
                depth += 1
                continue
            if lab == '@rfe':
                if depth:
                    depth -= 1
                    if depth == 0 and pending:
                        pending[2] += t - exc_t
                continue
            if depth:
                continue
            if lab.startswith('@ret'):
                ra = lab[4:].lower()
                if ra in open_calls:
                    pending = [open_calls.pop(ra), t, 0]
                continue
            if lab.startswith('@'):
                continue
            if pending:
                key = pending[0]
                tot[key][0] += 1
                tot[key][1] += t - pending[1] - pending[2]
                pending = None
            if lab in RET_CALLEES and a.lo <= g <= a.hi:
                i = sp[2].find('ra=')
                open_calls[sp[2][i + 3:i + 11]] = (lab, caller_of(sp[2]), step)
    rows = defaultdict(lambda: [0, 0])
    for (name, caller, st), (n, t) in tot.items():
        for k in ((name, -1, caller), (name, st, caller)):
            rows[k][0] += n
            rows[k][1] += t
    with open(a.out, 'w') as f:
        f.write('# name@ret\tnow_step (-1: all)\tcaller\tcalls\tmean_ticks\ttotal_ticks'
                '  (pc/tools/measure_cpu_costs.py analyze-ret: return to the next entry)\n')
        for (name, st, caller), (n, t) in sorted(rows.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2] or '')):
            if n and caller:
                f.write(f'{name}@ret\t{st}\t{caller}\t{n}\t{t // n}\t{t}\n')
    print(f'{len(rows)} rows -> {a.out}')
    return 0


# HAL models whose last measured stage runs from their last probed helper
# to the caller's next call (PutDrawEnv: from _addque2's SetIntrMask
# restore; PutDispEnv: from its last _ctl), a constant measured in
# vblank_function: that tail holds the CALLER's code after the return, so
# it differs by caller (draw_and_disp_env_init 552 against 285).
# `analyze-tail` measures it by caller (rows NAME@tail, for `emit-boot --ret`).
TAIL_HAL = {
    'PutDrawEnv': {'SetDrawEnv', 'get_cs', 'get_ce', 'get_ofs', 'get_mode', 'get_tw',
                   '_addque2', 'set_alarm', 'VSync', 'SetIntrMask', '_cwc', '_clr'},
    'PutDispEnv': {'_ctl', 'GetVideoMode'},
}
TAIL_IGNORE = {'StGetNext', 'CheckCallback'}


def cmd_analyze_tail(a):
    tot = defaultdict(lambda: [0, 0])
    cur = None          # [name, caller, step, last helper tick]
    depth, g, step = 0, 0, 0
    with open(a.trace, errors='replace') as fh:
        for line in fh:
            if line.startswith('frame='):
                i = line.find(' frame_counter=')
                if i >= 0:
                    g = int.from_bytes(bytes.fromhex(line[i + 15:i + 23]), 'little')
                i = line.find(' now_step=')
                if i >= 0:
                    step = int(line[i + 10:i + 12], 16)
                continue
            if not line.startswith('pc='):
                continue
            sp = line.split(' ', 2)
            lab = sp[0][3:]
            t = int(sp[1][5:])
            if lab == '@exc':
                depth += 1
                continue
            if lab == '@rfe':
                depth = max(0, depth - 1)
                continue
            if depth or lab.startswith('@') or lab in TAIL_IGNORE:
                continue
            if cur and lab in TAIL_HAL[cur[0]]:
                cur[3] = t
                continue
            if cur:
                for k in ((cur[0], -1, cur[1]), (cur[0], cur[2], cur[1])):
                    tot[k][0] += 1
                    tot[k][1] += t - cur[3]
                cur = None
            if lab in TAIL_HAL and a.lo <= g <= a.hi:
                cur = [lab, caller_of(sp[2]), step, t]
    with open(a.out, 'w') as f:
        f.write('# name@tail\tnow_step (-1: all)\tcaller\tcalls\tmean_ticks\ttotal_ticks'
                '  (pc/tools/measure_cpu_costs.py analyze-tail: last helper to the next entry)\n')
        for (name, st, caller), (n, t) in sorted(tot.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2] or '')):
            if n and caller:
                f.write(f'{name}@tail\t{st}\t{caller}\t{n}\t{t // n}\t{t}\n')
    print(f'{len(tot)} rows -> {a.out}')
    return 0


# HAL replacements of pure-CPU SDK functions (they charge their table entry
# by name, pc/hal/cpu_cost.h HAL_CPU_COST) that do not call the helpers
# retail's versions call: those helpers' time, in that context, is theirs.
ABSORB = {
    'GetTPage': {'GetGraphType'},
    'SetDrawMode': {'get_mode', 'get_tw'},
    # libgte InitGeom: exception_handler_install's critical section and the
    # BIOS FlushCache (47,725 ticks) -- the HAL's InitGeom (pc/hal/gte.c)
    # charges them as InitGeom's own
    'InitGeom': {'exception_handler_install', 'EnterCriticalSection',
                 'FlushCache', 'ExitCriticalSection'},
}


# HAL replacements of SDK functions that charge retail's time by name and
# whose retail versions call other probed functions the HAL does not
# (`analyze-hal`): the time from the function's entry to its caller's next
# call, with the helpers entered from inside it -- an entry of a listed
# helper whose caller (the probe's ra) is the function or another of its
# helpers -- folded in.  A caller test, not a name test: the game's own
# VSync(0) right after ClearOTagR returns is not ClearOTagR's.  Measured on
# retail (boot movie start, title, name entry): ClearOTagR 64 -> ~670
# (_otc, set_alarm, VSync(-1)), DecDCTReset 15 -> ~880.
HAL_ABSORB = {
    'ClearOTagR': {'_otc', 'set_alarm', 'VSync'},
    'DecDCTReset': {'ResetCallback', 'startIntr', 'MDEC_reset', 'MDEC_in',
                    'MDEC_in_sync', 'DecDCToutCallback', 'DMACallback',
                    'setIntrDMA', 'SetIntrMask'},
    'StSetRing': {'StClearRing', 'init_ring_status'},
    'StSetStream': set(),
    'StSetMask': set(),
    'CdIntToPos': set(),
    'CdPosToInt': set(),
    'ResetRCnt': set(),
    'GsGetVcount': set(),
    'GsClearVcount': set(),
    'SetDefDispEnv': set(),
    'SetDefDrawEnv': set(),
    # round 5 (2026-10-31): HAL replacements the boot calls (boot table only)
    'SsSetMVol': {'_spu_ioctl', '_spu_setCommonAttr'},
    'CdMix': {'CD_vol', 'CD_sync', 'VSync', 'CheckCallback'},
    'SsStart2': {'_SsStart', 'VSyncCallback', 'setIntrVSync'},
    'DecDCToutCallback': {'DMACallback', 'setIntrDMA', 'SetIntrMask'},
}


OVERLAY_BASE = 0x80132000


_CALLERS = None


def caller_of(rest):
    """the function the probe line's ra (the caller's return address) is in:
    a main-binary function name, '@ovl' for overlay code, None if unknown"""
    global _CALLERS
    if _CALLERS is None:
        pl = probe_list()
        _CALLERS = ([int(a, 16) for a, _ in pl], [n for _, n in pl])
    i = rest.find('ra=')
    if i < 0:
        return None
    ra = int(rest[i + 3:i + 11], 16)
    if ra >= OVERLAY_BASE:
        return '@ovl'
    j = bisect.bisect_right(_CALLERS[0], ra - 1) - 1
    return _CALLERS[1][j] if j >= 0 else None


def gaps(path, lo=0, hi=1 << 62, inside=frozenset(), by_step=False, ovl=False):
    """-> {name: [count, total, min, max]} over game frames lo..hi, or
    {(name, now_step): ...} with by_step (the scene the entry ran in), or
    {(name, now_step, caller): ...} with ovl (also by_step): the function
    that called it (caller_of), since the gap to the next entry is the rest
    of the callee plus the caller's code up to its next call -- after a call
    from overlay code it runs on through code neither side probes"""
    st = defaultdict(lambda: [0, 0, 1 << 62, 0])
    cur, cur_t = None, 0
    depth, saved = 0, None
    g = 0
    step = 0
    unbalanced = 0
    with open(path, errors='replace') as fh:
        for line in fh:
            if line.startswith('frame='):
                i = line.find(' frame_counter=')
                if i >= 0:
                    g = int.from_bytes(bytes.fromhex(line[i + 15:i + 23]), 'little')
                i = line.find(' now_step=')
                if i >= 0 and by_step:
                    step = int(line[i + 10:i + 12], 16)
                continue
            if not line.startswith('pc='):
                continue
            sp = line.split(' ', 2)
            lab = sp[0][3:]
            t = int(sp[1][5:])
            if lab == '@exc':
                if depth == 0:
                    if cur is not None and lo <= g <= hi:
                        s = st[cur]
                        d = t - cur_t
                        s[1] += d
                    saved = cur
                depth += 1
                continue
            if lab == '@rfe':
                if depth == 0:
                    unbalanced += 1
                    continue
                depth -= 1
                if depth == 0:
                    cur, cur_t = saved, t
                continue
            if depth:
                continue
            if lab in inside:
                continue    # not an entry in the port: its time stays with `cur`
            if cur is not None and lab in ABSORB.get(cur[0] if by_step else cur, ()):
                continue    # called from a HAL replacement that does not call it
            key = ((lab, step, caller_of(sp[2])) if ovl else (lab, step)) \
                if by_step else lab
            if lo <= g <= hi:
                if cur is not None:
                    st[cur][1] += t - cur_t
                s = st[key]
                s[0] += 1
            cur, cur_t = key, t
    if unbalanced:
        print(f'warning: {unbalanced} exception returns without an entry', file=sys.stderr)
    return st


def irq_gaps(path, lo=0, hi=1 << 62):
    """The sequencer inside VBlank exceptions: -> ({name: [count, total]},
    [base ticks per exception]).  A function entered between
    SsSeqCalledTbyT's entry and its return (@seqret, in libetc's
    trapIntrVSync) is charged the time to the next entry or to the return;
    `base` is the rest of the exception, entry to ReturnFromException."""
    st = defaultdict(lambda: [0, 0])
    bases = []
    g = 0
    depth = 0
    exc_t = None
    window = None        # [(name, tick)] while the sequencer runs
    seq_ticks = None     # this exception's sequencer time
    with open(path, errors='replace') as fh:
        for line in fh:
            if line.startswith('frame='):
                i = line.find(' frame_counter=')
                if i >= 0:
                    g = int.from_bytes(bytes.fromhex(line[i + 15:i + 23]), 'little')
                continue
            if not line.startswith('pc='):
                continue
            sp = line.split(' ', 2)
            lab = sp[0][3:]
            t = int(sp[1][5:])
            if lab == '@exc':
                if depth == 0:
                    exc_t, window, seq_ticks = t, None, None
                depth += 1
                continue
            if lab == '@rfe':
                if depth == 0:
                    continue
                depth -= 1
                if depth == 0 and seq_ticks is not None and lo <= g <= hi:
                    bases.append(t - exc_t - seq_ticks)
                continue
            if not depth:
                continue
            if lab == 'SsSeqCalledTbyT' and window is None and seq_ticks is None:
                window = [(lab, t)]
                continue
            if window is None:
                continue
            if lab == '@seqret':
                if lo <= g <= hi:
                    for (n, x), (_, y) in zip(window, window[1:] + [(lab, t)]):
                        st[n][0] += 1
                        st[n][1] += y - x
                seq_ticks = t - window[0][1]
                window = None
                continue
            window.append((lab, t))
    return st, bases


def cmd_analyze_ovl(a):
    """entries by caller: per function and calling function ('@ovl': overlay
    code), over all scenes and per scene (the rows `emit --ovl` adds)"""
    tot = defaultdict(lambda: [0, 0])
    inside = inside_names()
    for p in a.traces:
        for (name, step, caller), (n, t, _, _) in gaps(p, a.lo, a.hi, inside,
                                                       by_step=True, ovl=True).items():
            if not caller:
                continue
            for key in ((name, -1, caller), (name, step, caller)):
                tot[key][0] += n
                tot[key][1] += t
    with open(a.out, 'w') as f:
        f.write('# name\tnow_step (-1: all)\tcaller\tcalls\tmean_gap_ticks\ttotal_ticks'
                '  (pc/tools/measure_cpu_costs.py analyze-ovl: by calling function)\n')
        for key in sorted(tot):
            n, t = tot[key]
            if n:
                f.write(f'{key[0]}\t{key[1]}\t{key[2]}\t{n}\t{t // n}\t{t}\n')
    print(f'{len(tot)} rows -> {a.out}')
    return 0


def cmd_analyze_irq(a):
    tot = defaultdict(lambda: [0, 0])
    bases = []
    for p in a.traces:
        st, b = irq_gaps(p, a.lo, a.hi)
        bases += b
        for name, (n, t) in st.items():
            tot[name][0] += n
            tot[name][1] += t
    if not bases:
        print('no sequencer windows (traces probed without @seqret?)', file=sys.stderr)
        return 1
    bases.sort()
    med = bases[len(bases) // 2]
    with open(a.out, 'w') as f:
        f.write('# name\tcalls\tmean_gap_ticks\ttotal_ticks  (pc/tools/measure_cpu_costs.py '
                'analyze-irq: inside the VBlank exception\'s sequencer tick)\n')
        f.write(f'@vblank_base\t{len(bases)}\t{med}\t0\n')
        for name in sorted(tot):
            n, t = tot[name]
            f.write(f'{name}\t{n}\t{t // n}\t{t}\n')
    print(f'{len(bases)} VBlank exceptions: base median {med} '
          f'(p10 {bases[len(bases) // 10]}, p90 {bases[len(bases) * 9 // 10]})')
    for name in sorted(tot, key=lambda k: -tot[k][1])[:a.top]:
        n, t = tot[name]
        print(f'{name:32s} calls {n:8d} mean {t // n:8d} total {t:12d}')
    return 0


def cmd_emit_irq(a):
    rows, base = [], None
    with open(a.tsv) as fh:
        tsv_lines = fh.readlines()
    for line in tsv_lines:
        if line.startswith('#') or not line.strip():
            continue
        name, n, mean, _ = line.rstrip('\n').split('\t')
        if name == '@vblank_base':
            base = int(mean)
        elif int(mean) > 0:
            rows.append((name, int(mean), int(n)))
    rows.sort()
    with open(a.out, 'w') as f:
        f.write('/* GENERATED by pc/tools/measure_cpu_costs.py emit-irq from '
                f'{os.path.relpath(a.tsv, ROOT)} -- do not edit.\n'
                ' * Retail CPU ticks of the libsnd sequencer tick inside the VBlank\n'
                ' * exception: each function\'s entry to the next entry or to\n'
                ' * SsSeqCalledTbyT\'s return (mean), and the rest of the exception\n'
                ' * (median); pc/hal/cpu_cost.c. */\n'
                '#include "cpu_cost.h"\n\n'
                f'const unsigned cpu_cost_vblank_base = {base}u;\n\n'
                'const CpuCost cpu_cost_irq_table[] = {\n')
        for name, mean, n in rows:
            f.write(f'    {{"{name}", -1, {mean}u}},   /* {n} calls */\n')
        f.write('};\nconst unsigned cpu_cost_irq_count = '
                'sizeof cpu_cost_irq_table / sizeof cpu_cost_irq_table[0];\n')
    print(f'{len(rows)} rows, base {base} -> {a.out}')
    return 0


def cmd_analyze(a):
    tot = defaultdict(lambda: [0, 0])
    inside = inside_names()
    for p in a.traces:
        for (name, step), (n, t, _, _) in gaps(p, a.lo, a.hi, inside, by_step=True).items():
            for key in ((name, -1), (name, step)):
                tot[key][0] += n
                tot[key][1] += t
    rows = sorted(((t, n, key) for key, (n, t) in tot.items() if n), reverse=True)
    with open(a.out, 'w') as f:
        f.write('# name\tnow_step (-1: all)\tcalls\tmean_gap_ticks\ttotal_ticks'
                '  (pc/tools/measure_cpu_costs.py)\n')
        for t, n, (name, step) in sorted(rows, key=lambda r: r[2]):
            f.write(f'{name}\t{step}\t{n}\t{t // n}\t{t}\n')
    for t, n, (name, step) in [r for r in rows if r[2][1] < 0][:a.top]:
        print(f'{name:32s} calls {n:8d} mean {t // n:8d} total {t:12d}')
    return 0


def hal_gaps(path, lo=1, hi=1 << 62):
    """-> {(name, now_step): [count, total]} for the HAL_ABSORB functions,
    their helpers folded in (exceptions excluded, as in gaps())"""
    st = defaultdict(lambda: [0, 0])
    cur = None          # (name, step) of the open HAL_ABSORB function, else None
    cur_t = 0
    other = None        # any other open entry: closes cur's gap
    depth = 0
    g = step = 0
    with open(path, errors='replace') as fh:
        for line in fh:
            if line.startswith('frame='):
                i = line.find(' frame_counter=')
                if i >= 0:
                    g = int.from_bytes(bytes.fromhex(line[i + 15:i + 23]), 'little')
                i = line.find(' now_step=')
                if i >= 0:
                    step = int(line[i + 10:i + 12], 16)
                continue
            if not line.startswith('pc='):
                continue
            sp = line.split(' ', 2)
            lab = sp[0][3:]
            t = int(sp[1][5:])
            if lab == '@exc':
                if depth == 0 and cur is not None:
                    st[cur][1] += t - cur_t
                depth += 1
                continue
            if lab == '@rfe':
                if depth:
                    depth -= 1
                    if depth == 0:
                        cur_t = t
                continue
            if depth or lab.startswith('@'):
                continue
            if cur is not None:
                helpers = HAL_ABSORB[cur[0]]
                if lab in helpers and caller_of(sp[2]) in helpers | {cur[0]}:
                    continue
                st[cur][1] += t - cur_t
                cur = None
            if lab in HAL_ABSORB and lo <= g <= hi:
                cur, cur_t = (lab, step), t
                st[cur][0] += 1
    return st


def cmd_analyze_hal(a):
    tot = defaultdict(lambda: [0, 0])
    for p in a.traces:
        for (name, step), (n, t) in hal_gaps(p, a.lo, a.hi).items():
            for key in ((name, -1), (name, step)):
                tot[key][0] += n
                tot[key][1] += t
    with open(a.out, 'w') as f:
        f.write('# name\tnow_step (-1: all)\tcalls\tmean_gap_ticks\ttotal_ticks'
                '  (pc/tools/measure_cpu_costs.py analyze-hal: HAL_ABSORB folded in)\n')
        for (name, step), (n, t) in sorted(tot.items()):
            if n:
                f.write(f'{name}\t{step}\t{n}\t{t // n}\t{t}\n')
    for (name, step), (n, t) in sorted(tot.items()):
        if step < 0 and n:
            print(f'{name:24s} calls {n:8d} mean {t // n:8d}')
    return 0


# Functions whose retail time is a busy-wait on a device or a timer, not
# CPU work: their duration is the HAL's to model (VSync's v_wait, libcd's
# _waitfs, the MDEC DMA syncs, libgpu's alarm poll).  Never charged.
WAITS = {'v_wait', '_waitfs', 'MDEC_in_sync', 'MDEC_out_sync', 'DecDCTinSync',
         'DecDCToutSync', 'get_alarm', 'CdSync', 'CdReady', 'CdReadSync',
         'DrawSync', 'VSync', 'SpuIsTransferCompleted'}


# Never given a row by `merge`: functions whose port implementation models
# their time as waits on a device (the card driver's directory reads).
MERGE_SKIP = {'_bu_init'}


def cmd_merge(a):
    """rows of NEW whose function has no row in BASE (all-scene rows only):
    the boot (game frame 0, `analyze --lo 0 --hi 0`) runs functions no later
    frame enters -- _main's init, the BIOS thunks it calls"""
    with open(a.base) as fh:
        base = fh.readlines()
    have = {line.split('\t')[0] for line in base if not line.startswith('#')}
    added = []
    with open(a.new) as fh:
        for line in fh:
            if line.startswith('#') or not line.strip():
                continue
            name, step = line.split('\t')[:2]
            if step == '-1' and name not in have and name not in MERGE_SKIP \
                    and name not in WAITS:
                added.append(line)
    rows = [line for line in base if not line.startswith('#')] + added
    rows.sort(key=lambda r: (r.split('\t')[0], int(r.split('\t')[1])))
    with open(a.out or a.base, 'w') as f:
        f.writelines([line for line in base if line.startswith('#')] + rows)
    for line in added:
        print(line.rstrip())
    print(f'{len(added)} rows added')
    return 0


def cmd_emit(a):
    rows, glob = [], {}
    with open(a.tsv) as fh:
        lines = fh.readlines()
    if getattr(a, 'hal', None):
        # analyze-hal rows replace the plain gaps of those functions
        with open(a.hal) as fh:
            hal = [ln for ln in fh if not ln.startswith('#') and ln.strip()]
        names = {ln.split('\t')[0] for ln in hal}
        lines = [ln for ln in lines if ln.startswith('#') or ln.split('\t')[0] not in names] + hal
    for line in lines:
        if line.startswith('#') or not line.strip():
            continue
        name, step, n, mean, _ = line.rstrip('\n').split('\t')
        if name in WAITS:
            continue
        rows.append((name, int(step), int(mean), int(n)))
        if int(step) < 0:
            glob[name] = int(mean)
    # A scene's own mean where it is measured often enough and differs
    # (the same function walks different data in each scene); the
    # all-scene mean everywhere else.
    rows = [r for r in rows if r[1] < 0 and r[2] > 0 or
            r[1] >= 0 and r[3] >= a.min_calls and
            abs(r[2] - glob.get(r[0], 0)) > max(16, glob.get(r[0], 0) // 10)]
    # Called from overlay code (analyze-ovl): the gap runs on through that
    # overlay code, so the same function can cost ten times its usual
    # mean there.  Kept where measured often enough and different from the
    # row the port would use otherwise (the scene's, else the global one).
    if getattr(a, "ovl", None):
        kept = {(r[0], r[1]) for r in rows}
        with open(a.ovl) as fh:
            ovl_lines = fh.readlines()
        for line in ovl_lines:
            if line.startswith('#') or not line.strip():
                continue
            name, step, caller, n, mean, _ = line.rstrip('\n').split('\t')
            step, n, mean = int(step), int(n), int(mean)
            if name in WAITS or n < a.min_calls:
                continue
            base = next((r[2] for r in rows if r[0] == name and r[1] == step), None) \
                if (name, step) in kept else None
            if base is None:
                base = glob.get(name, 0)
            if abs(mean - base) > max(16, base // 10):
                rows.append((name, step, mean, n, caller))
    rows = [r if len(r) == 5 else r + ('',) for r in rows]
    rows.sort()
    with open(a.out, 'w') as f:
        f.write('/* GENERATED by pc/tools/measure_cpu_costs.py emit from '
                f'{os.path.relpath(a.tsv, ROOT)} -- do not edit.\n'
                ' * Retail CPU ticks from each function\'s entry to the next function\n'
                ' * entry (mean, interrupt exceptions excluded), over all scenes\n'
                ' * (now_step -1) and per scene; pc/hal/cpu_cost.c. */\n'
                '#include "cpu_cost.h"\n\n'
                'const CpuCost cpu_cost_table[] = {\n')
        for name, step, mean, n, caller in rows:
            if caller:
                f.write(f'    {{"{name}", {step}, {mean}u, "{caller}"}},   /* {n} calls from it */\n')
            else:
                f.write(f'    {{"{name}", {step}, {mean}u}},   /* {n} calls */\n')
        f.write('};\nconst unsigned cpu_cost_count = sizeof cpu_cost_table / sizeof cpu_cost_table[0];\n')
    print(f'{len(rows)} rows -> {a.out}')
    return 0


def cmd_emit_boot(a):
    """the boot's own rows (an analyze TSV of game frame 0: `--lo 0 --hi 0`)
    -> pc/hal/cpu_cost_boot_table.c.  The boot runs once, on the same data
    every power-on, so its means need no call threshold: a function the boot
    enters once costs exactly what that one entry took on retail.  The port
    uses these rows while frame_counter is 0 (cpu_cost.c BOOT_ROWS)."""
    rows = []
    with open(a.tsv) as fh:
        lines = fh.readlines()
    if getattr(a, 'hal', None):
        # analyze-hal rows of the boot replace those functions' plain gaps
        with open(a.hal) as fh:
            hal = [ln for ln in fh if not ln.startswith('#') and ln.strip()]
        names = {ln.split('\t')[0] for ln in hal}
        lines = [ln for ln in lines if ln.startswith('#') or ln.split('\t')[0] not in names] + hal
    for line in lines:
        if line.startswith('#') or not line.strip():
            continue
        name, step, n, mean, _ = line.rstrip('\n').split('\t')
        if name in WAITS or int(mean) <= 0:
            continue
        rows.append((name, int(step), int(mean), int(n)))
    rows = [r + ('',) for r in rows]
    for ret in getattr(a, 'ret', None) or []:
        with open(ret) as fh:
            for line in fh:
                if line.startswith('#') or not line.strip():
                    continue
                name, step, caller, n, mean, _ = line.rstrip('\n').split('\t')
                if int(step) >= 0 and int(mean) > 0:
                    rows.append((name, int(step), int(mean), int(n), caller))
    rows.sort()
    with open(a.out, 'w') as f:
        f.write('/* GENERATED by pc/tools/measure_cpu_costs.py emit-boot from '
                f'{os.path.relpath(a.tsv, ROOT)} -- do not edit.\n'
                ' * Retail CPU ticks from each function\'s entry to the next function\n'
                ' * entry in the boot (game frame 0; mean over the boot\'s calls, every\n'
                ' * call counted, interrupt exceptions excluded), over the boot\n'
                ' * (now_step -1) and per scene; pc/hal/cpu_cost.c BOOT_ROWS. */\n'
                '#include "cpu_cost.h"\n\n'
                'const CpuCost cpu_cost_boot_table[] = {\n')
        for name, step, mean, n, caller in rows:
            if caller:
                f.write(f'    {{"{name}", {step}, {mean}u, "{caller}"}},   /* {n} calls from it */\n')
            else:
                f.write(f'    {{"{name}", {step}, {mean}u}},   /* {n} calls */\n')
        f.write('};\nconst unsigned cpu_cost_boot_count = '
                'sizeof cpu_cost_boot_table / sizeof cpu_cost_boot_table[0];\n')
    print(f'{len(rows)} rows -> {a.out}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('run')
    p.add_argument('--ref', required=True, help='a playthrough phase reference trace (.ps1.trace)')
    p.add_argument('--card', required=True, help='the phase\'s starting memory card')
    p.add_argument('--frames', type=int, required=True)
    p.add_argument('--out', required=True)
    p = sub.add_parser('analyze')
    p.add_argument('traces', nargs='+')
    p.add_argument('--out', required=True)
    p.add_argument('--lo', type=int, default=1, help='first game frame (0 = BIOS boot)')
    p.add_argument('--hi', type=int, default=1 << 62)
    p.add_argument('--top', type=int, default=30)
    p = sub.add_parser('merge')
    p.add_argument('base', help='the committed TSV (pc/tools/cpu_costs.tsv)')
    p.add_argument('new', help='an analyze TSV (the boot: --lo 0 --hi 0)')
    p.add_argument('--out', help='default: rewrite BASE')
    p = sub.add_parser('emit')
    p.add_argument('tsv')
    p.add_argument('--out', default=os.path.join(ROOT, 'pc/hal/cpu_cost_table.c'))
    p.add_argument('--min-calls', type=int, default=16,
                   help='calls a scene needs before its own mean is used')
    p.add_argument('--ovl', help='analyze-ovl TSV: rows for calls from overlay code')
    p.add_argument('--hal', help='analyze-hal TSV: rows replacing those functions\' plain gaps')
    p = sub.add_parser('emit-boot')
    p.add_argument('tsv')
    p.add_argument('--hal', help='analyze-hal TSV of the boot: rows replacing those functions\' plain gaps')
    p.add_argument('--ret', action='append', help='analyze-ret / analyze-tail TSV of the boot: NAME@ret / NAME@tail rows by caller (repeatable)')
    p.add_argument('--out', default=os.path.join(ROOT, 'pc/hal/cpu_cost_boot_table.c'))
    p = sub.add_parser('run-boot')
    p.add_argument('--frames', type=int, required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--ret-from', help='a probe trace: probe the return addresses of its RET_CALLEES calls')
    p = sub.add_parser('analyze-tail')
    p.add_argument('trace')
    p.add_argument('--out', required=True)
    p.add_argument('--lo', type=int, default=0)
    p.add_argument('--hi', type=int, default=0)
    p = sub.add_parser('analyze-ret')
    p.add_argument('trace')
    p.add_argument('--out', required=True)
    p.add_argument('--lo', type=int, default=0)
    p.add_argument('--hi', type=int, default=0)
    p = sub.add_parser('analyze-hal')
    p.add_argument('traces', nargs='+')
    p.add_argument('--out', required=True)
    p.add_argument('--lo', type=int, default=1)
    p.add_argument('--hi', type=int, default=1 << 62)
    p = sub.add_parser('analyze-ovl')
    p.add_argument('traces', nargs='+')
    p.add_argument('--out', required=True)
    p.add_argument('--lo', type=int, default=1)
    p.add_argument('--hi', type=int, default=1 << 62)
    p = sub.add_parser('analyze-irq')
    p.add_argument('traces', nargs='+')
    p.add_argument('--out', required=True)
    p.add_argument('--lo', type=int, default=1)
    p.add_argument('--hi', type=int, default=1 << 62)
    p.add_argument('--top', type=int, default=20)
    p = sub.add_parser('emit-irq')
    p.add_argument('tsv')
    p.add_argument('--out', default=os.path.join(ROOT, 'pc/hal/cpu_cost_irq_table.c'))
    a = ap.parse_args()
    return {'run': cmd_run, 'analyze': cmd_analyze, 'emit': cmd_emit,
            'analyze-irq': cmd_analyze_irq, 'emit-irq': cmd_emit_irq,
            'analyze-ovl': cmd_analyze_ovl, 'merge': cmd_merge,
            'analyze-hal': cmd_analyze_hal, 'emit-boot': cmd_emit_boot,
            'run-boot': cmd_run_boot, 'analyze-ret': cmd_analyze_ret,
            'analyze-tail': cmd_analyze_tail}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
