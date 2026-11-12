#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure what the enhanced profile buys over parity (pc/hal/profile.h).

Every number is the port's own emulated clock, read from the per-VBlank
trace (pc/hal/trace.c PC_TRACE): a VBlank is 1/59.94 s of wall time when the
port runs paced (the normal game; headless runs here use PC_TURBO, which only
removes the host sleeps).  So "VBlanks" below is the wall time a player sees.

    python3 pc/tools/profile_bench.py boot              # power-on -> title menu
    python3 pc/tools/profile_bench.py scenes [--scenes 128,144,...] [--vblanks 900]
    python3 pc/tools/profile_bench.py playthrough [--script pc/tools/playthrough/new_game.txt]
    python3 pc/tools/profile_bench.py ff                # paced: fast-forward's wall time
    python3 pc/tools/profile_bench.py all --json build/profile_bench.json

boot         VBlanks from power-on (no input) to the title menu (now_step 18,
             sub-step 2): the boot movie, the logos, every load on the way.
scenes       per scene (SCENE_NAV, no input): game frames per VBlank over the
             second half of the run.  1.00 = every frame on time; below 1 =
             frames overrun their VBlank (slowdown).  Also the host's own time
             per game frame (PC_TURBO wall clock / frames), which must stay well
             under 16.7 ms for the enhanced profile's "no overrun" to hold in
             wall time too.
playthrough  playthrough.py on both profiles; the VBlanks between consecutive
             milestones (screen-to-screen times: loads, card writes, the
             scripted presses' fixed waits).
"""
import argparse
import json
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run   # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PC_BIN = os.path.join(ROOT, 'pc/build/tokimeki-pc')
VBLANK_HZ = 59.94
PROFILES = ('parity', 'enhanced')
# Scenes the matrix enters (SCENE_NAV, no input) whose frames overrun on
# parity: a date (32), TACO (128), the title (64), two 2D scenes.  96 and
# 115 run standalone loops / bounce to the title there and measure nothing.
DEFAULT_SCENES = (3, 32, 64, 128, 144)
FIELD = re.compile(r'(\w+)=([0-9a-f]+)')


def le32(hexs):
    return int.from_bytes(bytes.fromhex(hexs.ljust(8, '0')[:8]), 'little')


def read_trace(path):
    rows = []
    with open(path, errors='replace') as fh:
        for line in fh:
            if not line.startswith('frame='):
                continue
            f = dict(FIELD.findall(line))
            rows.append(dict(vblank=int(f['frame']), game=le32(f.get('frame_counter', '0')),
                             step=int(f.get('now_step', '0'), 16),
                             sub=int(f.get('now_sub_step', '0'), 16),
                             subsub=int(f.get('now_sub_sub_step', '0'), 16)))
    return rows


def run_port(profile, env_extra, timeout, binary):
    with tempfile.TemporaryDirectory(prefix='profile_bench_') as work:
        os.makedirs(os.path.join(work, 'pc'))
        os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
        trace = os.path.join(work, 'frame.trace')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_TRACE=trace, TOKI_PROFILE=profile, TOKI_CONFIG='/dev/null')
        env.update(env_extra)
        t0 = time.monotonic()
        r = run([binary, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
                timeout=timeout, max_output=8 * 1024 * 1024)
        wall = time.monotonic() - t0
        rows = read_trace(trace) if os.path.exists(trace) else []
        return r, rows, wall


def bench_boot(binary, timeout):
    out = {}
    # + skip_intro: the enhanced profile's START taps through the movie and
    # the title animation (pc/hal/enhance.c; forced on, as in a harness run
    # the player's [enhanced] skip_intro is never read)
    for name, p, extra in [(p, p, {}) for p in PROFILES] + \
            [('enhanced+skip_intro', 'enhanced', {'TOKI_SKIP_INTRO': '1'})]:
        r, rows, wall = run_port(p, dict(PC_TRACE_CLOCK='vblank', PC_TRACE_FRAMES='4000', **extra),
                                 timeout, binary)
        hit = next((x for x in rows if x['step'] == 18 and x['sub'] == 2), None)
        out[name] = dict(title_vblank=hit and hit['vblank'], title_game_frame=hit and hit['game'],
                         seconds=hit and round(hit['vblank'] / VBLANK_HZ, 2))
        print(f'[boot] {name:19s} title menu at VBlank {out[name]["title_vblank"]} '
              f'({out[name]["seconds"]} s), game frame {out[name]["title_game_frame"]}', flush=True)
    return out


def bench_scenes(binary, scenes, vblanks, timeout):
    out = {}
    for s in scenes:
        out[s] = {}
        for p in PROFILES:
            r, rows, wall = run_port(p, dict(SCENE_NAV=str(s), PC_TRACE_CLOCK='vblank',
                                             PC_TRACE_FRAMES=str(vblanks)), timeout, binary)
            tail = rows[len(rows) // 2:]
            if len(tail) < 2:
                out[s][p] = dict(error=r.reason or r.returncode)
                continue
            dv = tail[-1]['vblank'] - tail[0]['vblank']
            dg = tail[-1]['game'] - tail[0]['game']
            frames = rows[-1]['game'] or 1
            out[s][p] = dict(step=f"{tail[-1]['step']}/{tail[-1]['sub']}/{tail[-1]['subsub']}",
                             frames_per_vblank=round(dg / dv, 3) if dv else None,
                             host_ms_per_frame=round(1000.0 * wall / frames, 2))
            print(f'[scene {s:3d}] {p:9s} end {out[s][p]["step"]:9s} game frames/VBlank '
                  f'{out[s][p]["frames_per_vblank"]}  host {out[s][p]["host_ms_per_frame"]} ms/frame',
                  flush=True)
    return out


def bench_ff(binary, vblanks, timeout):
    """Wall time of a PACED enhanced run (no PC_TURBO): the boot, then
    SCENE_NAV=64 and `vblanks` VBlanks there, with fast-forward released and
    held (TOKI_FF_HOLD=1).  The emulated run is the same in both (same
    VBlank count); only the wall pacing differs, the boot movie included."""
    out = {}
    for label, extra in (('normal', {}), ('fast_forward', {'TOKI_FF_HOLD': '1'})):
        env = dict(SCENE_NAV='64', PC_TRACE_CLOCK='vblank', PC_TRACE_FRAMES=str(vblanks))
        env.update(extra)
        with tempfile.TemporaryDirectory(prefix='profile_bench_ff_') as work:
            os.makedirs(os.path.join(work, 'pc'))
            os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
            trace = os.path.join(work, 'frame.trace')
            e = {k: v for k, v in os.environ.items()
                 if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
            e.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TRACE=trace,
                     TOKI_PROFILE='enhanced', TOKI_CONFIG='/dev/null', **env)
            t0 = time.monotonic()
            r = run([binary, os.path.join(ROOT, 'pc/assets')], cwd=work, env=e,
                    timeout=timeout, max_output=8 * 1024 * 1024)
            wall = time.monotonic() - t0
            rows = read_trace(trace) if os.path.exists(trace) else []
        out[label] = dict(wall_seconds=round(wall, 2), vblanks=rows[-1]['vblank'] if rows else None,
                          ok=r.ok)
        print(f'[ff] {label:13s} {out[label]["vblanks"]} VBlanks in {out[label]["wall_seconds"]} s',
              flush=True)
    return out


def bench_playthrough(binary, script, timeout):
    out = {}
    with tempfile.TemporaryDirectory(prefix='profile_bench_pt_') as work:
        for p in PROFILES:
            d = os.path.join(work, p)
            r = run([sys.executable, os.path.join(ROOT, 'pc/tools/playthrough.py'), script,
                     '--out', d, '--bin', binary, '--profile', p],
                    cwd=ROOT, timeout=timeout, max_output=8 * 1024 * 1024)
            rep = os.path.join(d, 'report.json')
            if not os.path.exists(rep):
                out[p] = dict(error=r.reason or r.returncode)
                continue
            with open(rep) as fh:
                report = json.load(fh)
            out[p] = dict(ok=r.ok, failures=report['failures'], milestones=report['pc'])
            print(f'[playthrough] {p:9s} {"PASS" if r.ok else "FAIL"} '
                  f'{len(report["pc"])} milestones', flush=True)
    if all(p in out and 'milestones' in out[p] for p in PROFILES):
        a, b = out['parity']['milestones'], out['enhanced']['milestones']
        rows, prev = [], {}
        for label, m in sorted(a.items(), key=lambda kv: (kv[1]['phase'], kv[1]['vblank'])):
            if label not in b:
                continue
            ph = m['phase']
            pa, pb = prev.get(ph, (0, 0))
            rows.append(dict(label=label, phase=ph, parity=m['vblank'] - pa,
                             enhanced=b[label]['vblank'] - pb,
                             parity_total=m['vblank'], enhanced_total=b[label]['vblank']))
            prev[ph] = (m['vblank'], b[label]['vblank'])
        for x in rows:
            if x['parity'] or x['enhanced']:
                print(f'[segment] {x["phase"]:9s} -> {x["label"]:20s} parity {x["parity"]:5d}  '
                      f'enhanced {x["enhanced"]:5d} VBlanks')
        out['segments'] = rows
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=('boot', 'scenes', 'playthrough', 'ff', 'all'))
    ap.add_argument('--bin', default=PC_BIN)
    ap.add_argument('--scenes', default=','.join(map(str, DEFAULT_SCENES)))
    ap.add_argument('--vblanks', type=int, default=900)
    ap.add_argument('--script', default=os.path.join(ROOT, 'pc/tools/playthrough/new_game.txt'))
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--json', help='write the results here')
    a = ap.parse_args()
    binary = os.path.abspath(a.bin)
    res = {}
    if a.what in ('boot', 'all'):
        res['boot'] = bench_boot(binary, a.timeout)
    if a.what in ('scenes', 'all'):
        res['scenes'] = bench_scenes(binary, [int(s) for s in a.scenes.split(',') if s],
                                     a.vblanks, a.timeout)
    if a.what in ('ff', 'all'):
        res['ff'] = bench_ff(binary, a.vblanks * 2, a.timeout)
    if a.what in ('playthrough', 'all'):
        res['playthrough'] = bench_playthrough(binary, os.path.abspath(a.script), a.timeout * 3)
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        with open(a.json, 'w') as fh:
            json.dump(res, fh, indent=2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
