#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""windows_parity.py -- run each player feature on the Linux build and on the
Windows build (under Wine, a private prefix per run) and require the
recorded artifacts to be identical bit for bit.

Every case is one headless boot with a feature switched on.  Each run
records what the harness can observe:

  trace    PC_TRACE (pre_irq): the game's own state every frame
  frames   PC_FRAME_HASH: VRAM, scanout and (with a raised internal
           resolution) the high-resolution shadow, hashed every frame
  present  PC_PRESENT_HASH: the window the presenter shows (CRT filter,
           UI font, menus, toasts), hashed every frame
  audio    PC_PARITY_DIR audio.s16le: the game-thread SPU mix, sample for
           sample (boot-anchored or from the title, see the case)

The port is deterministic run to run, so any difference is a portability
bug.

    python3 pc/tools/windows_parity.py [--out build/windows-parity] [--jobs 12]
        [--only CASE,...] [--linux pc/build/tokimeki-pc]
        [--exe pc/build-win/tokimeki-pc.exe] [--json OUT]
        [--negative-control]   the Windows runs get input of their own
                               (START, CROSS, a click at fixed ticks): every
                               case's trace must then DIFFER, and each
                               artifact must differ in some case, or the
                               comparison is blind
Exit 0 when every case matches, 1 otherwise, 2 when a binary is missing.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402
import game_platform  # noqa: E402

ROOT = str(game_platform.ROOT)
ASSETS = os.path.join(ROOT, 'pc/assets')

# name -> (frames, env, artifacts, what it covers)
ALL = ('trace', 'frames', 'present', 'audio')
CASES = {
    'boot': (1500, {}, ALL,
             'parity boot: BIOS, boot movie (MDEC + XA audio), title; SPU/SEQ mix'),
    'title-audio': (400, {'SCENE_NAV': '18', 'PC_PARITY_ANCHOR': ''}, ALL,
                    'title music from the title anchor (SEQ/VAB through the SPU core)'),
    'enhanced': (1500, {'TOKI_PROFILE': 'enhanced'}, ALL, 'the Enhanced profile defaults'),
    'hires2': (900, {'TOKI_PROFILE': 'enhanced', 'TOKI_RES_SCALE': '2'}, ALL,
               'internal resolution 2x (gpu_hires shadow)'),
    'hires4': (900, {'TOKI_PROFILE': 'enhanced', 'TOKI_RES_SCALE': '4'}, ALL,
               'internal resolution 4x'),
    'pgxp': (900, {'TOKI_PROFILE': 'enhanced', 'TOKI_RES_SCALE': '3', 'TOKI_SUBPIXEL': '1'}, ALL,
             'sub-pixel 3D (gpu_pgxp) at 3x'),
    'crt': (900, {'TOKI_PROFILE': 'enhanced', 'TOKI_CRT': '1'}, ALL, 'CRT filter (presenter)'),
    'skip-intro': (900, {'TOKI_PROFILE': 'enhanced', 'TOKI_SKIP_INTRO': '1'}, ALL,
                   'skip intro assist'),
    'english': (1500, {'TOKI_LANG': 'en'}, ALL,
                'English translation pack (embedded), UI font, Latin text'),
    'english-enhanced': (1500, {'TOKI_LANG': 'en', 'TOKI_PROFILE': 'enhanced'}, ALL,
                         'English with the Enhanced profile'),
    'mouse': (700, {'TOKI_PROFILE': 'enhanced', 'TOKI_INPUT_DEVICE': 'mouse',
                    '@script': '200 pointer 100 80\n230 lbutton 1\n236 lbutton 0\n'
                               '300 mouse 40 20\n330 pad start\n340 pad none\n'
                               '400 pointer 200 160\n420 lbutton 1\n426 lbutton 0\n'
                               '440 rbutton 1\n446 rbutton 0\n500 key Return\n'},
              ALL, 'mouse input (injected SDL events) and keyboard'),
    'menu': (500, {'@script': '150 key Ctrl+O Down Down Down Right Right Escape\n'}, ALL,
             'settings menu drawn with the UI font, a live change'),
}


def sha(path):
    if not os.path.exists(path):
        return None
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()[:16]


def boot(platform, binary, case, out, negative):
    frames, extra, artifacts, _ = CASES[case]
    work = os.path.join(out, case, platform)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, 'pc'))
    os.symlink(ASSETS, os.path.join(work, 'pc/assets'))
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'CPU_COST', 'GP0_'))}
    files = {'trace': os.path.join(work, 'trace'), 'frames': os.path.join(work, 'frames'),
             'present': os.path.join(work, 'present'),
             'audio': os.path.join(work, 'parity/audio.s16le')}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               PC_TRACE=files['trace'], PC_TRACE_PHASE='pre_irq', PC_TRACE_FRAMES=str(frames),
               PC_FRAME_HASH=files['frames'], PC_PRESENT_HASH=files['present'],
               TOKI_CONFIG_DIR=os.path.join(work, 'cfg'),
               TOKI_INPUT_DEVICE='pad', PC_PARITY_DIR=os.path.join(work, 'parity'),
               PC_PARITY_FRAMES='1800', PC_PARITY_ANCHOR='boot')
    script = extra.get('@script', '')
    if script:
        script += f'{frames - 5} quit\n'
    if negative and platform == 'windows':
        # the control: input on Windows only -- START, CROSS and a mouse
        # click at fixed game ticks (the movie is skipped, menus open):
        # every artifact must differ
        script = ('100 pad start\n106 pad none\n300 pad cross\n306 pad none\n'
                  '320 lbutton 1\n326 lbutton 0\n500 pad start\n506 pad none\n') + script
    if script:
        path = os.path.join(work, 'input.txt')
        with open(path, 'w') as f:
            f.write(''.join(sorted(script.splitlines(True), key=lambda l: int(l.split()[0]))))
        env['TOKI_INPUT_SCRIPT'] = path
    for k, v in extra.items():
        if k == '@script':
            continue
        if v == '':
            env.pop(k, None)
        else:
            env[k] = v
    log = os.path.join(work, 'log')
    r = run([binary, ASSETS], cwd=work, env=env, timeout=1800, log_path=log,
            max_output=512 * 1024 * 1024)
    text = open(log, errors='replace').read()
    problems = []
    if r.reason or r.returncode != 0:
        problems.append(f'exit {r.reason or r.returncode}')
    if '[CRASH]' in text:
        problems.append('[CRASH]')
    if '[FOLD] total=0 ' not in text:
        problems.append('folds')
    return {a: sha(files[a]) for a in artifacts}, problems, log


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/windows-parity'))
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--only')
    ap.add_argument('--linux', default=str(game_platform.LINUX_GAME))
    ap.add_argument('--exe', default=str(game_platform.WINDOWS_GAME))
    ap.add_argument('--json')
    ap.add_argument('--negative-control', action='store_true')
    a = ap.parse_args(argv)
    if not (os.path.isfile(a.linux) and os.path.isfile(a.exe) and game_platform.wine_available()):
        print('windows_parity: needs both builds and Wine')
        return 2
    cases = a.only.split(',') if a.only else list(CASES)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    jobs = {}
    with cf.ThreadPoolExecutor(a.jobs) as pool:
        for c in cases:
            for plat, binary in (('linux', a.linux), ('windows', a.exe)):
                jobs[c, plat] = pool.submit(boot, plat, binary, c, out, a.negative_control)
    rows, bad = [], 0
    for c in cases:
        lh, lp, llog = jobs[c, 'linux'].result()
        wh, wp, wlog = jobs[c, 'windows'].result()
        same = {k: lh[k] is not None and lh[k] == wh[k] for k in lh}
        ok = not lp and not wp and all(same.values())
        if a.negative_control:
            ok = not lp and not wp and not same['trace'] and not all(same.values())
        bad += not ok
        rows.append(dict(case=c, covers=CASES[c][3], linux=lp or 'ok', windows=wp or 'ok',
                         same=same, linux_hashes=lh, windows_hashes=wh, ok=ok))
        cells = ' '.join(f'{k}={"=" if v else "DIFF"}{"" if lh[k] else "(none)"}'
                         for k, v in same.items())
        print(f'{"PASS" if ok else "FAIL"} {c:17s} linux={lp or "ok"} windows={wp or "ok"} {cells}',
              flush=True)
    if a.negative_control:
        # and each artifact must be seen to differ somewhere: no blind column
        for art in ALL:
            if all(r['same'].get(art, True) for r in rows):
                print(f'negative control: {art} never differed -- that comparison is blind')
                bad += 1
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(rows, f, indent=1)
    print(f'{len(rows) - bad}/{len(rows)} cases '
          f'{"differ as required (negative control)" if a.negative_control else "identical"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
