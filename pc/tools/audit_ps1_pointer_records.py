#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""List PS1-layout records that the PC build compiles with pointer members.

A record whose layout is fixed by the console (a class_w slot, a
screen_w block, a table in the data blob) and that declares a member as a
native pointer changes size and member offsets on LP64: the pointer is 4
bytes on the console and 8 (8-aligned) on the host.  When such a record is
used to WRITE PS1 memory, the PC port stores at the wrong offsets and
strides (src/main/pc-port/basyo_init.c, pc/include/game/class_slot.h).

Static screen over every source the PC build compiles (the obj .srcmap, or
the pc-port > matched > semi tiers if no build exists).  For each
struct/union definition with a pointer member it reports the PS1-layout
evidence it can see:

  ps1-global   the record types an object that lives at a PS1 address:
               `extern T sym` where sym is class_w / screen_w / a symbol
               in pc/src/layout_aliases.ld / ps1_addr_aliases.c /
               globals_data.h, or `(T *)` applied to one of them
  offsets      offset comments (`/* +0x1C */`, `/* 12: */`, `/* 0x800E... */`)
  pads         explicit padding members (padN, unkN, u8 x[N] fillers)
  writes       the record's members are assigned through such an object

Rows with ps1-global AND writes are the defect class; the rest need a
reader to decide.  Records the pc-port tier already overrides with a
PS1-width view are reported only if they are still compiled.

usage: audit_ps1_pointer_records.py [--srcmap PATH] [--all]
"""
import argparse
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp_key, spell  # noqa: E402

STRUCT = re.compile(
    r'(typedef\s+)?(struct|union)\s*(\w+)?\s*\{((?:[^{}]|\{(?:[^{}]|\{[^{}]*\})*\})*)\}\s*([^;]*);')
COMMENT = re.compile(r'/\*.*?\*/', re.S)
OFFSET_COMMENT = re.compile(r'/\*\s*(?:\+?0x[0-9A-Fa-f]+|\+\d+|\d+\s*:|\d+\s*\*/|0x80[0-9A-Fa-f]{6})')
PAD = re.compile(r'\b(?:pad|unk|_pad|filler|field_)\w*\s*(?:\[[^\]]*\])?\s*;|\bu8\s+\w+\s*\[\s*\d+\s*\]\s*;')


def ps1_symbols():
    syms = {'class_w', 'screen_w'}
    for p in (ROOT / 'pc/src').glob('*.ld'):
        syms |= set(re.findall(r'^\s*(\w+)\s*=', p.read_text(errors='replace'), re.M))
    a = ROOT / 'pc/src/ps1_addr_aliases.c'
    if a.exists():
        syms |= set(re.findall(r'\.set\s+(\w+),', a.read_text()))
    g = ROOT / 'pc/include/game/globals_data.h'
    if g.exists():
        syms |= set(re.findall(r'#define\s+(\w+)\s+\(', g.read_text()))
    return syms


def is_ps1(sym, ps1):
    """A symbol at a console address: aliased/blob data or a D_8xxxxxxx."""
    return sym in ps1 or re.match(r'D_(?:byte_|word_)?8[0-9A-Fa-f]{7}', sym) is not None


def sources(srcmap):
    if srcmap and Path(srcmap).exists():
        out = []
        for line in Path(srcmap).read_text().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1].endswith('.c'):
                out.append(decomp_key(parts[1]))
        return out
    tiers = ['src/main/pc-port', 'src/main/matched', 'src/main/semi-matched']
    seen, out = set(), []
    for t in tiers:
        for p in sorted((ROOT / spell(t)).glob('*.c')):
            if p.name not in seen:
                seen.add(p.name)
                out.append('%s/%s' % (t, p.name))
    out += [str(p.relative_to(ROOT)) for p in sorted((ROOT / 'pc/src').glob('overlay_*.c'))]
    return out


def pointer_members(body):
    body_nc = COMMENT.sub('', body)
    # flatten nested aggregates to their member names
    members = []
    for decl in body_nc.split(';'):
        decl = ' '.join(decl.split())
        if not decl or decl.startswith('}'):
            decl = decl.lstrip('} ')
        if '*' in decl and '(' not in decl.split('*')[0]:
            m = re.search(r'\*+\s*(?:const\s+)?(\w+)\s*(\[[^\]]*\])?\s*$', decl)
            if m:
                members.append(m.group(1))
                continue
            m = re.search(r'\(\s*\*\s*(\w+)\s*\)', decl)
            if m:
                members.append(m.group(1) + '()')
        elif re.search(r'\(\s*\*\s*\w+\s*\)\s*\(', decl):
            members.append(re.search(r'\(\s*\*\s*(\w+)', decl).group(1) + '()')
    return members


def audit(files, ps1, show_all):
    rows = []
    for f in files:
        try:
            text = (ROOT / spell(f)).read_text(errors='replace')
        except OSError:
            continue
        for m in STRUCT.finditer(text):
            typedef, kind, tag, body, names = m.groups()
            ptrs = pointer_members(body)
            if not ptrs:
                continue
            tnames = [n.strip().lstrip('*').strip() for n in names.split(',') if n.strip()] \
                if typedef else []
            tnames = [re.match(r'\w+', n).group(0) for n in tnames if re.match(r'\w+', n)]
            if tag:
                tnames.append('struct ' + tag)
            # objects declared inline: `struct {..} sym;` without typedef
            inline_objs = [] if typedef else \
                [re.match(r'\**\s*(\w+)', n.strip()).group(1)
                 for n in names.split(',') if re.match(r'\**\s*\w', n.strip())]
            objs = set(inline_objs) & ps1
            typed_views = set()
            for t in tnames:
                tre = re.escape(t).replace(r'\ ', r'\s+')
                for om in re.finditer(r'\bextern\s+(?:volatile\s+)?%s\s+\**\s*(\w+)' % tre, text):
                    if is_ps1(om.group(1), ps1):
                        objs.add(om.group(1))
                for om in re.finditer(r'\(\s*%s\s*\*\s*\)\s*&?\s*\(?\s*(\w+)' % tre, text):
                    if is_ps1(om.group(1), ps1):
                        objs.add(om.group(1))
                # generate_overlay_modules.py typed views:
                #   typedef T D_80128B30__t1[];
                #   #define D_80128B30__v1 (*(D_80128B30__t1 *)&cw96_active)
                for om in re.finditer(r'typedef\s+%s\s+(\w+?)__t(\d+)\b' % tre, text):
                    if is_ps1(om.group(1), ps1):
                        objs.add(om.group(1))
                        typed_views.add('%s__v%s' % om.groups())
                for om in re.finditer(r'\(\s*%s\s*\*\s*\)\s*(0x[0-9A-Fa-f]{8})' % tre, text):
                    objs.add(om.group(1))
            # a view of this record type stands for its object; other views
            # of the same object (other record types) do not
            views = sorted(typed_views)
            direct = [o for o in objs if not any(v.startswith(o + '__v') for v in views)]
            writes = []
            for o in direct + views:
                for p in ptrs:
                    pn = p.rstrip('()')
                    if re.search(r'\b%s\b[^;=]*?(?:\.|->)%s\s*=[^=]' % (re.escape(o), re.escape(pn)), text):
                        writes.append('%s.%s' % (o, pn))
            any_write = bool(objs) and bool(re.search(
                r'\b(?:%s)\b[^;=]*?(?:\.|->)\w+\s*(?:[-+*/|&^]?=)[^=]' %
                '|'.join(map(re.escape, direct + views)), text))
            sig = []
            if objs:
                sig.append('ps1-global:' + ','.join(sorted(objs)))
            if OFFSET_COMMENT.search(body):
                sig.append('offsets')
            if PAD.search(COMMENT.sub('', body)):
                sig.append('pads')
            if writes:
                sig.append('ptr-writes:' + ','.join(sorted(writes)))
            elif any_write:
                sig.append('writes')
            if not sig and not show_all:
                continue
            line = text.count('\n', 0, m.start()) + 1
            rows.append((f, line, (tnames or inline_objs or ['?'])[0], ptrs, sig))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--srcmap', default=str(ROOT / 'pc/build/obj/.srcmap'))
    ap.add_argument('--all', action='store_true', help='also rows with no evidence')
    a = ap.parse_args()
    ps1 = ps1_symbols()
    rows = audit(sources(a.srcmap), ps1, a.all)
    rank = lambda r: (-sum(s.startswith(('ptr-writes', 'ps1-global')) for s in r[4]) * 2
                      - len(r[4]), r[0], r[1])
    for f, line, name, ptrs, sig in sorted(rows, key=rank):
        print('%s:%d %s  ptr=[%s]  %s' % (f, line, name, ','.join(ptrs), ' '.join(sig)))
    print('%d record(s)' % len(rows), file=sys.stderr)


if __name__ == '__main__':
    main()
