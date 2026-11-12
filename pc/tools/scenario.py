#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Jump the PC port to a game situation: scenario files (DEBUG-ONLY).

    python3 pc/tools/scenario.py pc/tools/scenarios/scene115_rensyu.txt
        [--out build/scenarios] [--bin pc/build/tokimeki-pc] [--window]
    python3 pc/tools/scenario.py --all          # every scenario; exit 0 if all reach their target

A scenario either
  (b) `start album`: builds a memory-card album with the wanted game state
      (pc/tools/tokimeki_album.py over a card the game itself wrote) and
      loads it through the game's own title -> load -> album 1 path, or
  (a) `start state PATH`: loads a save state (pc/hal/dbg_state.c),
then runs toolkit commands (pokes, goto) and checks that the target is
reached and stays.  It ends with a save state and a screenshot of the target
in --out/NAME/ (state.tks, target.ppm), so the next session can start from
`PC_LOADSTATE=.../state.tks` directly.  With --window the run is shown in a
window at normal speed and stays open (no quit) once the target is reached.

Format (pc/tools/scenarios/*.txt; `#` comments):

  start album | start state PATH | start state @NAME
                           (@NAME: the state scenario NAME ended with)
  album FIELD=VALUE ...    album record edits (tokimeki_album.FIELDS; also
                           date=YY/MM/DD (sets youbi, pass_days, season and
                           the month's calendar table), club=NAME|N,
                           girl.<name|n>.<tokimeki|yuukou|bomb|flags>=V,
                           reserved.<i>=U32, ev_flag.<i>=V, para_*=V)
  input LINE               after the load: a playthrough.py input line
                           (`+N BUTTONS[*H]`, `wait LABEL COND... within N`)
  do COMMAND               after the load: a toolkit command (PC_DBG_CMDS:
                           poke, goto, `when COND do ...`, `after N ...`)
  expect COND... [within N] [hold M]
                           success: every COND (NAME[:SIZE]=VALUE, as in
                           PC_INPUT waits) holds within N game frames of the
                           last `do`, and still holds M frames later

The template card and the calendar data come from one scripted new game
(pc/tools/playthrough/new_game.txt, first phase), generated once under
--out/template/ from YOUR assets; nothing derived from the game is tracked.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playthrough as pt  # noqa: E402
import tokimeki_album as alb  # noqa: E402

ROOT = pt.ROOT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input  # noqa: E402
SCEN_DIR = os.path.join(ROOT, 'pc/tools/scenarios')
LOAD_ALBUM_1 = """\
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
wait album_loaded now_step=0x34 now_sub_step=1 within 1800
"""


class ScenarioError(Exception):
    pass


def parse(path):
    sc = dict(name=os.path.splitext(os.path.basename(path))[0], start=None, state=None,
              album=[], input=[], do=[], expect=None)
    with open(path) as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            w = line.split()
            if w[0] == 'start':
                sc['start'] = w[1]
                if w[1] == 'state':
                    # @NAME: the state scenario NAME ended with (in --out)
                    sc['state'] = w[2] if w[2].startswith('@') or os.path.isabs(w[2]) \
                        else os.path.join(ROOT, w[2])
                elif w[1] != 'album':
                    raise ScenarioError(f'{path}:{n}: start album | start state PATH')
            elif w[0] == 'album':
                sc['album'] += w[1:]
            elif w[0] == 'input':
                sc['input'].append(line[len('input'):].strip())
            elif w[0] == 'do':
                sc['do'].append(line[len('do'):].strip())
            elif w[0] == 'expect':
                conds, within, hold, rest = [], 1200, 120, w[1:]
                while rest:
                    t = rest.pop(0)
                    if t == 'within':
                        within = int(rest.pop(0))
                    elif t == 'hold':
                        hold = int(rest.pop(0))
                    else:
                        conds.append(t)
                sc['expect'] = dict(conds=conds, within=within, hold=hold)
            else:
                raise ScenarioError(f'{path}:{n}: unknown directive {w[0]}')
    if sc['start'] is None or sc['expect'] is None:
        raise ScenarioError(f'{path}: needs `start` and `expect`')
    if sc['start'] != 'album' and (sc['album'] or sc['input']):
        raise ScenarioError(f'{path}: album/input lines need `start album`')
    return sc


def symbols(binary):
    """PS1 data symbols of the binary (name -> address)"""
    r = subprocess.run(['nm', binary], capture_output=True, text=True)
    out = {}
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'AaBbDd':
            out.setdefault(p[2], int(p[0], 16))
    return out


SETTINGS = {}    # --set NAME=VALUE: environment for the port (TOKI_LANG=en, PC_SWEEP_DIR=...)


def run_port(binary, work, log, env_extra, timeout, window=False, template=False):
    os.makedirs(os.path.join(work, 'pc'), exist_ok=True)
    for name, target in (('pc/assets', os.path.join(ROOT, 'pc/assets')),
                         ('disks', str(disc_input('disks')))):
        p = os.path.join(work, name)
        if not os.path.lexists(p):
            os.symlink(target, p)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
    if not window:
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1')
    env.update(TOKI_INPUT_DEVICE='pad')
    env.update(env_extra)
    if not template:
        env.update(SETTINGS)     # the scenario's run only: the template stays the retail new game
    with open(log, 'w') as fh:
        r = subprocess.run([binary, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
                           stdout=fh, stderr=subprocess.STDOUT, timeout=timeout)
    with open(log, errors='replace') as fh:
        return r.returncode, fh.read()


def ensure_template(out, binary, timeout):
    """--out/template/card.mcr (system file + album 1, written by the game in
    new_game.txt's first phase) and schedule.ram (PS1 RAM on the night
    schedule: the calendar pictures)."""
    d = os.path.join(out, 'template')
    card, ram = os.path.join(d, 'card.mcr'), os.path.join(d, 'schedule.ram')
    stamp = os.path.join(d, 'binary')
    if os.path.exists(card) and os.path.exists(ram):
        return card, ram
    os.makedirs(d, exist_ok=True)
    script = pt.parse(os.path.join(ROOT, 'pc/tools/playthrough/new_game.txt'))
    phase = script['phases'][0]
    inp = os.path.join(d, 'new_game.input')
    with open(inp, 'w') as fh:
        fh.write(pt.numeric_phase(phase, script['hold'], False))
    with open(os.path.join(d, 'template.cmds'), 'w') as fh:
        fh.write(f'when now_step=0x34 now_sub_step=1 do ram {ram}\n')
    work = os.path.join(d, 'work')
    shutil.rmtree(os.path.join(work, 'saves'), ignore_errors=True)
    print(f'[scenario] making the template card (new_game.txt, phase {phase["name"]})', flush=True)
    rc, text = run_port(binary, work, os.path.join(d, 'template.log'),
                        dict(PC_INPUT=inp, PC_DBG_CMDS=os.path.join(d, 'template.cmds')), timeout,
                        template=True)
    src = os.path.join(work, 'saves/card1.mcr')
    if rc != 0 or not os.path.exists(src) or not os.path.exists(ram):
        raise ScenarioError(f'template run failed (rc {rc}); see {d}/template.log')
    shutil.copy(src, card)
    with open(stamp, 'w') as fh:
        fh.write(binary + '\n')
    return card, ram


def build_card(sc, template_card, template_ram, syms, dest):
    with open(template_card, 'rb') as fh:
        card = bytearray(fh.read())
    rec = alb.read_record(card, 1)
    if not alb.album_listed(card, 1):
        raise ScenarioError('the template system file does not list album 1')
    date = None
    for kv in sc['album']:
        k, _, v = kv.partition('=')
        if k == 'date':
            date = tuple(int(x) for x in v.split('/'))
        else:
            alb.set_field(rec, k, v)
    if date:
        keys = {kv.partition('=')[0] for kv in sc['album']}
        with open(template_ram, 'rb') as fh:
            ram = fh.read()
        try:
            alb.apply_date(rec, *date, ram=ram, symbols=syms, keep=keys)
        except alb.AlbumError as e:
            raise ScenarioError(f'{e} in {template_ram}')
    alb.write_record(card, 1, rec)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'wb') as fh:
        fh.write(card)


def run(sc, out, binary, timeout, window):
    d = os.path.join(out, sc['name'])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    work = os.path.join(d, 'work')
    os.makedirs(work)
    syms = symbols(binary)
    env = {}
    cmds = []
    if sc['start'] == 'album':
        card, ram = ensure_template(out, binary, timeout)
        build_card(sc, card, ram, syms, os.path.join(work, 'saves/card1.mcr'))
        load = LOAD_ALBUM_1
        if sc['do'] and sc['input']:
            # the `do` commands run the moment the album is loaded (the
            # `when` below), so a `goto` leaves the planner before the
            # input's VBlank check can see it: the album_loaded wait would
            # never be reached and the scenario's own input never run.  The
            # scenario's first input wait anchors the rest instead.
            load = '\n'.join(l for l in LOAD_ALBUM_1.splitlines()
                             if not l.startswith('wait album_loaded')) + '\n'
        text = 'hold 2\nphase scenario\n' + load + '\n'.join(sc['input']) + '\n'
        with open(os.path.join(d, 'scenario.input.txt'), 'w') as fh:
            fh.write(text)
        script = pt.parse(os.path.join(d, 'scenario.input.txt'))
        inp = os.path.join(d, 'scenario.input')
        with open(inp, 'w') as fh:
            fh.write(pt.numeric_phase(script['phases'][0], script['hold'], False))
        env['PC_INPUT'] = inp
        cmds.append('when now_step=0x34 now_sub_step=1 do echo album_loaded')
    else:
        if sc['state'].startswith('@'):
            sc['state'] = os.path.join(out, sc['state'][1:], 'state.tks')
        if not os.path.exists(sc['state']):
            raise ScenarioError(f'no state {sc["state"]} (make it first, e.g. with the scenario that saves it)')
        env['PC_LOADSTATE'] = sc['state']
    cmds += sc['do']
    cmds.append('echo scenario_go')
    ex = sc['expect']
    cmds.append('when ' + ' '.join(ex['conds']) + ' do echo target_reached')
    cmds.append(f'after {ex["hold"]} echo target_held')
    cmds.append(f'save {os.path.join(d, "state.tks")}')
    cmds.append(f'shot {os.path.join(d, "target.ppm")}')
    if not window:
        cmds.append('quit')
    with open(os.path.join(d, 'scenario.cmds'), 'w') as fh:
        fh.write('\n'.join(cmds) + '\n')
    syms_traced = sorted({re.split(r'[:!=]', c)[0] for c in ex['conds']} - set(pt.FIELD_SIZES))
    env.update(PC_DBG_CMDS=os.path.join(d, 'scenario.cmds'), PC_TRACE=os.path.join(d, 'trace.txt'),
               PC_TRACE_PHASE='pre_irq', PC_TRACE_FRAMES='40000')
    if syms_traced:
        env['PC_TRACE_SYMS'] = ','.join(syms_traced)
    print(f'[scenario] {sc["name"]}: running', flush=True)
    rc, text = run_port(binary, work, os.path.join(d, 'run.log'), env, timeout, window)
    go = re.search(r'\[DBG\] cmds line \d+ at frame (\d+): echo scenario_go', text)
    hit = re.search(r'\[DBG\] cmds line \d+ at frame (\d+): echo target_reached', text)
    held = re.search(r'\[DBG\] cmds line \d+ at frame (\d+): echo target_held', text)
    res = dict(name=sc['name'], rc=rc)
    if not go:
        res['error'] = 'the load did not finish (see run.log)'
        return res
    res['go'] = int(go.group(1))
    if not hit or int(hit.group(1)) - res['go'] > ex['within']:
        res['error'] = 'target not reached' + (f' within {ex["within"]} frames' if hit else '')
        return res
    res['reached'] = int(hit.group(1))
    # it must still hold at the end of the hold window (the trace shows the
    # whole window; a bounce through the title would show there)
    conds = [pt.parse_cond(c) for c in ex['conds']]
    rows = [r for r in pt.tc.parse_trace(os.path.join(d, 'trace.txt'))
            if res['reached'] <= r.get('frame_counter', -1) <= res['reached'] + ex['hold']]
    bad = [r['frame_counter'] for r in rows if not pt.holds(r, conds)]
    res['held'] = bool(held) and not bad
    if not res['held']:
        res['error'] = f'target left during the hold window (game frames {bad[:5]})'
    res['state'] = os.path.join(d, 'state.tks') if os.path.exists(os.path.join(d, 'state.tks')) else None
    res['shot'] = os.path.join(d, 'target.ppm') if os.path.exists(os.path.join(d, 'target.ppm')) else None
    m = re.findall(r'\[DBG\] ok saved \S+: (.*)', text)
    res['at'] = m[-1] if m else None
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scenarios', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/scenarios'))
    ap.add_argument('--bin', default=pt.PC_BIN)
    ap.add_argument('--window', action='store_true', help='show the run; stay at the target')
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                    help='an environment setting for the port run (TOKI_LANG=en, PC_SWEEP_DIR=...)')
    a = ap.parse_args()
    SETTINGS.update(dict(kv.split('=', 1) for kv in a.set))
    paths = a.scenarios or []
    if a.all:
        paths = sorted(glob.glob(os.path.join(SCEN_DIR, '*.txt')))
    if not paths:
        ap.error('no scenario given')
    out, binary = os.path.abspath(a.out), os.path.abspath(a.bin)
    failed = 0
    # scenarios that start from a state made by another one run last
    scs = [parse(p) for p in paths]
    scs.sort(key=lambda s: s['start'] == 'state')
    for sc in scs:
        try:
            r = run(sc, out, binary, a.timeout, a.window)
        except ScenarioError as e:
            r = dict(name=sc['name'], error=str(e))
        ok = 'error' not in r
        failed += not ok
        print(f'[scenario] {"OK  " if ok else "FAIL"} {sc["name"]:24s} '
              + (f'reached at game frame {r["reached"]} (load done {r["go"]}); {r.get("at")}; '
                 f'state {os.path.relpath(r["state"], ROOT) if r.get("state") else "-"}' if ok else r['error']),
              flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
