#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gen_overlay_symtab.py -- the overlay address -> native function tables.

On the PS1 the 26 EXN overlays share one window (0x80132000..); the main
binary reaches the loaded one only through PS1 code addresses:
scene_entry_set stores `game_mode_step = func_80144A14` (NAME_ENT's
name_ent_intro_main when prev_step is 17), game_main calls through it, and
overlay data tables hold code addresses too.  On the PC every overlay is a
native module (pc/src/overlay_<OV>.c, functions prefixed `<OV>_`), so an
overlay code address means "the function at that address in whichever EXN
is loaded" -- pc/hal/overlay.c resolves it against the overlay the CD HAL
last read into the window (pc/hal/cd.c).

This writes pc/src/overlay_symtab.c:
  * per overlay, the sorted table {PS1 address, native function} of every
    module function, keyed by its config/overlay/<OV>.sym address (a
    `func_XXXXXXXX` name's own address too, where the window was widened
    over a hoisted head);
  * one trampoline `func_XXXXXXXX` for every overlay function address in
    any .sym map: `movl $ADDR, %eax; jmp overlay_tramp` -- the common stub
    keeps the argument registers and jumps to the resolved function, so a
    direct call, and a pointer taken in C (`game_mode_step = func_80144A14`),
    run the loaded overlay's code with the caller's arguments.

Pointers that reach code as raw PS1 addresses (a table word, an s32) are
handled at run time by the fault handler in pc/hal/overlay.c, which uses
the same tables.

Needs the module objects (it reads their symbols with nm), so run it after
a build:
    make -f pc/Makefile all && python3 pc/tools/gen_overlay_symtab.py
then rebuild; pc/tools/prune_stub_file.py drops the old `return 0` stubs
that the trampolines replace.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import spell  # noqa: E402
OBJ = os.path.join(ROOT, 'pc/build/obj')
SYM_DIR = os.path.join(ROOT, spell('config/overlay'))
OUT = os.path.join(ROOT, 'pc/src/overlay_symtab.c')
WINDOW_LO, WINDOW_HI = 0x80132000, 0x80232000


def module_functions(ov):
    obj = os.path.join(OBJ, f'overlay_{ov}.o')
    if not os.path.exists(obj):
        sys.exit(f'{obj} missing: build first (make -f pc/Makefile all)')
    out = subprocess.run(['nm', '--defined-only', obj], capture_output=True, text=True).stdout
    fns = set()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'Tt' and p[2].startswith(ov + '_'):
            fns.add(p[2])
    return fns


def main_referenced_addresses():
    """overlay-window code addresses the main binary's C names (func_801xxxxx
    in src/main/{matched,pc-port,semi-matched,archive}): every one needs a
    trampoline, found in a table or not"""
    out = set()
    for sub in ('matched', 'pc-port', 'semi-matched', 'archive'):
        d = os.path.join(ROOT, spell('src/main/' + sub))
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if not f.endswith('.c'):
                continue
            for m in re.finditer(r'\bfunc_(80[12][0-9A-Fa-f]{5})\b', open(os.path.join(d, f), errors='replace').read()):
                a = int(m.group(1), 16)
                if WINDOW_LO <= a < WINDOW_HI:
                    out.add(a)
    return out


def symbol_start(ov, name):
    """`@SymbolStart: 0x...` in src/overlay/<OV>/<name>.c, or None"""
    path = os.path.join(ROOT, spell('src/overlay'), ov, name + '.c')
    if not os.path.exists(path):
        return None
    with open(path, errors='replace') as fh:
        m = re.search(r'(?m)^[ \t]*(?:/\*|\*)?[ \t]*@SymbolStart:[ \t]*0x([0-9A-Fa-f]{8})', fh.read())
    return int(m.group(1), 16) if m else None


def sym_entries(ov):
    out = []
    for line in open(os.path.join(SYM_DIR, f'{ov}.sym')):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(':')
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        try:
            size = int(parts[1], 0)
        except ValueError:
            size = 0
        out.append((addr, size, parts[2]))
    return out


def main():
    overlays = sorted(f[len('overlay_'):-2] for f in os.listdir(os.path.join(ROOT, 'pc/src'))
                      if f.startswith('overlay_') and f.endswith('.c') and f != 'overlay_symtab.c')
    tables = {}
    all_addrs = set()
    referenced = main_referenced_addresses()
    report = []
    for ov in overlays:
        fns = module_functions(ov)
        rows = {}
        spans = []
        for addr, size, name in sym_entries(ov):
            native = f'{ov}_{name}'
            if native not in fns:
                continue
            spans.append((addr, size, native))
            if WINDOW_LO <= addr < WINDOW_HI:
                rows.setdefault(addr, native)
            m = re.fullmatch(r'func_([0-9A-Fa-f]{8})', name)
            if m:
                a2 = int(m.group(1), 16)
                if WINDOW_LO <= a2 < WINDOW_HI:
                    rows.setdefault(a2, native)
            # a window widened over a hoisted head: the .sym keeps the
            # labelled entry, the C header the true start, and callers keep
            # the head address (func_80135F0C -> date_parade_step)
            a3 = symbol_start(ov, name)
            if a3 is not None and WINDOW_LO <= a3 < WINDOW_HI:
                rows.setdefault(a3, native)
        spans.sort()
        # an address the main binary names that is no function start here: a
        # cross-prologue head the .sym left in the previous window belongs to
        # the function whose prologue follows within 0x80 bytes
        for a in sorted(referenced - set(rows)):
            nxt = [sp for sp in spans if a < sp[0] <= a + 0x80]
            if nxt and any(sp[0] <= a < sp[0] + sp[1] for sp in spans):
                rows[a] = nxt[0][2]
                report.append(f'{ov}: 0x{a:08X} -> {nxt[0][2]} (head before 0x{nxt[0][0]:08X})')
        tables[ov] = sorted(rows.items())
        all_addrs.update(rows)
    all_addrs |= referenced

    o = []
    o.append('/* Generated by pc/tools/gen_overlay_symtab.py -- do not edit.')
    o.append(' * The overlay address -> native function tables and the func_8013xxxx..')
    o.append(' * trampolines (see the generator docstring and pc/hal/overlay.c). */')
    o.append('#include "overlay_symtab.h"')
    o.append('#include "pc_asm.h"   /* PC_ASM_TYPE_FN: ELF .type / COFF .def */')
    o.append('')
    for ov in overlays:
        for _, native in tables[ov]:
            o.append(f'extern void {native}(void);')
    o.append('')
    for ov in overlays:
        o.append(f'static const OverlaySym tab_{ov}[] = {{')
        for addr, native in tables[ov]:
            o.append(f'    {{ 0x{addr:08X}u, (void *){native} }},')
        o.append('};')
    o.append('')
    o.append('const OverlayTable g_overlay_tables[] = {')
    for ov in overlays:
        o.append(f'    {{ "{ov}", tab_{ov}, sizeof tab_{ov} / sizeof tab_{ov}[0] }},')
    o.append('};')
    o.append('const int g_overlay_table_count = sizeof g_overlay_tables / sizeof g_overlay_tables[0];')
    o.append('')
    o.append('/* One trampoline per overlay function address (any overlay). */')
    o.append('__asm__(')
    o.append('    ".text\\n"')
    for addr in sorted(all_addrs):
        n = f'func_{addr:08X}'
        o.append(f'    ".globl {n}\\n" PC_ASM_TYPE_FN("{n}") "{n}:\\n movl $0x{addr:08X}, %eax\\n jmp overlay_tramp\\n"')
    o.append(');')
    open(OUT, 'w').write('\n'.join(o) + '\n')
    print(f'  {len(report)} main-referenced addresses mapped to the function whose prologue follows '
          f'(-v lists them)')
    if '-v' in sys.argv:
        for r in report:
            print('  ' + r)
    print(f'{OUT}: {len(overlays)} overlays, {sum(len(t) for t in tables.values())} functions, '
          f'{len(all_addrs)} trampolines')


if __name__ == '__main__':
    main()
