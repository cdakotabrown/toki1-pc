#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""windows_smoke.py -- boot the Windows build under Wine and compare it with
the Linux build, trace for trace.

Both binaries run headless with the same environment the playthroughs use
(dummy SDL devices, PC_TURBO, the pre_irq trace), in their own work
directories, for --frames game frames.  The Windows run must exit 0,
recover from no fault, fold no pointer, and write a trace byte-identical to
the Linux build's: the port is deterministic run to run, so any difference
is a portability bug (docs/pc-port/WINDOWS-BUILD.md lists the ones found).

    python3 pc/tools/windows_smoke.py [--frames 600] [--out build/windows-smoke]
        [--exe pc/build-win/tokimeki-pc.exe] [--linux pc/build/tokimeki-pc]
        [--no-compare]            only boot the Windows build (no Linux run)
        [--negative-control]      run Windows with CALL_CONTINUATION=0 (a
                                  documented timing-model switch): the traces
                                  must then DIFFER, or the comparison is blind
Exit 0 on success, 1 on a failed check, 2 when a binary or Wine is missing.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def workdir(out, name):
    work = os.path.join(out, name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, 'pc'))
    os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
    return work


def env_for(trace, frames, extra=()):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'CPU_COST', 'GP0_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               PC_TRACE=trace, PC_TRACE_PHASE='pre_irq', PC_TRACE_FRAMES=str(frames),
               TOKI_INPUT_DEVICE='pad', WINEDEBUG='-all')
    env.update(dict(extra))
    return env


def boot(name, command, out, frames, timeout, extra=()):
    work = workdir(out, name)
    trace = os.path.join(out, f'{name}.trace')
    if os.path.exists(trace):
        os.unlink(trace)
    log = os.path.join(out, f'{name}.log')
    r = run(command + [os.path.join(ROOT, 'pc/assets')], cwd=work,
            env=env_for(trace, frames, extra), timeout=timeout, log_path=log,
            max_output=256 * 1024 * 1024)
    text = open(log, errors='replace').read()
    problems = []
    if r.reason or r.returncode != 0:
        problems.append(f'{name}: process ended {r.reason or r.returncode} (log {log})')
    if f'PC_TRACE_FRAMES={frames} reached' not in text:
        problems.append(f'{name}: did not reach game frame {frames} (log {log})')
    if '[CRASH]' in text:
        problems.append(f'{name}: recovered from a fault ([CRASH] in {log})')
    if '[FOLD] total=0 ' not in text:
        problems.append(f'{name}: fault-fold recoveries or no fold report ({log})')
    if not os.path.exists(trace) or not os.path.getsize(trace):
        problems.append(f'{name}: no trace written')
    return trace, problems


def first_difference(a, b):
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        for n, (la, lb) in enumerate(zip(fa, fb), 1):
            if la != lb:
                return n, la.decode(errors='replace')[:160], lb.decode(errors='replace')[:160]
        rest_a, rest_b = fa.read(), fb.read()
    if rest_a or rest_b:
        return -1, 'one trace is longer', ''
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--frames', type=int, default=600)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/windows-smoke'))
    ap.add_argument('--exe', default=os.path.join(ROOT, 'pc/build-win/tokimeki-pc.exe'))
    ap.add_argument('--linux', default=os.path.join(ROOT, 'pc/build/tokimeki-pc'))
    ap.add_argument('--wine', default='wine')
    ap.add_argument('--timeout', type=int, default=1200)
    ap.add_argument('--no-compare', action='store_true')
    ap.add_argument('--negative-control', action='store_true')
    a = ap.parse_args(argv)
    wine = shutil.which(a.wine)
    if not wine or not os.path.isfile(a.exe):
        print(f'windows_smoke: needs {a.exe} (make -f pc/Makefile windows) and Wine')
        return 2
    if not a.no_compare and not os.path.isfile(a.linux):
        print(f'windows_smoke: needs the Linux build {a.linux} to compare with')
        return 2
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    extra = [('CALL_CONTINUATION', '0')] if a.negative_control else []
    # safe_process.run starts an .exe under Wine in a private prefix
    # (game_platform.py); another --wine is put in front explicitly
    command = [a.exe] if a.wine == 'wine' else [wine, a.exe]
    win_trace, problems = boot('windows', command, out, a.frames, a.timeout, extra)
    print(f'[smoke] windows: {"ok" if not problems else "; ".join(problems)}', flush=True)
    if a.no_compare:
        return 1 if problems else 0
    lin_trace, lin_problems = boot('linux', [a.linux], out, a.frames, a.timeout)
    print(f'[smoke] linux:   {"ok" if not lin_problems else "; ".join(lin_problems)}', flush=True)
    problems += lin_problems
    diff = None if problems else first_difference(lin_trace, win_trace)
    rows = sum(1 for _ in open(lin_trace, 'rb')) if os.path.exists(lin_trace) else 0
    if a.negative_control:
        if problems:
            print('[smoke] negative control: a run failed: ' + '; '.join(problems))
            return 1
        if diff is None:
            print('[smoke] negative control FAILED: the traces are identical although the '
                  'Windows run changed the timing model -- the comparison is blind')
            return 1
        print(f'[smoke] negative control ok: traces differ at line {diff[0]}')
        return 0
    if problems:
        return 1
    if diff:
        print(f'[smoke] TRACES DIFFER at line {diff[0]}:\n  linux:   {diff[1]}\n  windows: {diff[2]}')
        return 1
    print(f'[smoke] traces identical ({rows} lines, {a.frames} game frames)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
