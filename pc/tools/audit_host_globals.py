#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-check host-backed PS1 symbols against what the code accesses.

The PC build resolves data symbols into the emulated 2 MB PS1 RAM at
0x80000000 (pc/ld/psx_globals.syms via --just-symbols, typed views via the
PROVIDE()s in pc/src/*.ld).  A symbol whose NAME carries a PS1 address
(coord_buf_r, _sys_default_tpage_set_tbl0_w, D_800E2B8C_arr, srn_w_a, ...) but
that a host C file DEFINES instead is backed by a host variable of the
declared host size, detached from the PS1 RAM at that address.  That is
a defect when

  * the code reaches more of it than the host object holds (ad25a133bc:
    a 4-byte host u32 took 1,280 bytes of GsCOORDINATE2 stores), or
  * its address escapes into PS1 data or a pointer (other code then reads
    or writes the host object as if it were the PS1 one), or
  * the PS1 address holds retail data (the EXE image) or is reached by
    another name, so the two copies disagree.

For every such symbol this tool reports the PS1 address, the host size,
the console extent (the distance to the next PS1-resident symbol), and
every access the compiled objects make through relocations against the
name: the byte range of each direct load/store, `addr` when the address
is taken (lea, an immediate, a data relocation), `index` when the access
is register-indexed.  The verdict:

  UNUSED      no object references it: a dead host definition
  OVERFLOW    a direct access reaches past the host size
  ESCAPES     the address is taken or indexed and the console extent is
              larger than the host size (the extent is not bounded here)
  SHADOWS     in bounds, but the PS1 address holds non-zero retail data
              or another symbol at that address is referenced
  HOST-ONLY   in bounds, no retail data, no other name: a host scratch

Out of scope: PS1-RESIDENT symbols (resolved into PS1 RAM) that the code
accesses wider than the console cell (a native pointer in a 4-byte slot);
that is audit_ps1_pointer_records.py's class.

usage: audit_host_globals.py [--binary B] [--objdir D] [--exe PSX.EXE]
                             [--json] [--no-fail]
Exit status 1 when an OVERFLOW / ESCAPES / SHADOWS row is reported (the
dead and host-only rows are informational), unless --no-fail.
"""
import argparse
import bisect
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAM, RAM_END = 0x80000000, 0x80200000

# a PS1 address embedded in a data symbol's name, with a suffix
# (coord_buf_r, _sys_default_tpage_set_tbl0_w, D_800B9C34b, srn_w_a, D_80149B78__def)
NAMED = re.compile(r'^D[A-Za-z]*_(8[0-9A-Fa-f]{7})(?=[_A-Za-z]|$)')
EXACT = re.compile(r'^D_(8[0-9A-F]{7})$')

REG_WIDTH = {}
for _n, _w in (('rax', 8), ('eax', 4), ('ax', 2), ('al', 1), ('ah', 1)):
    REG_WIDTH[_n] = _w
for _b in 'bcd':
    REG_WIDTH['r%sx' % _b] = 8
    REG_WIDTH['e%sx' % _b] = 4
    REG_WIDTH['%sx' % _b] = 2
    REG_WIDTH['%sl' % _b] = 1
    REG_WIDTH['%sh' % _b] = 1
for _b in ('si', 'di', 'bp', 'sp'):
    REG_WIDTH['r' + _b] = 8
    REG_WIDTH['e' + _b] = 4
    REG_WIDTH[_b] = 2
    REG_WIDTH[_b + 'l'] = 1
for _i in range(8, 16):
    REG_WIDTH['r%d' % _i] = 8
    REG_WIDTH['r%dd' % _i] = 4
    REG_WIDTH['r%dw' % _i] = 2
    REG_WIDTH['r%db' % _i] = 1
SUFFIX_WIDTH = {'b': 1, 'w': 2, 'l': 4, 'q': 8}


def sh(cmd):
    return subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout


def nm_symbols(binary):
    """name -> (value, size, type) for every defined symbol."""
    out = {}
    for line in sh(['nm', '-S', '--defined-only', str(binary)]).splitlines():
        parts = line.split()
        if len(parts) == 4:
            value, size, kind, name = parts
            size = int(size, 16)
        elif len(parts) == 3:
            value, kind, name = parts
            size = 0
        else:
            continue
        out.setdefault(name, []).append((int(value, 16), size, kind))
    return out


def in_ram(v):
    return RAM <= v < RAM_END


def access_width(mnemonic, operands, slot):
    """Byte width of the memory access an x86-64 instruction makes."""
    m = mnemonic
    for pfx, w in (('movzb', 1), ('movsb', 1), ('movzw', 2), ('movsw', 2),
                   ('movslq', 4), ('cvtsi2sd', 4)):
        if m.startswith(pfx):
            return w
    if m in ('movss', 'cvtss2sd', 'addss', 'subss', 'mulss', 'divss', 'ucomiss',
             'comiss', 'movd'):
        return 4
    if m in ('movsd', 'cvtsd2ss', 'addsd', 'subsd', 'mulsd', 'divsd', 'ucomisd',
             'comisd', 'movq'):
        return 8
    if m in ('movdqa', 'movdqu', 'movaps', 'movups', 'movapd', 'movupd'):
        return 16
    others = [o for i, o in enumerate(operands) if i != slot]
    for o in others:
        r = re.fullmatch(r'%(\w+)', o)
        if r and r.group(1) in REG_WIDTH:
            return REG_WIDTH[r.group(1)]
        if r and r.group(1).startswith(('xmm',)):
            return 16
    if len(m) > 2 and m[-1] in SUFFIX_WIDTH:
        return SUFFIX_WIDTH[m[-1]]
    return 4


def split_operands(text):
    ops, depth, cur = [], 0, ''
    for ch in text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return ops


INSN = re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(\S+)?\s*(.*)$')
RELOC = re.compile(r'^\s*([0-9a-f]+): (R_X86_64_\w+)\s+([^\s+-]+)([+-]0x[0-9a-f]+)?')


FUNC_LABEL = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
# instructions whose memory operand in the destination slot is only read
READ_ONLY = ('cmp', 'test', 'ucomi', 'comi', 'bt', 'push')


def object_accesses(obj, names, detail=False):
    """[(name, kind, lo, hi)] for each .text relocation against `names`.

    With detail=True each tuple carries a fifth element, a dict with the
    enclosing function, the instruction address and text, and `store`
    (the memory operand is the AT&T destination of a writing
    instruction)."""
    insns = []          # [addr, end, mnemonic, operands, function]
    relocs = []         # (offset, type, name, addend, insn index)
    func = None
    for line in sh(['objdump', '-dr', str(obj)]).splitlines():
        f = FUNC_LABEL.match(line)
        if f:
            func = f.group(1)
            continue
        m = INSN.match(line)
        if m:
            addr = int(m.group(1), 16)
            end = addr + len(m.group(2).split())
            if m.group(3):
                if insns and insns[-1][1] > addr:
                    insns[-1][1] = addr
                insns.append([addr, end, m.group(3), m.group(4) or '', func])
            elif insns:
                # objdump wraps a long encoding onto a bytes-only line
                insns[-1][1] = max(insns[-1][1], end)
            continue
        r = RELOC.match(line)
        if r and r.group(3) in names and insns:
            relocs.append((int(r.group(1), 16), r.group(2), r.group(3),
                           int(r.group(4), 16) if r.group(4) else 0,
                           len(insns) - 1))
    PCREL = ('R_X86_64_PC32', 'R_X86_64_PLT32', 'R_X86_64_GOTPCREL',
             'R_X86_64_GOTPCRELX', 'R_X86_64_REX_GOTPCRELX')
    by_insn = defaultdict(list)
    for rel in relocs:
        by_insn[rel[4]].append(rel)
    out = []
    for i, rels in sorted(by_insn.items()):
        iaddr, iend, mnem, opstr, ifunc = insns[i]
        if i + 1 < len(insns):
            iend = insns[i + 1][0]
        ops = split_operands(opstr.split('#')[0].strip())
        mems = [j for j, o in enumerate(ops) if '(' in o or re.fullmatch(r'-?0x[0-9a-f]+', o)]
        rip = [j for j in mems if '(%rip)' in ops[j]]
        # absolute relocations land on a non-%rip displacement first, then
        # on an immediate (x86 encodes the displacement before the immediate)
        absolute = [j for j in mems if j not in rip] + \
            [j for j, o in enumerate(ops) if o.startswith('$')]
        rels_abs = sorted((r for r in rels if r[1] not in PCREL), key=lambda r: r[0])
        for rel in rels:
            off, rtype, name, addend, _i = rel
            if rtype in PCREL:
                k = addend + (iend - off)
                slot = rip[0] if rip else None
            else:
                k = addend
                n = rels_abs.index(rel)
                slot = absolute[n] if n < len(absolute) else None
            info = dict(func=ifunc, insn=iaddr, text=(mnem + ' ' + opstr).strip(),
                        store=slot is not None and slot == len(ops) - 1 and
                        not mnem.startswith(READ_ONLY) and
                        (len(ops) > 1 or mnem.startswith(('inc', 'dec', 'neg', 'not', 'set'))))
            if mnem.startswith('lea') or 'GOT' in rtype or slot is None \
                    or ops[slot].startswith('$'):
                acc = (name, 'addr', k, k)
            else:
                regs = re.search(r'\(([^)]*)\)', ops[slot])
                if regs and regs.group(1) != '%rip':
                    acc = (name, 'index', k, k)
                else:
                    acc = (name, 'rw', k, k + access_width(mnem, ops, slot))
            out.append(acc + (info,) if detail else acc)
    return out


def data_relocations(obj, names):
    """Relocations outside .text (pointer tables initialised with &sym)."""
    out = []
    section = None
    try:
        text = sh(['objdump', '-r', str(obj)])
    except subprocess.CalledProcessError:
        return out
    for line in text.splitlines():
        s = re.match(r'RELOCATION RECORDS FOR \[(.+)\]:', line)
        if s:
            section = s.group(1)
            continue
        if section is None or section.startswith('.rela.text') or \
                section.startswith('.text') or 'debug' in section or \
                section.startswith('.eh_frame'):
            continue
        r = re.match(r'^([0-9a-f]+) (R_X86_64_\w+)\s+([^\s+-]+)', line)
        if r and r.group(3) in names:
            out.append((r.group(3), 'addr', 0, 0))
    return out


def referencing_objects(objs, names):
    """Objects with an UNDEFINED or relocated reference to any of names."""
    hits = defaultdict(set)
    for obj in objs:
        try:
            text = sh(['nm', str(obj)])
        except subprocess.CalledProcessError:
            continue
        for line in text.splitlines():
            parts = line.split()
            if parts and parts[-1] in names:
                hits[obj].add(parts[-1])
    return hits


def exe_image(path):
    """(load, bytes) of a PS-X EXE, or None."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if data[:8] != b'PS-X EXE':
        return None
    load = int.from_bytes(data[0x18:0x1C], 'little')
    size = int.from_bytes(data[0x1C:0x20], 'little')
    return load, data[0x800:0x800 + size]


def retail_nonzero(exe, addr, n):
    if not exe:
        return None
    load, img = exe
    o = addr - load
    if o < 0 or o >= len(img):
        return None
    return any(img[o:o + max(n, 1)])


def audit(binary, objdir, exe_path):
    syms = nm_symbols(binary)
    resident = sorted({v for name, defs in syms.items() for v, _s, _k in defs
                       if in_ram(v)})
    resident_names = defaultdict(set)
    for name, defs in syms.items():
        for v, _s, _k in defs:
            if in_ram(v):
                resident_names[v].add(name)
    exe = exe_image(exe_path)

    rows = {}
    for name, defs in syms.items():
        m = NAMED.match(name)
        if not m or EXACT.match(name):
            continue
        addr = int(m.group(1), 16)
        if not in_ram(addr):
            continue
        for v, size, kind in defs:
            if in_ram(v):
                continue
            i = bisect.bisect_right(resident, addr)
            extent = (resident[i] - addr) if i < len(resident) else None
            rows[name] = dict(name=name, addr=addr, host=v, size=size,
                              local=kind.islower(), extent=extent,
                              accesses=[], objects=[])

    objs = sorted(Path(objdir).glob('*.o'))
    names = set(rows)
    hits = referencing_objects(objs, names)
    for obj, found in sorted(hits.items()):
        for name, kind, lo, hi in object_accesses(obj, found) + \
                data_relocations(obj, found):
            rows[name]['accesses'].append((kind, lo, hi))
            if obj.name not in rows[name]['objects']:
                rows[name]['objects'].append(obj.name)

    # other names at the same PS1 address that the code references
    referenced = set()
    if any(resident_names.get(r['addr']) for r in rows.values()):
        candidates = set()
        for r in rows.values():
            candidates |= resident_names.get(r['addr'], set())
        for obj, found in referencing_objects(objs, candidates).items():
            referenced |= found

    for r in rows.values():
        acc = r['accesses']
        others = sorted(n for n in resident_names.get(r['addr'], ())
                        if n in referenced)
        r['other_names'] = others
        r['retail_data'] = retail_nonzero(exe, r['addr'], max(r['size'], 4))
        reach = max([hi for k, lo, hi in acc if k == 'rw'], default=0)
        r['reach'] = reach
        unbounded = any(k in ('addr', 'index') for k, _lo, _hi in acc)
        r['unbounded'] = unbounded
        if not acc:
            v = 'UNUSED'
        elif reach > r['size']:
            v = 'OVERFLOW'
        elif unbounded and (r['extent'] or 0) > r['size']:
            v = 'ESCAPES'
        elif r['retail_data'] or others:
            v = 'SHADOWS'
        else:
            v = 'HOST-ONLY'
        r['verdict'] = v

    return sorted(rows.values(), key=lambda r: (r['verdict'], r['addr']))


DEFECT = ('OVERFLOW', 'ESCAPES', 'SHADOWS')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--binary', default=str(ROOT / 'pc/build/tokimeki-pc'))
    ap.add_argument('--objdir', default=str(ROOT / 'pc/build/obj'))
    ap.add_argument('--exe', default=str(ROOT / 'pc/assets/raw/PSX.EXE'))
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-fail', action='store_true')
    a = ap.parse_args(argv)
    rows = audit(a.binary, a.objdir, a.exe)
    if a.json:
        print(json.dumps(rows, indent=1, default=str))
    else:
        print('%-10s %-22s %-10s %5s %7s %6s %-5s %-6s %s' % (
            'verdict', 'symbol', 'ps1', 'host', 'extent', 'reach', 'addr?',
            'retail', 'objects / other names'))
        for r in rows:
            print('%-10s %-22s 0x%08X %5d %7s %6d %-5s %-6s %s%s' % (
                r['verdict'], r['name'] + (' (static)' if r['local'] else ''),
                r['addr'], r['size'], r['extent'], r['reach'],
                'yes' if r['unbounded'] else '-',
                {True: 'data', False: 'zero', None: '-'}[r['retail_data']],
                ','.join(r['objects']) or '-',
                (' | also ' + ','.join(r['other_names'])) if r['other_names'] else ''))
        counts = defaultdict(int)
        for r in rows:
            counts[r['verdict']] += 1
        print('\n' + ', '.join('%s %d' % kv for kv in sorted(counts.items())))
    bad = [r for r in rows if r['verdict'] in DEFECT and not r['local']]
    return 1 if bad and not a.no_fail else 0


if __name__ == '__main__':
    sys.exit(main())
