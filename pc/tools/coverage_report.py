#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reach coverage of the PC port: which game functions a test suite runs.

Three subcommands:

  build-reach [--out BIN]
      Link a copy of the port in which every game object (sources under
      src/main/ and the generated overlay modules pc/src/overlay_*.c) is
      compiled with -finstrument-functions.  pc/hal and the rest of pc/src
      stay plain (instrumenting the HAL stalls the boot movie).  Running it
      with REACH_LIST_FILE=<path> makes pc/hal/instrument.c append the name
      of every function entered to <path> at exit; `%p` in the path is the
      process id.  pc/build is not touched.

  run [--bin BIN] [--out DIR] [--frames 300] [--scripts ...] [--no-matrix]
      Run the scene matrix (scene_matrix.py, every enterable step for
      --frames frames) and the playthrough scripts (playthrough.py,
      default new_game, date_save, spring_events) on the reach binary, each
      with its own REACH_LIST_FILE under DIR/lists/.  A failing job is
      recorded in DIR/run.json and the others go on: a playthrough that
      stops mid-run still reports what it reached.  A process killed on a
      timeout writes no list (the flush runs at exit).

  report INPUT... [--bin BIN] [--json OUT] [--md OUT]
      INPUT is a reach list, a directory of them (recursively: *.txt and
      files named reach*), or an earlier report JSON; the reached sets are
      UNIONED, so reports merge across any number of runs.  Hex addresses
      in a list (functions dladdr could not name) are resolved with
      `nm BIN`.

The universe (denominator):

  overlay <OV>   one function per src/overlay/<OV>/*.c (the same set as
                 config/overlay/<OV>.sym); on the port it is <OV>_<name> in
                 pc/src/overlay_<OV>.c.  EVENT and GYOZI are dead overlays
                 (the retail loader never loads them) and are listed apart.
  main game      config/level3_functions.json (every registered main-binary
                 C function) plus the asm_only entries of
                 config/main_function_audit_by_addr.json, below
                 SDK_START.
  main sdk       the same at or above SDK_START = 0x800869A0 (DecDCTReset,
                 the first Sony library member: libpress, libcd, libsnd,
                 libspu, libgs, libgpu, libgte, libapi, BIOS thunks).  The
                 Konami code linked between 0x80086000 and there
                 (date_x_taku_set .. load_done_flag_set) is game code.

A universe function with no definition in the binary (asm_only Sony code
the HAL replaces, LIBGTE copies in overlays) is `not_in_build`: it cannot be
reached on the port and is not in the coverage denominator.  A reached name
outside the universe (PC-only helpers, pc-port glue, compiler clones of
non-game code) is listed under `unattributed`.  GCC clone suffixes
(`.part.0`, `.isra.0`, `.cold`, `.constprop.0`) map to their function.
"""
import argparse
import datetime
import glob
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP, MONOREPO_DECOMP, decomp_key  # noqa: E402
TOOLS = os.path.join(ROOT, 'pc', 'tools')
DEFAULT_BIN = os.path.join(ROOT, 'build', 'coverage', 'tokimeki-reach')
DEAD_OVERLAYS = ('EVENT', 'GYOZI')
SDK_START = 0x800869A0
PLAYTHROUGHS = ('new_game', 'date_save', 'spring_events')
CLONE_RE = re.compile(r'\.(part|isra|cold|constprop|lto_priv|localalias)(\.\d+)?.*$')
HEX_RE = re.compile(r'^(0x)?[0-9a-fA-F]+$')


# ----------------------------------------------------------------- universe

def load_universe(root=None):
    """{pc_name: {'group', 'name', 'addr'}} for every game function.

    group is 'overlay:<OV>', 'dead:<OV>', 'main:game' or 'main:sdk'.
    root: the tree holding src/overlay and config/ (default toki1-decomp)."""
    if root is None:
        root = ROOT if MONOREPO_DECOMP else str(DECOMP)
    uni = {}
    odir = os.path.join(root, 'src', 'overlay')
    for ov in sorted(os.listdir(odir)) if os.path.isdir(odir) else ():
        if not os.path.isdir(os.path.join(odir, ov)):
            continue
        addrs = {}
        sym = os.path.join(root, 'config', 'overlay', ov + '.sym')
        if os.path.exists(sym):
            for line in open(sym):
                parts = line.strip().split(':')
                if len(parts) == 3 and parts[0].startswith('0x'):
                    addrs.setdefault(parts[2], parts[0])
        group = ('dead:' if ov in DEAD_OVERLAYS else 'overlay:') + ov
        for f in sorted(glob.glob(os.path.join(odir, ov, '*.c'))):
            name = os.path.basename(f)[:-2]
            uni['%s_%s' % (ov, name)] = dict(group=group, name=name,
                                             addr=addrs.get(name))
    main, aliases = {}, {}
    l3 = os.path.join(root, 'config', 'level3_functions.json')
    if os.path.exists(l3):
        for e in json.load(open(l3))['functions']:
            main.setdefault(e['name'], int(e['address'], 16))
    audit = os.path.join(root, 'config', 'main_function_audit_by_addr.json')
    if os.path.exists(audit):
        for a, e in json.load(open(audit))['addresses'].items():
            if e.get('kind') != 'asm_only':
                continue
            al = e.get('aliases') or ['func_' + a[2:].upper()]
            if any(n in main for n in al):
                continue
            named = [n for n in al if not n.lower().startswith('func_')]
            name = (named or al)[0]
            main.setdefault(name, int(a, 16))
            # an asm_only function has no C, so the port may define it
            # under any of its names (GsTMDfastTG3LFG for func_800AxXXX)
            aliases[name] = sorted(set(al) - {name})
    for name, a in main.items():
        uni.setdefault(name, dict(group='main:sdk' if a >= SDK_START else 'main:game',
                                  name=name, addr='0x%08X' % a))
    taken = set(uni)
    for name, al in aliases.items():
        al = [n for n in al if n not in taken]
        if al:
            uni[name]['aliases'] = al
    return uni


def binary_functions(path):
    """({name: addr}, {addr: name}) of the text symbols of an ELF."""
    out = subprocess.run(['nm', '--defined-only', path], capture_output=True,
                         text=True, check=True).stdout
    return parse_nm(out)


def parse_nm(text):
    names, addrs = {}, {}
    for line in text.splitlines():
        p = line.split()
        if len(p) != 3 or p[1] not in 'TtWw':
            continue
        a = int(p[0], 16)
        names[p[2]] = a
        # prefer a global name for an address
        if a not in addrs or (p[1] in 'TW' and not addrs[a][1]):
            addrs[a] = (p[2], p[1] in 'TW')
    return names, {a: n for a, (n, _) in addrs.items()}


def base_name(n):
    return CLONE_RE.sub('', n)


# ------------------------------------------------------------------- inputs

def read_inputs(paths, addr_names=None):
    """Union of the reached names in reach lists / dirs / report JSONs.
    Returns (names, unresolved_hex, sources)."""
    names, unresolved, sources = set(), set(), []
    files = []
    for p in paths:
        if os.path.isdir(p):
            for dp, _, fs in os.walk(p):
                for f in sorted(fs):
                    if f.endswith('.txt') or f.startswith('reach'):
                        files.append(os.path.join(dp, f))
        else:
            files.append(p)
    for f in files:
        if f.endswith('.json'):
            d = json.load(open(f))
            names |= set(d.get('reached_names', ()))
            unresolved |= set(d.get('unresolved', ()))
            sources += d.get('inputs', [f])
            continue
        sources.append(os.path.relpath(f, ROOT) if f.startswith(ROOT) else f)
        for line in open(f, errors='replace'):
            n = line.strip()
            if not n:
                continue
            if HEX_RE.match(n):
                a = int(n, 16)
                if addr_names and a in addr_names:
                    names.add(addr_names[a])
                else:
                    unresolved.add(n)
            else:
                names.add(n)
    return names, unresolved, sources


# ------------------------------------------------------------------- report

def build_report(universe, reached, in_build=None, unresolved=(), inputs=()):
    """The report dict.  `in_build`: the set of function names the binary
    defines (None: assume every universe function is built)."""
    built_base = None
    if in_build is not None:
        built_base = {base_name(n) for n in in_build}
    reached_base = {base_name(n) for n in reached}
    groups = {}
    for pc, f in sorted(universe.items()):
        g = groups.setdefault(f['group'], dict(total=0, in_build=0, reached=0,
                                               unreached=[], not_in_build=[],
                                               reached_list=[]))
        g['total'] += 1
        rec = dict(name=f['name'], addr=f['addr'])
        names = {pc} | set(f.get('aliases', ()))
        if built_base is not None and not names & built_base:
            g['not_in_build'].append(rec)
            continue
        g['in_build'] += 1
        if names & reached_base:
            g['reached'] += 1
            g['reached_list'].append(f['name'])
        else:
            g['unreached'].append(rec)
    for g in groups.values():
        g['percent'] = round(100.0 * g['reached'] / g['in_build'], 1) if g['in_build'] else 0.0
        g['unreached'].sort(key=lambda r: (r['addr'] or '', r['name']))

    def total(pred):
        t = dict(total=0, in_build=0, reached=0)
        for k, g in groups.items():
            if pred(k):
                for f in t:
                    t[f] += g[f]
        t['percent'] = round(100.0 * t['reached'] / t['in_build'], 1) if t['in_build'] else 0.0
        return t

    summary = dict(
        live_overlays=total(lambda k: k.startswith('overlay:')),
        main_game=total(lambda k: k == 'main:game'),
        live_total=total(lambda k: k.startswith('overlay:') or k == 'main:game'),
        main_sdk=total(lambda k: k == 'main:sdk'),
        dead_overlays=total(lambda k: k.startswith('dead:')))
    known = set(universe)
    for f in universe.values():
        known.update(f.get('aliases', ()))
    unattributed = sorted({base_name(n) for n in reached} - known)
    return dict(version=1,
                generated=datetime.datetime.now().isoformat(timespec='seconds'),
                inputs=sorted(set(inputs)), summary=summary, groups=groups,
                unattributed=unattributed, unresolved=sorted(unresolved),
                reached_names=sorted(reached))


def markdown(rep, max_list=60):
    s = rep['summary']
    out = ['# PC port reach coverage', '',
           'Generated %s by `pc/tools/coverage_report.py` from %d input(s).'
           % (rep['generated'], len(rep['inputs'])), '',
           '| scope | reached | in build | universe | % |', '|---|---:|---:|---:|---:|']
    for k, label in (('live_total', '**live game code**'), ('live_overlays', 'live overlays'),
                     ('main_game', 'main game code'), ('main_sdk', 'Sony SDK (excluded)'),
                     ('dead_overlays', 'dead overlays EVENT/GYOZI (excluded)')):
        t = s[k]
        out.append('| %s | %d | %d | %d | %.1f |' % (label, t['reached'], t['in_build'],
                                                     t['total'], t['percent']))
    out += ['', '## Per group', '',
            '| group | reached | in build | universe | % | unreached |',
            '|---|---:|---:|---:|---:|---:|']
    order = sorted(rep['groups'], key=lambda k: (k.split(':')[0] != 'main',
                                                 k.startswith('dead:'), k))
    for k in order:
        g = rep['groups'][k]
        out.append('| %s | %d | %d | %d | %.1f | %d |' % (
            k, g['reached'], g['in_build'], g['total'], g['percent'], len(g['unreached'])))
    out += ['', '## Never reached', '',
            'Full lists (with PS1 addresses) are in the JSON report.', '']
    for k in order:
        g = rep['groups'][k]
        if not g['unreached'] or k == 'main:sdk' or k.startswith('dead:'):
            continue
        names = [r['name'] for r in g['unreached']]
        more = '' if len(names) <= max_list else ' ... (+%d more)' % (len(names) - max_list)
        out += ['<details><summary>%s: %d unreached</summary>' % (k, len(names)), '',
                ', '.join('`%s`' % n for n in names[:max_list]) + more, '', '</details>', '']
    if rep['unattributed']:
        out += ['%d reached names are outside the universe (PC-only code); see '
                '`unattributed` in the JSON.' % len(rep['unattributed']), '']
    if rep['unresolved']:
        out += ['%d reached addresses could not be named (pass `--bin`).'
                % len(rep['unresolved']), '']
    return '\n'.join(out) + '\n'


# -------------------------------------------------------------- build-reach

def game_objects(build_dir):
    """Object basenames whose source is game code."""
    objs = set()
    for line in open(os.path.join(ROOT, build_dir, 'obj', '.srcmap')):
        p = line.split()
        p = p[:1] + [decomp_key(x) for x in p[1:]]
        if len(p) == 2 and (p[1].startswith('src/main/') or
                            re.match(r'pc/src/overlay_[A-Z_0-9]+\.c$', p[1])):
            objs.add(p[0])
    return objs


def object_flags(cmd):
    """the instrumentation flags of one game object: audit_ps1_wide_access's
    reach_flags, never a naked function.  Until 2026-11-01 this builder used
    a bare -finstrument-functions: GCC then puts `lea this_fn,%rdi; call
    __cyg_profile_func_enter` in front of the naked BIOS trampolines
    (`func_800B4880() { jmp strlen }`), so strlen measured its own code in
    the reach binary.  NAME_ENT name_ent_caption_disp then strcpy'd a
    906-byte caption into a 35-byte buffer and the stack protector aborted
    (explorer, card-end_04_saki-s2); every reach run's path could part from
    the plain binary's at the first game strlen."""
    sys.path.insert(0, TOOLS)
    from audit_ps1_wide_access import reach_flags
    return reach_flags(cmd)


def build_reach(build_dir, out, jobs=8):
    sys.path.insert(0, TOOLS)
    from audit_ps1_pointer_casts import compile_commands
    from audit_ps1_wide_access import debug_object
    import shlex
    cmds = compile_commands(build_dir)
    objs = game_objects(build_dir)
    missing = sorted(objs - set(cmds))
    if missing:
        sys.exit('no compile command for %d objects, e.g. %s' % (len(missing), missing[:3]))
    target = os.path.join(build_dir, 'tokimeki-pc')
    r = subprocess.run(['make', '-s', '-n', '-B', '-f', 'pc/Makefile',
                        'BUILD_DIR=%s' % build_dir, target],
                       cwd=ROOT, capture_output=True, text=True, timeout=600)
    link = [l for l in r.stdout.splitlines() if ' -o %s' % target in l and ' -c ' not in l]
    if not link:
        sys.exit('no link command for %s' % target)
    argv = shlex.split(link[-1])
    out = os.path.abspath(out)
    odir = os.path.join(os.path.dirname(out), 'reach-obj')
    os.makedirs(odir, exist_ok=True)

    def one(b):
        dst = os.path.join(odir, b)
        src = os.path.join(ROOT, cmds[b][1])
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(
                os.path.join(ROOT, build_dir, 'obj', b)) and os.path.getmtime(dst) >= os.path.getmtime(src):
            return b, dst
        return b, dst if debug_object(cmds[b][0], dst, object_flags(cmds[b][0])) else None
    swap = {}
    with ThreadPoolExecutor(jobs) as ex:
        for b, dst in ex.map(one, sorted(objs)):
            if dst is None:
                sys.exit('could not instrument %s' % b)
            swap[os.path.join(build_dir, 'obj', b)] = dst
    argv = [swap.get(a, a) for a in argv]
    argv[argv.index('-o') + 1] = out
    subprocess.run(argv, cwd=ROOT, check=True)
    print('linked %s with %d instrumented game objects; run it with '
          'REACH_LIST_FILE=<list>' % (out, len(swap)))
    return len(swap)


# --------------------------------------------------------------------- run

def run_suite(bin_path, out, frames, scripts, matrix=True, jobs=4, timeout=3600):
    sys.path.insert(0, TOOLS)
    from safe_process import run
    out = os.path.abspath(out)
    lists = os.path.join(out, 'lists')
    os.makedirs(lists, exist_ok=True)
    results = {}
    env0 = {k: v for k, v in os.environ.items() if k != 'REACH_LIST_FILE'}
    env0.setdefault('SDL_VIDEODRIVER', 'dummy')
    env0.setdefault('SDL_AUDIODRIVER', 'dummy')
    todo = []
    if matrix:
        todo.append(('matrix', [sys.executable, os.path.join(TOOLS, 'scene_matrix.py'),
                                '--bin', bin_path, '--frames', str(frames),
                                '--jobs', str(jobs), '--out', os.path.join(out, 'scene_matrix.txt'),
                                '--logdir', os.path.join(out, 'matrix-logs')]))
    for s in scripts:
        path = s if os.path.exists(s) else os.path.join(TOOLS, 'playthrough', s + '.txt')
        name = os.path.splitext(os.path.basename(path))[0]
        todo.append((name, [sys.executable, os.path.join(TOOLS, 'playthrough.py'), path,
                            '--bin', bin_path, '--out', os.path.join(out, 'playthrough', name)]))

    def one(job):
        name, cmd = job
        env = dict(env0, REACH_LIST_FILE=os.path.join(lists, '%s.%%p.txt' % name))
        r = run(cmd, cwd=ROOT, env=env, timeout=timeout,
                log_path=os.path.join(out, 'logs', name + '.log'))
        return name, dict(returncode=r.returncode, reason=r.reason, cmd=cmd[1:3])
    # the matrix runs its own jobs in parallel; playthroughs one each
    with ThreadPoolExecutor(1 + len(scripts)) as ex:
        for name, res in ex.map(one, todo):
            results[name] = res
            print('%-14s rc=%s %s' % (name, res['returncode'], res['reason'] or ''))
    json.dump(dict(binary=bin_path, frames=frames, jobs=results),
              open(os.path.join(out, 'run.json'), 'w'), indent=1)
    return results


# -------------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build-reach')
    b.add_argument('--out', default=DEFAULT_BIN)
    b.add_argument('--build-dir', default='pc/build')
    b.add_argument('--jobs', type=int, default=8)
    r = sub.add_parser('run')
    r.add_argument('--bin', default=DEFAULT_BIN)
    r.add_argument('--out', default=os.path.join(ROOT, 'build', 'coverage', 'run'))
    r.add_argument('--frames', type=int, default=300)
    r.add_argument('--jobs', type=int, default=4, help='scene-matrix jobs')
    r.add_argument('--timeout', type=int, default=3600, help='seconds per job')
    r.add_argument('--scripts', nargs='*', default=list(PLAYTHROUGHS))
    r.add_argument('--no-matrix', action='store_true')
    p = sub.add_parser('report')
    p.add_argument('inputs', nargs='+')
    p.add_argument('--bin', help='binary that wrote the lists (names hex entries); '
                   'also decides which functions are in the build')
    p.add_argument('--json', default=os.path.join(ROOT, 'build', 'coverage', 'coverage.json'))
    p.add_argument('--md', default=os.path.join(ROOT, 'build', 'coverage', 'coverage.md'))
    a = ap.parse_args(argv)
    if a.cmd == 'build-reach':
        build_reach(a.build_dir, a.out, a.jobs)
        return 0
    if a.cmd == 'run':
        run_suite(os.path.abspath(a.bin), a.out, a.frames, a.scripts,
                  matrix=not a.no_matrix, jobs=a.jobs, timeout=a.timeout)
        return 0
    names = addr_names = None
    if a.bin:
        names, addr_names = binary_functions(a.bin)
    reached, unresolved, sources = read_inputs(a.inputs, addr_names)
    rep = build_report(load_universe(), reached, set(names) if names else None,
                       unresolved, sources)
    if a.bin:
        rep['binary'] = os.path.relpath(os.path.abspath(a.bin), ROOT)
    for path, text in ((a.json, json.dumps(rep, indent=1)), (a.md, markdown(rep))):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, 'w') as fh:
            fh.write(text + ('' if text.endswith('\n') else '\n'))
    s = rep['summary']
    for k in ('live_total', 'live_overlays', 'main_game', 'main_sdk', 'dead_overlays'):
        print('%-14s %5d / %5d  %5.1f%%' % (k, s[k]['reached'], s[k]['in_build'], s[k]['percent']))
    print('wrote %s and %s' % (a.json, a.md))
    return 0


if __name__ == '__main__':
    sys.exit(main())
