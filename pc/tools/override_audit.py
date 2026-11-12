#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""override_audit.py -- retire src/main/pc-port overrides in favour of the matched C.

Many pc-port files predate the matched function they shadow: "neutralized"
shadows that were written while a global was unmapped, `{ }` stubs, "loose
translations from ASM" with raw PS1 addresses.  The matched C is the source
of truth, so the default is to delete the override -- but only if the matched
function survives the PC build.  This tool does the experiment:

  1. baseline: build + scene matrix (pc/tools/scene_matrix.py);
  2. move the candidate overrides out of src/main/pc-port (to --stash);
  3. build; a compile/link failure puts the named files back;
  4. scene matrix; for every scene that got worse (OK -> CRASH/HANG/EXIT),
     the crash backtrace names functions -- removed overrides among them go
     back; repeat 3-4 until nothing gets worse;
  5. report: retired (deleted from pc-port, `git rm`), restored (with the
     scene + function that needed them), and scenes whose END STATE changed
     (informational: the matched C may take another path -- often the fix).

Candidates are given by name or by category of pc/tools/override_audit.py
--list (neutralized / stub / tiny / loose / ptrwidth / other).

    python3 pc/tools/override_audit.py --category neutralized stub --apply
    python3 pc/tools/override_audit.py --names foo bar            # dry run: no git rm
"""
import argparse
import glob
import os
import re
import shutil
import sys
import signal
import tempfile

from safe_process import lines, run, terminate

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import spell  # noqa: E402
PCPORT = os.path.join(ROOT, 'src/main/pc-port')
MATCHED = os.path.join(ROOT, spell('src/main/matched'))


def categorize():
    cats = {}
    for f in sorted(glob.glob(os.path.join(PCPORT, '*.c'))):
        b = os.path.basename(f)
        if not os.path.exists(os.path.join(MATCHED, b)):
            continue
        t = open(f, errors='replace').read()
        head = t[:1500].lower()
        body = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
        nsemi = body.count(';')
        if 'neutraliz' in head or 'pc-port shadow' in head:
            c = 'neutralized'
        elif 'loose translation' in head or 'todo' in head:
            c = 'loose'
        elif 'stub' in head and nsemi < 6:
            c = 'stub'
        elif any(k in head for k in ('pointer width', 'pointer-width', '64-bit', 'x86-64', 'sign-extend')):
            c = 'ptrwidth'
        elif nsemi < 4:
            c = 'tiny'
        else:
            c = 'other'
        cats.setdefault(c, []).append(b[:-2])
    return cats


def build(jobs=2):
    r = run(['make', '-f', 'pc/Makefile', f'-j{jobs}', 'all'],
            cwd=ROOT, timeout=600)
    return r.ok, r.stdout + (f'\n[harness] {r.reason}' if r.reason else '')


def matrix(out, logdir, frames, jobs):
    if os.path.isdir(logdir):
        shutil.rmtree(logdir)
    if os.path.exists(out):
        os.remove(out)
    r = run([sys.executable, 'pc/tools/scene_matrix.py', '--frames', str(frames), '--jobs', str(jobs),
             '--out', out, '--logdir', logdir], cwd=ROOT,
            timeout=211 * 65 / jobs + 120)
    r.require_ok()
    res = {}
    if os.path.exists(out):
        for line in lines(out):
            p = line.split()
            if len(p) >= 5 and p[0].isdigit():
                scene = int(p[0])
                if scene in res:
                    raise RuntimeError('duplicate scene in matrix')
                res[scene] = (p[1], p[4])
    if set(res) != set(range(211)):
        raise RuntimeError(f'incomplete scene matrix: {len(res)}/211; refusing to grade')
    return res


def bt_functions(logdir, scene):
    path = os.path.join(logdir, f'scene{scene}.log')
    if not os.path.exists(path):
        return set()
    names = set()
    for line in lines(path):
        for m in re.finditer(r'(?:\(|sym=)(\w+)\+0x[0-9a-f]+', line):
            names.add(m.group(1))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--category', nargs='*', default=[])
    ap.add_argument('--names', nargs='*', default=[])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--apply', action='store_true', help='git rm the retired overrides')
    ap.add_argument('--frames', type=int, default=300)
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--build-jobs', type=int, default=2)
    ap.add_argument('--stash', help='backup directory (default: unique directory in /tmp)')
    a = ap.parse_args()
    cats = categorize()
    if a.list:
        for c, v in sorted(cats.items()):
            print(f'{c} ({len(v)}): {" ".join(v)}')
        return 0
    cand = list(a.names)
    for c in a.category:
        cand += cats.get(c, [])
    cand = sorted(set(cand))
    if not cand:
        sys.exit('no candidates')
    if a.jobs <= 0 or a.build_jobs <= 0 or a.frames <= 0:
        ap.error('jobs and frames must be positive')
    if any(not re.fullmatch(r'[A-Za-z_]\w*', n) for n in cand):
        ap.error('candidates must be C identifiers, not paths')
    if a.stash is None:
        a.stash = tempfile.mkdtemp(prefix='override-audit-')
    os.makedirs(a.stash, exist_ok=True)
    work = os.path.join(a.stash, 'work')
    os.makedirs(work, exist_ok=True)

    if any(os.path.exists(os.path.join(a.stash, n + '.c')) for n in cand):
        sys.exit('stash already contains a candidate; refusing to overwrite backups')
    ok, log = build(a.build_jobs)
    if not ok:
        sys.exit('baseline build failed:\n' + log[-2000:])
    base = matrix(os.path.join(work, 'base.txt'), os.path.join(work, 'base_logs'), a.frames, a.jobs)
    print(f'baseline: {sum(1 for v in base.values() if v[1] == "OK")}/{len(base)} OK')

    return experiment(a, cand, work, base)


def experiment(a, cand, work, base):
    removed, restored = set(), {}
    try:
        return retire(a, cand, work, base, removed, restored)
    finally:
        # retire() clears removed only after a checked successful apply.
        # Otherwise every experimental source removal is rolled back.
        conflicts = []
        for n in sorted(removed):
            src = os.path.join(PCPORT, n + '.c')
            backup = os.path.join(a.stash, n + '.c')
            if os.path.exists(src):
                conflicts.append(f'{src}; backup kept at {backup}')
                continue
            shutil.copy2(backup, src)
        if conflicts:
            raise RuntimeError('restore conflicts: ' + ', '.join(conflicts))


def retire(a, cand, work, base, removed, restored):
    for n in cand:
        src = os.path.join(PCPORT, n + '.c')
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(a.stash, n + '.c'))
            removed.add(n)
            os.remove(src)

    def restore(n, why):
        shutil.copy2(os.path.join(a.stash, n + '.c'), os.path.join(PCPORT, n + '.c'))
        removed.discard(n)
        restored[n] = why

    for rnd in range(12):
        ok, log = build(a.build_jobs)
        if not ok:
            bad = set(re.findall(r'(\w+)\.c:\d+', log)) & removed
            bad |= set(re.findall(r'(\w+)\.o:.*?undefined reference', log)) & removed
            bad |= set(re.findall(r'(\w+)\.o: in function', log)) & removed
            bad |= set(re.findall(r'multiple definition of `(\w+)\'', log)) & removed
            undef = set(re.findall(r"undefined reference to `(\w+)'", log))
            # A removed override may define symbols under names unrelated to its
            # basename (pc-port side channels such as func_80049370_reset in
            # prim_alloc.c).  Attribute an undefined symbol to the stashed file
            # that used to define it.
            bodies = {n: open(os.path.join(a.stash, n + '.c'), errors='replace').read()
                      for n in removed if os.path.exists(os.path.join(a.stash, n + '.c'))}
            for sym in undef:
                pat = re.compile(r'\b' + re.escape(sym) + r'\s*\(')
                bad |= {n for n, t in bodies.items() if pat.search(t)}
            bad |= set(m for m in undef if m in removed)
            if not bad:
                print('Build failed with no candidate parsed from error log:\n' + log[-1500:])
                for n in list(removed):
                    restore(n, 'build failed (unattributed)')
                break
            for n in bad:
                restore(n, 'build')
            continue
        cur = matrix(os.path.join(work, f'r{rnd}.txt'), os.path.join(work, f'r{rnd}_logs'), a.frames, a.jobs)
        worse = [s for s in cur if base.get(s, ('', 'OK'))[1] == 'OK' and cur[s][1] != 'OK']
        if not worse:
            break
        hit = False
        for s in worse:
            fns = bt_functions(os.path.join(work, f'r{rnd}_logs'), s) & removed
            for n in fns:
                restore(n, f'scene {s} {cur[s][1]}')
                hit = True
        if not hit:
            print('regressions with no removed function in the backtrace: '
                  + ', '.join(f'{s}:{cur[s][1]}' for s in worse))
            print('  (restoring nothing; inspect', os.path.join(work, f'r{rnd}_logs'), ')')
            break
    final = matrix(os.path.join(work, 'final.txt'), os.path.join(work, 'final_logs'), a.frames, a.jobs)
    if any(base[s][1] == 'OK' and final[s][1] != 'OK' for s in base):
        raise RuntimeError('final matrix regressed; restoring experimental overrides')
    print(f'final: {sum(1 for v in final.values() if v[1] == "OK")}/{len(final)} OK')
    changed = [(s, base[s][0], final[s][0]) for s in final if s in base and base[s][0] != final[s][0]]
    print(f'retired {len(removed)}: {" ".join(sorted(removed))}')
    print(f'restored {len(restored)}: ' + ', '.join(f'{n} ({w})' for n, w in sorted(restored.items())))
    print('end state changed: ' + ', '.join(f'{s}: {b} -> {f}' for s, b, f in changed))
    if a.apply and removed:
        run(['git', 'rm', '-q', '--cached'] + [f'src/main/pc-port/{n}.c' for n in sorted(removed)],
            cwd=ROOT).require_ok()
        removed.clear()
    else:
        for n in sorted(removed):
            restore(n, 'dry run')
    return 0


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
