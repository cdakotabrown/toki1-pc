#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate PC stubs for the FUNCTIONS the PC-port link cannot resolve.

A referenced function with no C anywhere in the build (an asm_only main
function, an overlay function another overlay file calls that has no .c of
its own, a `func_XXXXXXXX` used as a value in a jump table) gets

    s32 name() { return 0; }

in pc/src/missing_funcs_gen.c -- the same contract the hand-maintained
pc/src/missing_funcs.c has always given such symbols.  Only ADDRESS-NAMED and
OVERLAY-PREFIXED names are stubbed: `func_80133E78`, `GEKO_func_80133E78`,
`DATE_date_rank_x`.  A plain named function that is undefined is reported on
stderr instead, because a name came from config/symbol_names.json and the
function therefore HAS matched C: the cause is a stale pc/Makefile
HAL_EXCLUDES entry or an override file that no longer compiles, not a
missing stub.

A `func_<ADDR>` whose function exists under another name -- a lowercase-hex
spelling (`func_80044ac0`, 47 of them in the tree) or an absorbed-head entry
address whose window now carries the host's landed name (callers keep the
head address by doctrine: `func_80041A70` -> `game_work_init1`) -- gets a
naked tail-call trampoline instead of a stub.

Usage:
    make -f pc/Makefile all -k 2>&1 | python3 pc/tools/gen_missing_funcs.py
    (reads link errors on stdin; rewrites pc/src/missing_funcs_gen.c in place; `-o -` prints)

Incremental: the names the previous generated file provided are kept, minus
any that pc/build/obj now defines (nm), so re-running only tracks the build.
"""
import glob
import os
import re
import subprocess
import sys

from safe_process import lines

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import spell  # noqa: E402
OVERLAYS = sorted(os.path.basename(d) for d in glob.glob(os.path.join(ROOT, spell('src/overlay/*'))) if os.path.isdir(d))
PREFIXED = re.compile(r'^(?:' + '|'.join(re.escape(o) for o in OVERLAYS) + r')_\w+$') if OVERLAYS else None
ADDRESS = re.compile(r'^(?:func|FUN)_[0-9A-Fa-f]{8}$')
DATA_LIKE = re.compile(r'^(?:D|Da|Db|Dc|Du|D_arr|D_byte)_[0-9A-Fa-f]{8}(?:_[a-z])?$')


OUT = os.path.join(ROOT, 'pc/src/missing_funcs_gen.c')


def previous_names():
    """what the previous generated file provided (incremental: a link log only
    lists what was undefined WITH it linked)"""
    if not os.path.exists(OUT):
        return set()
    text = open(OUT, errors='replace').read()
    return set(re.findall(r'^s32 (\w+)\(\) \{ return 0; \}', text, re.M)) | \
        set(re.findall(r'^__attribute__\(\(naked\)\) void (\w+)\(void\)', text, re.M))


def link_objects():
    """the objects pc/Makefile links (NOT the directory listing: an object of a
    deleted source stays in pc/build/obj and would count as a definition)"""
    r = subprocess.run(['make', '-f', 'pc/Makefile', '--eval', 'print-objs: ; @echo $(ALL_OBJS)', 'print-objs'],
                       capture_output=True, text=True, cwd=ROOT)
    return [os.path.join(ROOT, o) for o in r.stdout.split() if os.path.exists(os.path.join(ROOT, o))]


def defined_symbols():
    out = set()
    for obj in link_objects():
        if os.path.basename(obj) == 'missing_funcs_gen.o':
            continue                      # our own previous output
        r = subprocess.run(['nm', '-g', '--defined-only', obj], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                out.add(parts[2])
    return out


def landed_name_at(addr):
    """the landed main-scope name of a function address, else None"""
    global _NAMES
    if _NAMES is None:
        import json
        doc = json.load(open(os.path.join(ROOT, spell('config/symbol_names.json'))))['symbols']
        _NAMES = {int(k, 16): e['name'] for k, e in doc.items()
                  if ':' not in k and e.get('kind') == 'func' and e.get('landed')}
    return _NAMES.get(addr)


_NAMES = None


def main():
    undef = set((name for line in lines('/dev/stdin') for name in re.findall(r"undefined reference to `([A-Za-z_]\w*)'", line))) | previous_names()
    defined = defined_symbols()
    undef = sorted(undef)
    stubs, named, tramps = [], [], []
    for n in undef:
        if n in defined or DATA_LIKE.match(n):
            continue
        if ADDRESS.match(n):
            # a lowercase-hex spelling of a function that exists under its
            # canonical (upper-hex or landed) name: a trampoline, not a stub --
            # 47 matched/overlay files still spell e.g. `func_80044ac0`
            addr = int(n.split('_')[1], 16)
            for target in (f'func_{addr:08X}', landed_name_at(addr)):
                if target and target != n and target in defined:
                    tramps.append((n, target))
                    break
            else:
                stubs.append(n)
            continue
        if PREFIXED and PREFIXED.match(n):
            stubs.append(n)
        elif n[0].isupper() or '_' in n:
            # could be a function or a global; the globals generator takes the
            # ones symbol_names.json knows -- everything else is reported
            named.append(n)
    if named:
        print('gen_missing_funcs: %d undefined NAMED symbols left alone (stale HAL_EXCLUDES / override, or a '
              'global the globals generator does not know): %s' % (len(named), ' '.join(named[:60])), file=sys.stderr)
    out = ['/* Generated by pc/tools/gen_missing_funcs.py -- return-0 stubs for functions with no C in the build */',
           '#include "types.h"', '']
    for n in stubs:
        out.append('s32 %s() { return 0; }' % n)
    if tramps:
        out += ['', '/* address-name spellings (lowercase hex, absorbed-head entry addresses) of',
                '   functions that exist under another name: tail-call trampolines */']
        for n, target in tramps:
            out.append('__attribute__((naked)) void %s(void) { __asm__("jmp %s"); }' % (n, target))
    text = '\n'.join(out) + '\n'
    if '-o' in sys.argv and sys.argv[sys.argv.index('-o') + 1] == '-':
        sys.stdout.write(text)
    else:
        open(OUT, 'w').write(text)
    print('gen_missing_funcs: %d stubs, %d trampolines -> %s' % (len(stubs), len(tramps), os.path.relpath(OUT, ROOT)), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
