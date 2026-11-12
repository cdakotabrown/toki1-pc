#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Save-state round-trip test: save at game frame N, load it in a new
process, run to N+K, and require the machine to be exactly where an
uninterrupted run is (pc/hal/dbg_state.c).

For each playthrough script (pc/tools/playthrough/*.txt, first phase: power-on
with a new card) the script's state-anchored input is resolved once into
absolute game-frame presses, so every later run sees the same pad.  Then for
each point N (a milestone label plus an offset):

  reference  power-on, the absolute input, `save ref.tks` at N, `anchor`,
             `save ref_end.tks` at N+K, quit
  independent  a second power-on run saving at N: must equal ref.tks
             outside the KEEP ranges (a word that differs between two
             processes is a host pointer or non-determinism that a load
             would carry into another process)
  loaded     PC_LOADSTATE=ref.tks, `anchor`, `save load_end.tks` at N+K, quit
  hotload    power-on, at frame 200 `load ref.tks` into the RUNNING process,
             then as `loaded` (the hotkey/console path: the live process has
             its own host state, which the KEEP rules must preserve)

and requires: the two N+K states identical (PS1 RAM, MMIO, VRAM, SPU RAM and
the rest of the restored executable data -- savestate_tool.py diff), and the
two runs' parity artifacts identical: the PCM mixed since N (audio.s16le),
and the displayed image, VRAM and RAM at VBlank checkpoints after N.  All
runs mix audio on the game thread (PC_PARITY_DIR), the deterministic mode.
It also prints savestate_tool.py's value-based host-pointer audit of every
state (advisory: game data that happens to look like an address shows up
there too; the independent run is the check that decides).

    python3 pc/tools/savestate_roundtrip.py --out build/savestate-rt
        [--bin pc/build/tokimeki-pc] [--k 240] [--only new_game|date_save]
        [--points LABEL+OFF,...] [--no-hotload]

Exit 0 only when every point passes.  report.json holds the details.
"""
import argparse
import filecmp
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playthrough as pt  # noqa: E402
import savestate_tool as sst  # noqa: E402

ROOT = pt.ROOT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input  # noqa: E402
SCRIPTS = {
    'new_game': ('pc/tools/playthrough/new_game.txt',
                 ['sysfile_prompt+40', 'surname_entry+40', 'prologue+30', 'lesson+50']),
    'date_save': ('pc/tools/playthrough/date_save.txt',
                  ['calendar+20', 'date_start+60', 'date_choice+5', 'save_menu+10',
                   'save_confirm+70']),
}
REACHED = re.compile(r'\[TRACE\] PC_INPUT wait (\S+) reached: game frame (\d+)')


def is_windows(binary):
    """the Windows build (run under Wine)"""
    return str(binary).endswith('.exe')


def command(binary):
    """the command line that runs BINARY: the Windows build under Wine, with
    no X11/Wayland display, so Wine can never put a window or a dialog on
    the desktop (native runs use SDL's dummy drivers)"""
    if str(binary).endswith('.exe'):
        return ['env', '-u', 'DISPLAY', '-u', 'WAYLAND_DISPLAY', shutil.which('wine') or 'wine',
                str(binary)]
    return [str(binary)]


def run_port(binary, work, log, env_extra, timeout):
    os.makedirs(os.path.join(work, 'pc'), exist_ok=True)
    for name, target in (('pc/assets', os.path.join(ROOT, 'pc/assets')),
                         ('disks', str(disc_input('disks')))):
        p = os.path.join(work, name)
        if not os.path.lexists(p):
            os.symlink(target, p)
    shutil.rmtree(os.path.join(work, 'saves'), ignore_errors=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'GP0_', 'MDFN_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               TOKI_INPUT_DEVICE='pad', WINEDEBUG='-all')
    env.update(env_extra)
    with open(log, 'w') as fh:
        r = subprocess.run(command(binary) + [os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
                           stdout=fh, stderr=subprocess.STDOUT, timeout=timeout)
    with open(log, errors='replace') as fh:
        return r.returncode, fh.read()


def resolve(script_path, out, binary, timeout, env_extra=None):
    """Run the first phase with its anchored input (parity audio mode) ->
    (milestones {label: game frame}, absolute input text, end frame).

    The absolute input is the port's own record of every press the anchored
    run made (pc/hal/trace.c PC_INPUT_DUMP): the scripted presses where they
    landed and each `mash` repeat and branch case as it happened, in the
    script's clock.  Replayed, it reproduces that run exactly, so the
    milestones' frames hold for every later run that uses it.  ENV_EXTRA
    (e.g. TOKI_PROFILE=enhanced) is added to that run's environment; later
    runs must use the same."""
    script = pt.parse(script_path)
    phase = script['phases'][0]
    num = os.path.join(out, 'anchored.input')
    with open(num, 'w') as fh:
        fh.write(pt.numeric_phase(phase, script['hold'], False))
    work = os.path.join(out, 'work-resolve')
    syms = pt.trace_syms(script)
    dump = os.path.join(os.path.abspath(out), 'resolved.input')
    env = dict(PC_INPUT=num, PC_PARITY_DIR=os.path.join(os.path.abspath(out), 'resolve-parity'),
               PC_PARITY_FRAMES='1', PC_INPUT_DUMP=dump)
    if syms:
        env['PC_TRACE_SYMS'] = ','.join(f'{n}:{s}' for n, s in syms.items())
    env.update(env_extra or {})
    rc, text = run_port(binary, work, os.path.join(out, 'resolve.log'), env, timeout)
    reached = {m.group(1): int(m.group(2)) for m in REACHED.finditer(text)}
    want = [it['label'] for it in phase['items'] if it['kind'] in ('wait', 'branch')]
    missing = [l for l in want if l not in reached]
    if rc != 0 or missing or not os.path.isfile(dump):
        raise RuntimeError(f'{script_path}: anchored run rc={rc}, milestones not reached: {missing}')
    m = re.search(r'\[TRACE\] PC_INPUT end at game frame (\d+)', text)
    with open(dump) as fh:
        return reached, fh.read(), int(m.group(1)) if m else None


def machine_same(state):
    """every region equal; `stak` (the game stack's leftover bytes, which hold
    host values -- libc/SDL frames run on that stack -- and differ between
    any two processes) is reported but not required to match: the loaded
    process gets the saver's copy, so game reads of it agree.  `host_noise`
    (Windows only: the CD queue's uninitialised read payloads,
    savestate_tool.cdq_noise) is reported, not required to match either."""
    return not any(n for r, n in state.items() if r not in ('stak', 'host_noise'))


def compare_dirs(a, b, windows=False):
    """parity artifact directories -> list of differing/missing file names;
    WINDOWS: PS1 RAM dumps (.ram) are compared outside the CD queue's
    uninitialised read payloads (savestate_tool.cdq_noise)"""
    bad = []
    names = sorted(set(os.listdir(a)) | set(os.listdir(b))) if os.path.isdir(a) and os.path.isdir(b) else ['<missing dir>']
    for n in names:
        pa, pb = os.path.join(a, n), os.path.join(b, n)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            bad.append(n)
        elif windows and n.endswith('.ram'):
            with open(pa, 'rb') as fa, open(pb, 'rb') as fb:
                ra, rb = fa.read(), fb.read()
            noise = sst.cdq_noise(ra, rb)
            if sst.mask_ram(ra, noise) != sst.mask_ram(rb, noise):
                bad.append(n)
        elif not filecmp.cmp(pa, pb, shallow=False):
            bad.append(n)
    return bad


def run_point(name, label, offset, reached, abs_input, out, binary, k, timeout, hotload, syms):
    n = reached[label] + offset
    d = os.path.join(out, f'{name}-{label}+{offset}')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    inp = os.path.join(d, 'abs.input')
    with open(inp, 'w') as fh:
        fh.write(abs_input)
    frames = ','.join(str(v) for v in sorted({1, 30, max(31, k // 2), max(32, k - 10)}))
    res = dict(point=f'{label}+{offset}', frame=n, k=k)

    def env_for(tag):
        return dict(PC_INPUT=inp, PC_PARITY_DIR=os.path.join(d, f'{tag}-parity'),
                    PC_PARITY_FRAMES=frames, PC_PARITY_RAM='1',
                    PC_DBG_CMDS=os.path.join(d, f'{tag}.cmds'))

    ref_state, ref_end = os.path.join(d, 'ref.tks'), os.path.join(d, 'ref_end.tks')
    with open(os.path.join(d, 'ref.cmds'), 'w') as fh:
        fh.write(f'at {n} save {ref_state}\nanchor\nat {n + k} save {ref_end}\nafter 1 quit\n')
    rc, text = run_port(binary, os.path.join(d, 'work'), os.path.join(d, 'ref.log'),
                        env_for('ref'), timeout)
    if rc != 0 or 'deferred' in text or not os.path.exists(ref_end):
        res['error'] = f'reference run rc={rc}' + (' (save deferred)' if 'deferred' in text else '')
        return res
    info = sst.read_state(ref_state)['info']
    res['scene'] = f"{info['now_step']}/{info['now_sub_step']}/{info['now_sub_sub_step']}"
    res['overlay'] = info.get('overlay') or '-'
    ok = True
    # An independent power-on run to N: its state must equal ref.tks outside
    # the KEEP ranges.  A word that differs is either non-determinism or a
    # process-dependent value (a host pointer: ASLR moves the heap, stack and
    # libraries between processes) that a load would wrongly restore.
    ind = os.path.join(d, 'ind.tks')
    with open(os.path.join(d, 'ind.cmds'), 'w') as fh:
        fh.write(f'at {n} save {ind}\nafter 1 quit\n')
    env = env_for('ind')
    rc, text = run_port(binary, os.path.join(d, 'work'), os.path.join(d, 'ind.log'), env, timeout)
    v = dict(rc=rc)
    if 'deferred' in text:
        v['error'] = 'the save was deferred'
        ok = False
    elif rc != 0 or not os.path.exists(ind):
        v['error'] = f'run rc={rc}, no state'
        ok = False
    else:
        sa = sst.read_state(ref_state)
        rep = sst.diff_states(sa, sst.read_state(ind), syms)
        v['state'] = {r: sum(h - l for l, h in rs) for r, rs in rep.items()}
        v['state_diff'] = sst.diff_names(sa, rep, syms=syms)
        v['ok'] = machine_same(v['state'])
        ok = v['ok']
    res['independent'] = v
    variants = [('loaded', f'anchor\nat {n + k} save {{end}}\nafter 1 quit\n', {'PC_LOADSTATE': ref_state})]
    if hotload:
        variants.append(('hotload', f'at 200 load {ref_state}\nanchor\nat {n + k} save {{end}}\nafter 1 quit\n', {}))
    for tag, cmds, extra in variants:
        end = os.path.join(d, f'{tag}_end.tks')
        with open(os.path.join(d, f'{tag}.cmds'), 'w') as fh:
            fh.write(cmds.format(end=end))
        env = env_for(tag)
        env.update(extra)
        rc, text = run_port(binary, os.path.join(d, 'work'), os.path.join(d, f'{tag}.log'), env, timeout)
        v = dict(rc=rc)
        if 'deferred' in text:
            v['error'] = 'a save was deferred (an operation in flight): the states are not at the same frame'
            ok = False
        elif rc != 0 or not os.path.exists(end):
            v['error'] = f'run rc={rc}, no end state'
            ok = False
        else:
            sa = sst.read_state(ref_end)
            rep = sst.diff_states(sa, sst.read_state(end), syms)
            v['state'] = {r: sum(h - l for l, h in rs) for r, rs in rep.items()}
            v['state_diff'] = sst.diff_names(sa, rep, syms=syms)
            v['parity_diff'] = compare_dirs(os.path.join(d, 'ref-parity'), os.path.join(d, f'{tag}-parity'),
                                            binary.endswith('.exe'))
            v['ok'] = machine_same(v['state']) and not v['parity_diff']
            ok = ok and v['ok']
        res[tag] = v
    res['ok'] = ok
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--bin', default=pt.PC_BIN)
    ap.add_argument('--k', type=int, default=240, help='game frames run after the load')
    ap.add_argument('--only', choices=sorted(SCRIPTS))
    ap.add_argument('--points', help='LABEL+OFF,... (with --only)')
    ap.add_argument('--no-hotload', action='store_true')
    ap.add_argument('--timeout', type=int, default=900)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    binary = os.path.abspath(a.bin)
    syms = sst.Symbols(binary)
    report, failed = dict(binary=binary, build_id=syms.build_id, k=a.k, scripts={}), 0
    states = []
    for name, (path, points) in SCRIPTS.items():
        if a.only and name != a.only:
            continue
        if a.points:
            points = a.points.split(',')
        sd = os.path.join(out, name)
        os.makedirs(sd, exist_ok=True)
        print(f'[{name}] resolving {path}', flush=True)
        reached, abs_input, _ = resolve(os.path.join(ROOT, path), sd, binary, a.timeout)
        results = []
        for p in points:
            label, _, off = p.partition('+')
            r = run_point(name, label, int(off or 0), reached, abs_input, sd, binary, a.k,
                          a.timeout, not a.no_hotload, syms)
            results.append(r)
            states += [os.path.join(sd, f'{name}-{label}+{int(off or 0)}', f) for f in ('ref.tks', 'ref_end.tks')]
            status = 'PASS' if r.get('ok') else 'FAIL'
            failed += status == 'FAIL'
            detail = r.get('error') or ' '.join(
                f"{t}:{'ok' if r[t].get('ok') else r[t].get('error') or r[t].get('state_diff') or r[t].get('parity_diff')}"
                for t in ('independent', 'loaded', 'hotload') if t in r)
            print(f'[{name}] {status} {p:22s} frame {r["frame"]:6d} scene {r.get("scene", "?"):10s} '
                  f'overlay {r.get("overlay", "?"):9s} {detail}', flush=True)
        report['scripts'][name] = dict(milestones=reached, points=results)
    # host pointers outside the KEEP rules, in every reference state
    existing = [s for s in states if os.path.exists(s)]
    audit = subprocess.run([sys.executable, os.path.join(ROOT, 'pc/tools/savestate_tool.py'), 'audit',
                            '--bin', binary] + existing, capture_output=True, text=True)
    report['audit'] = audit.stdout
    print('[audit] ' + audit.stdout.strip().replace('\n', '\n[audit] '))
    with open(os.path.join(out, 'report.json'), 'w') as fh:
        json.dump(report, fh, indent=1)
    print(f'{"PASS" if not failed else "FAIL"}: {failed} failing point(s); {os.path.join(out, "report.json")}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
