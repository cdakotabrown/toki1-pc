#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pe_rebind_relocs.py -- give a Windows (COFF) object ELF's binding of its
references to its own global definitions.

The port links every object with --allow-multiple-definition and
pc/ld/psx_globals.syms (--just-symbols: each PS1 global at its absolute RAM
address).  A translation unit may define a global the link gives to another
definition: src/main/pc-port/cdq_command_process.c defines cdq_head (the
matched TU's same-TU data idiom) while psx_globals.syms puts cdq_head at
0x800E8DA0; pc/src/missing_globals_gen.c and friends define fallbacks for
thousands of PS1 globals; two overlay modules can define one function.  On
Linux (ELF) the object's own references are relocations against the SYMBOL
and follow whichever definition the link keeps.  GCC writes the same
symbolic reference for Windows (`cdq_head(%rip)`), but the COFF assembler
resolves a reference to a global defined in the same object against the
object's SECTION, so the object kept using its private copy: on 2026-11-02
the CD queue ran on a dead cdq_head while get_CD_State2 and the trace read
0x800E8DA0.

The Windows build compiles with -ffunction-sections -fdata-sections, so
every global lives in a section of its own (`.bss$cdq_head`) and nothing
unnamed (a string, a jump table) shares it.  A relocation against the
section symbol of a section that holds exactly one external definition at
offset 0 and no other symbol is a reference to that definition; this
rewrites its symbol index to the definition's -- the ELF form.  The field
(the addend, which the assembler also folds the instruction's trailing
immediate bytes into) is unchanged: the definition starts the section.
Unwind tables and debug sections keep their section-relative form (they
describe the object's own code).  Idempotent.

    python3 pc/tools/pe_rebind_relocs.py OBJ [OBJ...]       rewrite in place
    python3 pc/tools/pe_rebind_relocs.py --report OBJ...    count, change nothing
Test: pc/tools/test_windows_build.py.
"""
import struct
import sys

IMAGE_FILE_MACHINE_AMD64 = 0x8664
SCN_LNK_NRELOC_OVFL = 0x01000000
CLASS_EXTERNAL = 2
CLASS_STATIC = 3

# AMD64 relocations that address a symbol: ADDR64, ADDR32, ADDR32NB,
# REL32, REL32_1..REL32_5, SECREL is left alone (debug info)
ADDRESSING = {1, 2, 3, 4, 5, 6, 7, 8, 9}

SKIP_PREFIXES = ('.pdata', '.xdata', '.debug', '.eh_frame', '.drectve', '.gnu', '.idata')


def parse(data):
    mach, nsec, _ts, symptr, nsym, opt, _ch = struct.unpack_from('<HHIIIHH', data, 0)
    if mach != IMAGE_FILE_MACHINE_AMD64:
        raise ValueError('not an x86-64 COFF object')
    strtab = symptr + nsym * 18

    def long_name(off):
        end = data.index(b'\0', strtab + off)
        return data[strtab + off:end].decode('latin-1')

    secs = []
    base = 20 + opt
    for i in range(nsec):
        h = base + i * 40
        raw = data[h:h + 8]
        if raw[:1] == b'/':                      # "/123": string table offset
            name = long_name(int(raw[1:].rstrip(b'\0')))
        else:
            name = raw.rstrip(b'\0').decode('latin-1')
        _vs, _va, rawsize, rawptr, relptr, _ln, nrel, _nln, ch = \
            struct.unpack_from('<IIIIIIHHI', data, h + 8)
        first = relptr
        if ch & SCN_LNK_NRELOC_OVFL and nrel == 0xFFFF:
            nrel = struct.unpack_from('<I', data, relptr)[0] - 1
            first = relptr + 10
        secs.append(dict(name=name, rawptr=rawptr, relptr=first, nrel=nrel))
    syms = []
    i = 0
    while i < nsym:
        off = symptr + i * 18
        raw = data[off:off + 8]
        name = long_name(struct.unpack_from('<I', raw, 4)[0]) if raw[:4] == b'\0\0\0\0' \
            else raw.rstrip(b'\0').decode('latin-1')
        value, secno, _typ, cls, aux = struct.unpack_from('<IhHBB', data, off + 8)
        syms.append(dict(index=i, name=name, value=value, sec=secno, cls=cls, aux=aux))
        i += 1 + aux
    return secs, syms


def plan(data):
    """-> [(reloc file offset, new symbol index)] for every section-relative
    reference to a section that is exactly one external definition"""
    secs, syms = parse(data)
    sec_sym = {}          # section-symbol index -> section number
    named = {}            # section number -> [symbols other than its section symbol]
    for s in syms:
        if s['sec'] <= 0:
            continue
        if (s['cls'] == CLASS_STATIC and s['aux'] and s['value'] == 0 and
                s['name'] == secs[s['sec'] - 1]['name']):
            sec_sym[s['index']] = s['sec']
        else:
            named.setdefault(s['sec'], []).append(s)
    owner = {}            # section number -> its sole external definition
    for secno, lst in named.items():
        if len(lst) == 1 and lst[0]['cls'] == CLASS_EXTERNAL and lst[0]['value'] == 0:
            owner[secno] = lst[0]['index']
    out = []
    for sec in secs:
        if sec['name'].startswith(SKIP_PREFIXES) or not sec['nrel']:
            continue
        for r in range(sec['nrel']):
            roff = sec['relptr'] + r * 10
            _va, symi, typ = struct.unpack_from('<IIH', data, roff)
            if typ in ADDRESSING and symi in sec_sym and sec_sym[symi] in owner:
                out.append((roff, owner[sec_sym[symi]]))
    return out


def rebind(path, write=True):
    with open(path, 'rb') as fh:
        data = bytearray(fh.read())
    changes = plan(bytes(data))
    for roff, symi in changes:
        struct.pack_into('<I', data, roff + 4, symi)
    if write and changes:
        with open(path, 'wb') as fh:
            fh.write(data)
    return len(changes)


def main(argv):
    report = '--report' in argv
    paths = [a for a in argv if not a.startswith('--')]
    if not paths:
        print(__doc__)
        return 2
    total = 0
    for p in paths:
        n = rebind(p, write=not report)
        total += n
        if report and n:
            print(f'{n:6d} {p}')
    if report:
        print(f'pe_rebind_relocs: {total} section-relative reference(s) to an own '
              f'global in {len(paths)} object(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
