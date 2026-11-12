#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run one scripted playthrough on two PC binaries and require the same game.

Two builds of the same sources (the plain binary and a reach-instrumented
copy, an -O1 or a sanitizer build) must behave identically: the port is
deterministic run to run, so any difference is build-dependent behaviour --
undefined behaviour in game or port code (an uninitialised stack read, an
access GCC reorders because it runs past its declared object, ...).

    python3 pc/tools/build_compare.py pc/tools/playthrough/spring_events.txt \\
        --a pc/build/tokimeki-pc --b build/reach/tokimeki-pc --out build/cmp
        [--states 1000,5000,...] [--frames N]

Each phase of the playthrough script (pc/tools/playthrough.py's format and
its numeric PC_INPUT, nothing here changes game state) runs on both binaries
side by side with PC_TRACE and the deterministic game-thread audio mix
(PC_PARITY_DIR with no anchor: SPU mixing and libsnd voice state follow the
emulated clock).  In the default mode the SDL audio thread mixes on the
host's clock, and libsnd's voice state, its key-offs and with them the
sequencer's interrupt time differ run to run even for one binary, which
would drown a build difference.

Compared: every per-VBlank trace row (all traced fields and the clock), and
with --states the 2 MB PS1 RAM of a save state at each game frame given
(PC_SAVESTATE; those runs use the debug toolkit's fixed layout), words
holding host code addresses of the respective binary excepted (function
pointers the port keeps in PS1 cells).  Reports the first differing row
and the symbols of differing RAM; exit 1 on a difference, 2 on a run
failure.

Build a variant (all objects of the normal link, the chosen ones recompiled
with extra flags; BUILD_DIR untouched):

    python3 pc/tools/build_compare.py --variant build/o1/tokimeki-pc \\
        --cflags=-O1 [--objs game|all|a.o,b.o] [--ldflags=...]

The reach-instrumented binary is audit_ps1_wide_access.py --build-reach.
"""
import argparse
import bisect
import os
import shlex
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playthrough as pt  # noqa: E402
from safe_process import run  # noqa: E402

ROOT = pt.ROOT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input  # noqa: E402
RAM_BASE, RAM_SIZE = 0x80000000, 2 << 20


# ------------------------------------------------------------------ variants

def link_command(build_dir):
    target = os.path.join(build_dir, 'tokimeki-pc')
    r = subprocess.run(['make', '-s', '-n', '-B', '-f', 'pc/Makefile',
                        'BUILD_DIR=%s' % build_dir, target],
                       cwd=ROOT, capture_output=True, text=True, timeout=600)
    text = r.stdout.replace('\\\n', ' ')        # recipe lines continued with a backslash
    link = [line for line in text.splitlines()
            if ' -o %s' % target in line and ' -c ' not in line]
    compiles = {}
    for line in text.splitlines():
        if ' -c ' in line and ' -o ' in line:
            argv = shlex.split(line)
            compiles[os.path.basename(argv[argv.index('-o') + 1])] = argv
    if not link:
        sys.exit('no link command for %s' % target)
    return shlex.split(link[-1]), compiles


def build_variant(build_dir, out, cflags, ldflags, objs):
    argv, compiles = link_command(build_dir)
    if objs == 'all':
        chosen = set(compiles)
    elif objs == 'game':
        chosen = {o for o, a in compiles.items() if 'pc/include/game/sony_bios_alias.h' in a}
    else:
        chosen = set(objs.split(','))
    odir = os.path.join(os.path.dirname(os.path.abspath(out)), 'variant-obj')
    os.makedirs(odir, exist_ok=True)

    def one(b):
        cmd, skip = [], False
        for a in compiles[b]:
            if skip:
                skip = False
                continue
            if a in ('-MF', '-MT', '-MQ', '-o'):
                skip = True
                continue
            if a in ('-MMD', '-MD', '-MP'):
                continue
            cmd.append(a)
        dst = os.path.join(odir, b)
        r = subprocess.run(cmd + cflags + ['-o', dst], cwd=ROOT, capture_output=True, text=True)
        return b, dst if r.returncode == 0 else None, r.stderr[-2000:]
    swap = {}
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for b, dst, err in ex.map(one, sorted(chosen)):
            if dst is None:
                sys.exit('could not compile %s:\n%s' % (b, err))
            swap[os.path.join(build_dir, 'obj', b)] = dst
    argv = [swap.get(a, a) for a in argv]
    argv[argv.index('-o') + 1] = os.path.abspath(out)
    subprocess.run(argv + ldflags, cwd=ROOT, check=True)
    return len(swap)


# ----------------------------------------------------------------- compare

def text_range(binary):
    out = subprocess.run(['objdump', '-h', binary], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) > 4 and p[1] == '.text':
            start = int(p[3], 16)
            return start, start + int(p[2], 16)
    return 0, 0


def ps1_symbols(binary):
    syms = []
    for line in subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            v = int(p[0], 16)
            if RAM_BASE <= v < RAM_BASE + RAM_SIZE:
                syms.append((v, p[2]))
    return syms


def sym_of(syms, addr):
    i = bisect.bisect_right([s[0] for s in syms], addr) - 1
    return '%s+0x%x' % (syms[i][1], addr - syms[i][0]) if i >= 0 else '0x%08x' % addr


def pram(state):
    out = state + '.pram'
    subprocess.run([sys.executable, os.path.join(ROOT, 'pc/tools/savestate_tool.py'),
                    'extract', state, 'pram', out], check=True, capture_output=True)
    with open(out, 'rb') as fh:
        return fh.read()


def ram_diffs(a, b, text_a, text_b):
    """[address] of differing 4-byte words, host code addresses excepted"""
    out = []
    for off in range(0, min(len(a), len(b)), 4):
        wa, wb = a[off:off + 4], b[off:off + 4]
        if wa == wb:
            continue
        va, vb = int.from_bytes(wa, 'little'), int.from_bytes(wb, 'little')
        if text_a[0] <= va < text_a[1] and text_b[0] <= vb < text_b[1]:
            continue
        out.append(RAM_BASE + off)
    return out


def trace_rows(path):
    with open(path, errors='replace') as fh:
        for line in fh:
            if line.startswith('frame='):
                yield line.rstrip('\n')


def first_trace_diff(pa, pb):
    n = 0
    for la, lb in zip(trace_rows(pa), trace_rows(pb)):
        n += 1
        if la != lb:
            da = dict(kv.split('=', 1) for kv in la.split())
            db = dict(kv.split('=', 1) for kv in lb.split())
            return n, da, db, [k for k in da if da.get(k) != db.get(k)]
    na, nb = sum(1 for _ in trace_rows(pa)), sum(1 for _ in trace_rows(pb))
    if na != nb:
        return min(na, nb) + 1, None, None, ['row count %d vs %d' % (na, nb)]
    return None


def run_side(label, binary, work, phase_input, trace, env_extra, timeout):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               PC_TRACE=trace, PC_TRACE_PHASE='pre_irq', PC_INPUT=phase_input,
               TOKI_INPUT_DEVICE='pad', PC_PARITY_FRAMES='1',
               PC_PARITY_DIR=os.path.join(work, 'parity'))
    env.update(env_extra)
    log = trace[:-len('.trace')] + '.log'
    return run([binary, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
               timeout=timeout, log_path=log, max_output=pt.PC_LOG_LIMIT)


def compare(script_path, bins, out, states, frames, timeout):
    script = pt.parse(script_path)
    syms = pt.trace_syms(script)
    texts = [text_range(b) for b in bins]
    psyms = ps1_symbols(bins[0])
    works = []
    for side in 'ab':
        w = os.path.join(out, 'work-' + side)
        shutil.rmtree(w, ignore_errors=True)
        os.makedirs(os.path.join(w, 'pc'))
        os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(w, 'pc/assets'))
        os.symlink(str(disc_input('disks')), os.path.join(w, 'disks'))
        works.append(w)
    status = 0
    for phase in script['phases']:
        name = phase['name']
        if not phase['keep']:
            for w in works:
                shutil.rmtree(os.path.join(w, 'saves'), ignore_errors=True)
        num = os.path.join(out, name + '.input')
        with open(num, 'w') as fh:
            fh.write(pt.numeric_phase(phase, script['hold'], False))
        results, traces = [None, None], []
        threads = []
        for i, side in enumerate('ab'):
            trace = os.path.join(out, '%s.%s.trace' % (name, side))
            traces.append(trace)
            extra = {'PC_TRACE_FRAMES': str(frames)}
            if syms:
                extra['PC_TRACE_SYMS'] = ','.join('%s:%d' % kv for kv in syms.items())
            if states:
                extra['PC_SAVESTATE'] = ','.join(
                    '%d:%s' % (n, os.path.join(out, '%s.%s.%d.tks' % (name, side, n))) for n in states)

            def go(i=i, side=side, trace=trace, extra=extra):
                results[i] = run_side(side, os.path.abspath(bins[i]), works[i], num, trace, extra, timeout)
            threads.append(threading.Thread(target=go))
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for side, r in zip('ab', results):
            if r.reason:
                print('[%s] phase %s: process ended %s' % (side, name, r.reason))
                status = max(status, 2)
        d = first_trace_diff(*traces)
        rows = sum(1 for _ in trace_rows(traces[0]))
        if d is None:
            print('[cmp] phase %s: traces identical (%d VBlank rows)' % (name, rows))
        else:
            n, da, db, keys = d
            status = max(status, 1)
            print('[cmp] phase %s: traces differ at row %d%s' % (
                name, n, ' (VBlank %s, game frame 0x%s)' % (da['frame'], da.get('frame_counter', '?'))
                if da else ''))
            for k in keys:
                print('       %-16s %s | %s' % (k, (da or {}).get(k, '')[:48], (db or {}).get(k, '')[:48]))
        for n in states:
            sa, sb = (os.path.join(out, '%s.%s.%d.tks' % (name, s, n)) for s in 'ab')
            if not (os.path.exists(sa) and os.path.exists(sb)):
                continue
            diffs = ram_diffs(pram(sa), pram(sb), *texts)
            if diffs:
                status = max(status, 1)
                where = sorted({sym_of(psyms, a).split('+')[0] for a in diffs})
                print('[cmp] phase %s frame %d: PS1 RAM differs in %d words: %s' % (
                    name, n, len(diffs), ' '.join(where[:12]) + (' ...' if len(where) > 12 else '')))
            else:
                print('[cmp] phase %s frame %d: PS1 RAM identical' % (name, n))
        for w in works:
            shutil.rmtree(os.path.join(w, 'parity'), ignore_errors=True)
        if status:
            break
    print('SAME' if status == 0 else ('DIFFERENT' if status == 1 else 'RUN FAILURE'))
    return status


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('script', nargs='?')
    ap.add_argument('--a', default=pt.PC_BIN, help='first binary (default pc/build/tokimeki-pc)')
    ap.add_argument('--b', help='second binary')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/build-compare'))
    ap.add_argument('--states', default='', help='game frames for PS1 RAM checkpoints, comma-separated')
    ap.add_argument('--frames', type=int, default=40000, help='game frames per phase at most')
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per phase')
    ap.add_argument('--variant', metavar='OUT', help='build a variant binary and exit')
    ap.add_argument('--build-dir', default='pc/build')
    ap.add_argument('--cflags', default='', help='with --variant: extra compile flags')
    ap.add_argument('--ldflags', default='', help='with --variant: extra link flags')
    ap.add_argument('--objs', default='game', help='with --variant: game, all, or a.o,b.o')
    a = ap.parse_args(argv)
    if a.variant:
        n = build_variant(a.build_dir, a.variant, shlex.split(a.cflags), shlex.split(a.ldflags), a.objs)
        print('linked %s with %d recompiled objects' % (a.variant, n))
        return 0
    if not a.script or not a.b:
        ap.error('a script and --b are required')
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    states = [int(x) for x in a.states.split(',') if x.strip()]
    return compare(a.script, [a.a, a.b], a.out, states, a.frames, a.timeout)


if __name__ == '__main__':
    sys.exit(main())
