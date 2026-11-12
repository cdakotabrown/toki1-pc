#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Scene matrix: run every scene id headless, uncapped, for N game frames
and record how it ends.

For each scene: SCENE_NAV=<id> PC_TURBO=1 PC_TRACE_FRAMES=<frames>
(pc/hal/trace.c), no input.  SCENE_NAV runs the REAL boot first (scene-16
movie, scene 0's boot_data_load, the icon_init slot population) and then
navigates to the scene with goto_step -- the faithful scenario, so the
matched code runs against boot state as on the console (the old SCENE=
direct start skipped it and crashed in sprite_slot_disp).  --frames counts
from the navigation (trace_anchor), not from power-on. --clock vblank uses
hardware frames for standalone loops (TT); default game preserves the
main-loop-frame budget. The chosen clock is recorded in the JSON report.
Columns:

  scene    the id started
  end      step/sub/subsub at the last traced frame (where it went)
  in       whether the requested scene was actually reached:
             entered  = the last frame is the requested step
             advanced = the scene legitimately moved on (scene 16 is the
                        movie, 48/49/56/57 hand off to 64, 50 to 34, 65 to 83)
             bounced  = back at the title (18): goto_step(N) was rejected
                        because game_main has no case for N (`default:
                        game_reset()`), so the row proves nothing about N
  frames   traced VBlanks, including boot, before exit / timeout
  crashes  recovered crashes (SIGSEGV/SIGALRM caught by pc/src/main.c)
  folds    sign-extended PS1 addresses recovered by pc/src/main.c's fault fold
           (`[FOLD] total=N`, or the `[FOLD] site` lines when the process
           died before its summary) -- not a crash, but a latent 64-bit
           defect: a comparison or a range check on such a value is wrong
  first    the first crash's function (sym=... of the first [CRASH] line) --
           the first failing function for the matrix
  status   OK (no crash, no fold, ran to the frame limit) / CRASH / FOLD
           (a fault-fold recovery, no crash) / HANG (wall-clock timeout
           without reaching the frame limit) / EXIT (quit early) /
           OUTPUT_LIMIT (8 MiB) / TRACE_LIMIT (16 MiB, polled) / CANCELLED

The scene list defaults to the steps game_main actually dispatches, read out of
`src/main/matched/game_main.c` (see `enterable_steps`).  The old default was the
raw range 0-210: 154 of those ids are internal sub-steps, so `goto_step(N)` fell
into `default: game_reset()` and the row silently measured the title instead of
the scene.  `--scenes all` keeps the raw range for a crash smoke test; `bounced`
rows make the run fail, because a requested enterable step that lands back at the
title is a real regression, not a scene that finished.

Usage:
    python3 pc/tools/scene_matrix.py [--frames 300] [--timeout 60] [--scenes 0-210|all|enterable] [--jobs N] [--out FILE]
Writes a text table (default build/scene_matrix.txt) and prints a summary.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from safe_process import run
import game_platform

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import decomp, disc_input  # noqa: E402
PC_BIN = str(game_platform.GAME)   # PC_TEST_PLATFORM=windows: the .exe under Wine
GAME_MAIN_C = os.path.join(ROOT, decomp('src/main/matched/game_main.c'))
STEP_REDIRECT_C = os.path.join(ROOT, decomp('src/main/matched/step_state_save_and_redirect.c'))
TITLE_STEP = 18

# game_main dispatches these, but they are not scenes: 0/1 are the boot chain
# (0 re-runs boot_data_load, 1 is its gate), 2 is overlay_load_main -- the step
# the redirect sends a real scene THROUGH on its way in, 254 is
# screen_offset_adjust and 255 exit_function.  Navigating straight at them
# measures the boot or loops forever (254 emitted 31,010 frames), so they are
# excluded from the default sweep and stay reachable with `--scenes all`.
NOT_A_SCENE = {0, 1, 2, 254, 255}


def enterable_steps():
    """The `now_step` values game_main dispatches, read from its switch.

    Everything else reaches `default: game_reset()`, and game_reset ends with
    `goto_step(18)` -- that is the bounce the old 0-210 default measured as
    "OK".  Parsed rather than hardcoded so a step added to the game cannot
    silently drop out of the sweep; parsed defensively so a refactor that
    changes the file's shape fails loudly instead of returning [] or half a
    list.
    """
    with open(GAME_MAIN_C) as fh:
        text = fh.read()
    steps = sorted({int(m.group(1)) for m in re.finditer(r'^\s*case (\d+):', text, re.M)})
    if len(steps) < 40:
        raise SystemExit(f'[scene_matrix] only {len(steps)} case labels in {GAME_MAIN_C}; '
                         'the switch shape changed -- fix the parser, do not sweep')
    if not re.search(r'^\s*default:\s*\n?\s*game_reset\(\);', text, re.M):
        raise SystemExit('[scene_matrix] game_main.c no longer ends the switch in game_reset(); '
                         're-derive the enterable set before trusting any sweep')
    return steps


def redirect_steps():
    """Steps step_state_save_and_redirect sends through step 2 first.

    Those are the scenes that need the retail sequence (step -> 2 ->
    overlay_load_main -> handler) rather than a bare `now_step = N`; the
    matrix's goto_step call reproduces it because goto_step runs the redirect
    itself.  Read for the report, and to explain a bounce that is not the
    default: case.
    """
    with open(STEP_REDIRECT_C) as fh:
        text = fh.read()
    return sorted({int(m.group(1)) for m in re.finditer(r'^\s*case (\d+):', text, re.M)})


def scene_steps():
    """The enterable set minus the steps that are not scenes at all."""
    steps = [s for s in enterable_steps() if s not in NOT_A_SCENE]
    if len(steps) < 40:
        raise SystemExit(f'[scene_matrix] only {len(steps)} scene steps after removing '
                         f'{sorted(NOT_A_SCENE)}; NOT_A_SCENE is out of date')
    return steps


LOGDIR = None   # --logdir: keep the output of every scene that is not OK
OUTPUT_LIMIT = 8 * 1024 * 1024
TRACE_LIMIT = 16 * 1024 * 1024
FOLD_TOTAL = re.compile(r'\[FOLD\] total=(\d+)')
STOP = threading.Event()


def capture_scene(env, trace, log, timeout):
    r = run([PC_BIN, os.path.join(ROOT, 'pc/assets')],
            cwd=os.path.dirname(trace), env=env, timeout=timeout,
            max_output=OUTPUT_LIMIT, log_path=log, stop=STOP,
            watch_files=[(trace, TRACE_LIMIT, 'TRACE_LIMIT')])
    return r.returncode, 'HANG' if r.reason == 'TIMEOUT' else r.reason


def run_scene(scene, frames, timeout, clock='game'):
    if STOP.is_set():
        return dict(scene=scene, end='?', frames=0, crashes=0,
                    first='', status='CANCELLED', folds=0)
    with tempfile.TemporaryDirectory(prefix=f'scene{scene}_') as work:
        return scene_in_dir(scene, frames, timeout, work, clock)


def scene_in_dir(scene, frames, timeout, work, clock='game'):
    trace = os.path.join(work, 'frame.trace')
    log = os.path.join(work, 'output.log')
    # Each scene gets private diagnostics and saves; inputs stay read-only.
    os.makedirs(os.path.join(work, 'pc'), exist_ok=True)
    os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
    os.symlink(str(disc_input('disks')), os.path.join(work, 'disks'))
    env = dict(os.environ)
    env.pop('SCENE', None)
    env.pop('PC_POKE', None)    # a state-writing test fixture (pc/hal/trace.c)
    env.update({'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'PC_TURBO': '1',
                'PC_TRACE': trace, 'PC_TRACE_FRAMES': str(frames),
                'SCENE_NAV': str(scene), 'PC_TRACE_CLOCK': clock})
    code, reason = capture_scene(env, trace, log, timeout)
    if os.path.exists(trace) and os.path.getsize(trace) > TRACE_LIMIT:
        reason = reason or 'TRACE_LIMIT'
    crashes, first, reached = 0, '', False
    folds = fold_sites = 0
    # Did the game ever dispatch the requested step?  `SCENE_CHANGE ... new=N`
    # is the port's handler log; it separates "the scene ran and then chose to
    # leave" (state-dependent scenes such as SHOUGATU 91, which needs the
    # calendar the title does not set up) from "goto_step(N) never took".
    entered_log = re.compile(rf'SCENE_CHANGE old=\d+ new={scene}\b')
    scene_ran = False
    # Bounded readline also handles a broken logger that never emits newlines.
    with open(log, errors='replace') as fh:
        while line := fh.readline(65536):
            crashes += line.count('[CRASH] sig=')
            fm = FOLD_TOTAL.search(line)
            if fm:
                folds = max(folds, int(fm.group(1)))
            fold_sites += line.startswith('[FOLD] site ')
            m = re.search(r'\[CRASH\] sig=(\d+) pc=\S+ sym=(\S+)', line)
            if m and not first:
                first = f'{m.group(2)} (sig {m.group(1)})'
            reached |= re.search(r'PC_TRACE_FRAMES=\d+ reached', line) is not None
            if not scene_ran and entered_log.search(line):
                scene_ran = True
    last, n = None, 0
    if os.path.exists(trace):
        with open(trace, errors='replace') as fh:
            remaining = TRACE_LIMIT
            while remaining > 0:
                line = fh.readline(min(65536, remaining))
                if not line:
                    break
                remaining -= len(line)
                if line.startswith('frame='):
                    n += 1
                    last = line
    end = '?'
    if last:
        f = dict(re.findall(r'(\w+)=([0-9a-f]+)', last))
        end = f"{int(f.get('now_step', '00'), 16)}/{int(f.get('now_sub_step', '00'), 16)}/{int(f.get('now_sub_sub_step', '00'), 16)}"
        # SIGALRM recovery need not emit a [CRASH] line. The game's trace
        # counter also covers those recoveries and interleaved diagnostics.
        crashes = max(crashes, int(f.get('crashes', '0'), 10))
    if reason:
        status = reason
    elif crashes:
        status = 'CRASH'
    elif folds or fold_sites:
        # a sign-extended PS1 address reached a dereference: the build's
        # zero-extension (pc/tools/adapt_ps1_addresses.py) missed a path
        status = 'FOLD'
    elif reached and code == 0:
        status = 'OK'
    else:
        status = f'EXIT({code})'
    if (status != 'OK' or folds or fold_sites) and LOGDIR:
        dest = os.path.join(LOGDIR, f'scene{scene}.log')
        shutil.copyfile(log, dest)
        if reason:
            with open(dest, 'a') as fh:
                fh.write(f'\n[harness] stopped: {reason}; output may be incomplete\n')
    return dict(scene=scene, end=end, frames=n, crashes=crashes, first=first,
                status=status, entry=classify_entry(scene, end, scene_ran),
                folds=max(folds, fold_sites))


def classify_entry(scene, end, scene_ran):
    """Did the requested scene run?  A row that ends at the title is not a pass
    for any other step, however healthy the rest of it looks."""
    try:
        step = int(end.split('/')[0])
    except (AttributeError, ValueError):
        return '?'
    if step == scene:
        return 'entered'
    if step == TITLE_STEP and scene != TITLE_STEP:
        # Entered and left by its own logic (needs game state the title has not
        # set up), versus never dispatched at all -- different work items.
        return 'returned' if scene_ran else 'bounced'
    return 'advanced'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', type=int, default=300)
    ap.add_argument('--clock', choices=('game', 'vblank'), default='vblank',
                    help='frame budget clock (default vblank: a matrix about scene entry '
                         'wants hardware frames, because standalone loops such as TT (208) '
                         'never advance the game clock and ran to TRACE_LIMIT on it)')
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('--scenes', default='enterable',
                    help="'enterable' (default: the scene steps game_main dispatches), "
                         "'all' (0-210 smoke), or an ascending range like 0-210")
    ap.add_argument('--allow-bounced', action='store_true',
                    help='exit 0 even when a scene never reached its step (triage runs)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/scene_matrix.txt'))
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2,
                    help='scenes run in parallel (default: every core; each loads its own assets)')
    ap.add_argument('--bin', help='the binary to run (default pc/build/tokimeki-pc; '
                                  'e.g. a reach-instrumented copy)')
    ap.add_argument('--logdir', help='write the output of each non-OK or fault-folding scene here')
    a = ap.parse_args()
    if a.frames <= 0 or a.timeout <= 0 or a.jobs <= 0:
        ap.error('--frames, --timeout and --jobs must be positive')
    STOP.clear()
    global LOGDIR
    if a.logdir:
        os.makedirs(a.logdir, exist_ok=True)
        LOGDIR = a.logdir
    lo, hi = (a.scenes.split('-') + [a.scenes])[:2]
    if a.scenes == 'enterable':
        scenes = scene_steps()
    elif a.scenes == 'all':
        scenes = range(0, 211)
    else:
        scenes = range(int(lo), int(hi) + 1)
        if not 0 <= int(lo) <= int(hi) <= 210:
            ap.error('--scenes must be enterable, all, or a range within 0-210')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    # A developer may rebuild during a sweep. Every row must run the same
    # executable, rather than silently mixing results from multiple builds.
    global PC_BIN
    original = PC_BIN
    if a.bin:
        PC_BIN = os.path.abspath(a.bin)
    with tempfile.TemporaryDirectory(prefix='scene_matrix_bin_') as work:
        snapshot = os.path.join(work, os.path.basename(PC_BIN))
        shutil.copy2(PC_BIN, snapshot)
        digest = hashlib.sha256()
        with open(snapshot, 'rb') as fh:
            while chunk := fh.read(65536):
                digest.update(chunk)
        PC_BIN = snapshot
        try:
            return run_matrix(a, scenes, digest.hexdigest())
        finally:
            PC_BIN = original


def run_matrix(a, scenes, binary_sha256):
    rows = []
    metadata = dict(binary_sha256=binary_sha256, frames=a.frames, clock=a.clock,
                    timeout=a.timeout, jobs=a.jobs, complete=False, rows=rows)

    def checkpoint():
        path = os.path.abspath(a.out) + '.json'
        with open(path + '.tmp', 'w') as fh:
            json.dump(metadata, fh, indent=2)
        os.replace(path + '.tmp', path)

    checkpoint()
    with open(a.out, 'w') as fh:
        hdr = f'{"scene":>5s} {"end":>11s} {"in":>8s} {"frames":>6s} {"crashes":>7s} {"status":<9s} first crash'
        print(hdr, flush=True); fh.write(hdr + '\n'); fh.flush()
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            results = pool.map(lambda sc: run_scene(sc, a.frames, a.timeout, a.clock), scenes)
            try:
                for r in results:
                    rows.append(r)
                    # Fabricated / pre-built rows (the exit-code tests) may omit
                    # the entry column; keep the report tolerant of that.
                    r.setdefault('entry', '?')
                    line = f'{r["scene"]:5d} {r["end"]:>11s} {r["entry"]:>8s} {r["frames"]:6d} {r["crashes"]:7d} {r["status"]:<9s} {r["first"]}'
                    print(line, flush=True); fh.write(line + '\n'); fh.flush()
                    checkpoint()
            except BaseException:
                STOP.set()
                raise
        counts = {}
        for r in rows:
            counts[r['status'].split('(')[0]] = counts.get(r['status'].split('(')[0], 0) + 1
        entries = {}
        for r in rows:
            entries[r['entry']] = entries.get(r['entry'], 0) + 1
        firsts = {}
        for r in rows:
            if r['first']:
                firsts[r['first']] = firsts.get(r['first'], 0) + 1
        summary = ['', 'summary: ' + ', '.join(f'{k}={v}' for k, v in sorted(counts.items())),
                   'entry: ' + ', '.join(f'{k}={v}' for k, v in sorted(entries.items())),
                   'first failing functions: ' + ', '.join(f'{k} x{v}' for k, v in sorted(firsts.items(), key=lambda kv: -kv[1])[:12])]
        folded = [f"{r['scene']}:{r.get('folds', 0)}" for r in rows if r.get('folds')]
        summary.append('fault folds: %d in %d scene(s)%s' % (
            sum(r.get('folds', 0) for r in rows), len(folded),
            (' -- ' + ' '.join(folded)) if folded else ''))
        metadata['folds'] = sum(r.get('folds', 0) for r in rows)
        bounced = [str(r['scene']) for r in rows if r['entry'] == 'bounced']
        returned = [str(r['scene']) for r in rows if r['entry'] == 'returned']
        if bounced:
            summary.append('NEVER ENTERED (goto_step did not take): ' + ' '.join(bounced))
        if returned:
            summary.append('ENTERED THEN LEFT (scene needs game state the title lacks): '
                           + ' '.join(returned))
        for l in summary:
            print(l); fh.write(l + '\n')
        metadata['entries'] = entries
    metadata['complete'] = True
    checkpoint()
    ok_status = [row for row in rows if row['status'] == 'OK']
    # Only a "bounced" entry (the step really was enterable but never took) is
    # an entry-level failure. "?" is a parse miss or a fabricated test row and
    # is caught by its status instead; "advanced" is legitimate scene flow.
    unreached = [row for row in rows if row['entry'] == 'bounced']
    healthy = (rows and len(ok_status) == len(rows) and not unreached)
    if getattr(a, 'allow_bounced', False):
        healthy = bool(rows) and len(ok_status) == len(rows)
    return 0 if healthy else 1


def terminate(signum, frame):
    STOP.set()
    raise SystemExit(128 + signum)


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
