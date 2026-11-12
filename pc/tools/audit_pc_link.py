#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit the PC-port link: duplicate and undefined symbols, by object.

pc/Makefile links with -Wl,--allow-multiple-definition, so two definitions of
one symbol never fail the build: the FIRST object on the link line wins
silently (pc-port, matched, semi, archive, HAL, pc/src, overlays, in that
order).  A HAL replacement that the HAL_EXCLUDES list no longer names (a
rename moved the matched file) therefore loses to the matched C without a
word, and a stale pc-port stub wins over the real function.  This tool makes
both visible.

    python3 pc/tools/audit_pc_link.py            # duplicates among the objects make would link
    python3 pc/tools/audit_pc_link.py --undefined build.log   # bucket the linker's undefined refs
    python3 pc/tools/audit_pc_link.py --hal-excludes          # replacement coverage only

Run after `make -f pc/Makefile all -k` so the objects exist.  Exit status 0;
it reports, the reader decides (some duplicates are deliberate: the pc-port
override tier).

How to read the tier shapes (first listed object wins the link):
  * `pcport + matched` / `pcport + archive`: the override tier doing its job.
  * `matched + hal`, `archive + hal`: a HAL replacement that LOSES -- the
    matched name is missing from pc/Makefile HAL_EXCLUDES (a Sony name landed
    on a matched file the HAL implements).  Add it.
  * `pcport + hal`: a pc-port file shadows the HAL; usually a stale
    address-named stub that a rename made collide.  Delete the stub.
  * `pcsrc + pcsrc`: a stub in pc_port_stubs.c / missing_funcs.c beside a real
    definition in an overlay module: python3 pc/tools/prune_stub_file.py.
  * the HAL_EXCLUDES check: every name in HAL_EXCLUDES must be DEFINED by a
    pc/hal, pc/src or src/main/pc-port object, or it just makes the symbol
    undefined (comm the list against `nm -g --defined-only` of those objects).
"""
import collections
import os
import re
import subprocess
import sys

from safe_process import lines

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import spell  # noqa: E402
TIERS = [('src/main/pc-port', 'pcport'), ('src/main/matched', 'matched'), ('src/main/semi-matched', 'semi'),
         ('src/main/archive', 'archive'), ('pc/hal', 'hal'), ('pc/src/overlay_inits', 'ovinit'),
         ('pc/src', 'pcsrc'), ('src/overlay', 'overlay')]


def link_objects():
    out = subprocess.run(['make', '-f', 'pc/Makefile', '--eval', 'print-objs: ; @echo $(ALL_OBJS)', 'print-objs'],
                         capture_output=True, text=True, cwd=ROOT).stdout
    # pc/src/*.c and pc/src/overlay_*.c both list the overlay modules; make's
    # $^ dedups them on the link line, so dedup here too
    return list(dict.fromkeys(out.split()))


def source_tier(obj):
    """which source tier an object came from (by basename lookup, the Makefile's own rule)."""
    stem = os.path.splitext(os.path.basename(obj))[0]
    for d, tier in TIERS:
        for cand in (stem + '.c', stem + '_painted.c'):
            if os.path.exists(os.path.join(ROOT, spell(d), cand)):
                return tier
    if stem.startswith('overlay_'):
        return 'overlay-module'
    return '?'


def defined_symbols(obj):
    out = subprocess.run(['nm', '--defined-only', '-g', obj], capture_output=True, text=True, cwd=ROOT).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'TDBRCVW' and parts[1] not in 'VW':
            syms.append(parts[2])
    return syms


def duplicates():
    objs = [o for o in link_objects() if os.path.exists(os.path.join(ROOT, o))]
    where = collections.defaultdict(list)
    for o in objs:
        for s in defined_symbols(o):
            where[s].append(o)
    dups = {s: os for s, os_ in where.items() for os in [os_] if len(os) > 1}
    by_shape = collections.Counter()
    for s, os_ in sorted(dups.items()):
        tiers = [source_tier(o) for o in os_]
        by_shape[' + '.join(tiers)] += 1
        print(f'{s}: ' + ', '.join(f'{os.path.basename(o)} [{t}]' for o, t in zip(os_, tiers)))
    print(f'\n{len(dups)} duplicate symbols among {len(objs)} objects; by tier shape:')
    for shape, n in by_shape.most_common():
        print(f'  {n:4d} {shape}')
    return dups


UNDEF = re.compile(r"undefined reference to `([^']+)'")


def undefined(log):
    names = sorted({name for line in lines(log) for name in UNDEF.findall(line)})
    buckets = collections.defaultdict(list)
    for n in names:
        if re.match(r'^D_[0-9A-Fa-f]{8}$', n):
            buckets['D_ address global (gen_missing_globals.py)'].append(n)
        elif re.match(r'^func_[0-9A-Fa-f]{8}$', n):
            buckets['func_ address function'].append(n)
        elif re.match(r'^[A-Z][A-Za-z0-9]*_', n) and n.split('_')[0].isupper():
            buckets['overlay-prefixed'].append(n)
        elif n[0].islower() and '_' in n:
            buckets['named global/function (renamed; globals.c / HAL / trampolines?)'].append(n)
        else:
            buckets['other'].append(n)
    for b, ns in sorted(buckets.items()):
        print(f'== {b}: {len(ns)}')
        for n in ns:
            print('   ', n)
    print(f'\n{len(names)} distinct undefined symbols')


def hal_excludes():
    """Check effective make exclusions against linked replacement objects.

    Generated fallback files do not establish a HAL implementation. Report
    them separately rather than letting a return-zero stub pass the check.
    Missing objects are inconclusive: build before interpreting the result.
    """
    result = subprocess.run(
        ['make', '-f', 'pc/Makefile', '--eval',
         'print-excludes: ; @echo $(HAL_EXCLUDES)', 'print-excludes'],
        capture_output=True, text=True, cwd=ROOT, check=True)
    names = sorted(set(result.stdout.split()))
    replacements = collections.defaultdict(list)
    missing = []
    fallbacks = {'pc_port_stubs', 'missing_funcs', 'missing_funcs_stubs',
                 'stubs_remaining'}
    for obj in link_objects():
        if source_tier(obj) not in {'hal', 'pcsrc', 'pcport'}:
            continue
        if not os.path.exists(os.path.join(ROOT, obj)):
            missing.append(obj)
            continue
        for name in defined_symbols(obj):
            replacements[name].append(obj)
    uncovered = []
    stub_only = []
    for name in names:
        objects = replacements[name]
        real = [obj for obj in objects
                if os.path.splitext(os.path.basename(obj))[0] not in fallbacks]
        if not objects:
            uncovered.append(name)
            print(f'NO REPLACEMENT: {name}')
        elif not real:
            stub_only.append(name)
            print(f'FALLBACK ONLY: {name}: ' + ', '.join(objects))
    print(f'HAL_EXCLUDES: {len(names)} names, {len(uncovered)} uncovered, '
          f'{len(stub_only)} fallback-only, {len(missing)} missing objects')
    for obj in missing:
        print(f'NOT BUILT: {obj}')
    return uncovered, stub_only, missing


def main():
    if '--hal-excludes' in sys.argv:
        hal_excludes()
    elif '--undefined' in sys.argv:
        undefined(sys.argv[sys.argv.index('--undefined') + 1])
    else:
        duplicates()
        print()
        hal_excludes()
    return 0


if __name__ == '__main__':
    sys.exit(main())
