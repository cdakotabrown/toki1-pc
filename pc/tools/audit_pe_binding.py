#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_pe_binding.py -- every reference of the Windows build to a global
binds to the definition the link keeps, as on Linux.

Two PE/COFF behaviours differ from ELF and each broke the first Windows
build:

  WEAK      a weak DEFINITION (a function defined where an
            `__attribute__((weak))` declaration of it is visible) is a COFF
            weak external, and GNU ld may bind the name to another object's
            default of 0: pc/hal/cpu_cost.c's whole hal_cpu_cost_* family
            was 0, so no BIOS/HAL CPU time was charged.  Check: no symbol an
            object defines is left undefined in the executable.

  LOCAL     the COFF assembler binds an object's references to a global it
            defines to its own SECTION, so with --allow-multiple-definition
            and pc/ld/psx_globals.syms the object keeps a private copy of a
            global the link gives to another definition (cdq_head: the CD
            queue ran on a dead copy).  pc/tools/pe_rebind_relocs.py turns
            them back into symbol references before the link.  Check: no
            object still references, through its section, a section holding
            a global whose definition the link does not keep from it (an
            absolute PS1 address, or a name several objects define).

    python3 pc/tools/audit_pe_binding.py [--exe EXE] [--objs DIR] [--nm NM]
Exit 1 (and a list) on any WEAK or LOCAL finding; 2 when nothing is built.
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_rebind_relocs as pe  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_nm():
    for cand in ('x86_64-w64-mingw32-nm',
                 os.path.join(ROOT, 'build/mingw-root/usr/bin/x86_64-w64-mingw32-nm')):
        path = shutil.which(cand) or (cand if os.path.isfile(cand) else None)
        if path:
            return path
    return None


def exe_symbols(nm, exe):
    """{name: kind} of the executable, and the set of undefined names"""
    out = subprocess.run([nm, exe], capture_output=True, text=True).stdout
    kinds, undefined = {}, set()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            kinds.setdefault(p[2], p[1])
        elif len(p) == 2 and p[0] == 'U':
            undefined.add(p[1])
    return kinds, undefined


def object_view(path):
    """(defined external names, weak-defined names, section-relative
    references: [(section of the reference, [external names in the target
    section])])"""
    with open(path, 'rb') as fh:
        data = fh.read()
    secs, syms = pe.parse(data)
    sec_sym, ext_in = {}, {}
    defined, weak = set(), set()
    for s in syms:
        if s['name'].startswith('.weak.'):
            # a weak DEFINITION's default is code or data of this object; a
            # weak REFERENCE's default is the absolute 0 (not a definition)
            if s['sec'] > 0:
                weak.add(s['name'][len('.weak.'):].split('.', 1)[0])
            continue
        if s['sec'] <= 0:
            continue
        if (s['cls'] == pe.CLASS_STATIC and s['aux'] and s['value'] == 0 and
                s['name'] == secs[s['sec'] - 1]['name']):
            sec_sym[s['index']] = s['sec']
        elif s['cls'] == pe.CLASS_EXTERNAL:
            defined.add(s['name'])
            ext_in.setdefault(s['sec'], []).append(s['name'])
    refs = []
    for sec in secs:
        if sec['name'].startswith(pe.SKIP_PREFIXES) or not sec['nrel']:
            continue
        for r in range(sec['nrel']):
            _va, symi, typ = struct.unpack_from('<IIH', data, sec['relptr'] + r * 10)
            if typ in pe.ADDRESSING and symi in sec_sym and sec_sym[symi] in ext_in:
                refs.append((sec['name'], ext_in[sec_sym[symi]]))
    return defined, weak, refs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--exe', default=os.path.join(ROOT, 'pc/build-win/tokimeki-pc.exe'))
    ap.add_argument('--objs', default=os.path.join(ROOT, 'pc/build-win/obj'))
    ap.add_argument('--nm', default=None)
    args = ap.parse_args(argv)
    nm = args.nm or find_nm()
    if not nm or not os.path.isfile(args.exe):
        print('audit_pe_binding: needs the Windows build and MinGW nm '
              '(make -f pc/Makefile windows)')
        return 2
    objs = sorted(glob.glob(os.path.join(args.objs, '*.o')))
    kinds, undefined = exe_symbols(nm, args.exe)
    views = {os.path.basename(o): object_view(o) for o in objs}
    owners = {}
    for o, (defined, weak, _refs) in views.items():
        for s in defined | weak:
            owners.setdefault(s, []).append(o)
    findings = []
    for s in sorted(undefined):
        if s in owners:
            findings.append(f'WEAK  {s}: defined in {owners[s][0]}, left undefined '
                            f'(a weak default) in the executable')
    for o, (_defined, _weak, refs) in sorted(views.items()):
        seen = set()
        for where, names in refs:
            for s in names:
                lost = kinds.get(s) in ('A', 'a') or len(owners.get(s, ())) > 1
                if lost and (s, where) not in seen:
                    seen.add((s, where))
                    findings.append(f'LOCAL {o}: {where} reaches {s} through its own '
                                    f'section; the link keeps another definition')
    for f in findings:
        print(f)
    print(f'audit_pe_binding: {len(objs)} objects, {len(findings)} finding(s)')
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
