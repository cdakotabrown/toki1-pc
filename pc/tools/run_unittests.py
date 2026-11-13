#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""run_unittests.py -- the pc/tools unit tests, one process per test file,
several files at once.

`python3 -m unittest discover -s pc/tools -p 'test_*.py'` runs ~135 files
one after another, and most of the time goes to files that boot the game
headless, each on one core.  This runs every file as its own
`python3 -m unittest discover -s pc/tools -p <file>` from the repo root
(the same loader and cwd as the full discover), up to
--jobs at once, longest-first from the timings of the previous run
(build/unittest-times.json), and prints a discover-style summary:

    python3 pc/tools/run_unittests.py [--jobs N] [-p 'test_*.py'] [--log DIR]

Exit status is non-zero when any file fails or errors.  Each file's full
output is kept in --log (default build/unittests/<module>.log).
"""
import argparse
import ast
import concurrent.futures as cf
import glob
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, 'pc', 'tools')
TIMES = os.path.join(ROOT, 'build', 'unittest-times.json')


def test_classes(path):
    """the module-level classes of a test file that are TestCases: a base
    named TestCase, or a class of the same file that is one (a subclass of
    a shared base -- `class FooWindows(Foo)` -- is a test class too; a
    regex on `TestCase)` missed those, and a split file silently dropped
    their tests, 2026-11-13)"""
    tree = ast.parse(open(path, encoding='utf-8').read())
    found = []
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        for b in node.bases:
            name = b.attr if isinstance(b, ast.Attribute) else getattr(b, 'id', None)
            if name == 'TestCase' or name in found:
                found.append(node.name)
                break
    return found


def units(module, split):
    """(label, module, class or None): a file whose last run took longer
    than `split` seconds and holds several TestCase classes runs one job
    per class (the slowest files boot the game many times)."""
    if split is not None:
        classes = test_classes(os.path.join(TOOLS, module + '.py'))
        if len(classes) > 1:
            return [('%s.%s' % (module, c), module, c) for c in classes]
    return [(module, module, None)]


def run_one(unit, log_dir):
    label, module, cls = unit
    t0 = time.time()
    # exactly what discover does for this one file, from the repo root
    # (several tests use repo-relative paths); -k keeps one class
    # -v: the log names each skip's reason (EXCLUDED ones are summarised)
    cmd = [sys.executable, '-m', 'unittest', 'discover', '-v', '-s', 'pc/tools', '-p', module + '.py']
    if cls:
        cmd += ['-k', '%s.%s.*' % (module, cls)]
    r = subprocess.run(cmd, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       stdin=subprocess.DEVNULL)
    out = r.stdout.decode('utf-8', 'replace')
    with open(os.path.join(log_dir, label + '.log'), 'w') as f:
        f.write(out)
    ran = re.findall(r'^Ran (\d+) tests?', out, re.M)
    skipped = re.findall(r'skipped=(\d+)', out)
    if r.returncode == 5 and cls:          # no tests matched: a helper class
        r.returncode = 0
    return dict(module=label, rc=r.returncode, secs=time.time() - t0,
                ran=int(ran[-1]) if ran else 0,
                skipped=int(skipped[-1]) if skipped else 0,
                failed=re.findall(r'^(?:FAIL|ERROR): .*$', out, re.M),
                excluded=sorted(set(re.findall(r"skipped '(EXCLUDED[^']*)'", out))), out=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('-p', '--pattern', default='test_*.py')
    ap.add_argument('--split', type=float, default=60.0,
                    help='split a file into one job per TestCase class when its last run '
                         'took longer than this many seconds (default 60)')
    ap.add_argument('--log', default=os.path.join(ROOT, 'build', 'unittests'))
    args = ap.parse_args()
    os.makedirs(args.log, exist_ok=True)
    modules = sorted(os.path.basename(p)[:-3] for p in glob.glob(os.path.join(TOOLS, args.pattern)))
    try:
        times = json.load(open(TIMES))
    except (OSError, ValueError):
        times = {}
    jobs = []
    for m in modules:
        jobs += units(m, None if times.get(m, 0) < args.split else args.split)
    jobs.sort(key=lambda u: -times.get(u[0], times.get(u[1], 60.0)))
    t0 = time.time()
    results = []
    with cf.ThreadPoolExecutor(args.jobs) as pool:
        for res in pool.map(lambda u: run_one(u, args.log), jobs):
            results.append(res)
    wall = time.time() - t0
    os.makedirs(os.path.dirname(TIMES), exist_ok=True)
    merged = dict(times)
    merged.update({r['module']: round(r['secs'], 1) for r in results})
    for m in modules:                    # a split file's total, for next time
        parts = [r['secs'] for r in results if r['module'].startswith(m + '.')]
        if parts:
            merged[m] = round(sum(parts), 1)
    json.dump(merged, open(TIMES, 'w'), indent=1)
    bad = [r for r in results if r['rc'] != 0]
    for r in bad:
        print('---- %s (exit %d, log %s/%s.log)' % (r['module'], r['rc'], args.log, r['module']))
        for line in r['failed'] or r['out'].strip().splitlines()[-5:]:
            print(line)
    # a test that cannot run in this checkout says so (cf. PC_TESTS_NO_DISC
    # in pc/tests/test_main.c), instead of hiding in the skip count
    for r in sorted(results, key=lambda r: r['module']):
        for why in r['excluded']:
            print('%s: %s' % (r['module'], why))
    total = sum(r['ran'] for r in results)
    skipped = sum(r['skipped'] for r in results)
    slow = sorted(results, key=lambda r: -r['secs'])[:5]
    print('slowest: ' + ', '.join('%s %.0fs' % (r['module'], r['secs']) for r in slow))
    print('Ran %d tests in %d files (%d jobs) in %.1fs, %d at once'
          % (total, len(modules), len(results), wall, args.jobs))
    print()
    if bad:
        print('FAILED (files=%d, skipped=%d)' % (len(bad), skipped))
        return 1
    print('OK (skipped=%d)' % skipped)
    return 0


if __name__ == '__main__':
    sys.exit(main())
