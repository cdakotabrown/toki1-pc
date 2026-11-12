#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Main-binary game sources with PS1-width pointer tables (PC build step).

The console keeps some tables of pointers as 4-byte cells: `_ss_score`
(0x80131380, libsnd's score record per sequence; SsSetTableSize writes
it 4 bytes per entry).  Matched C declares them `extern T *name[]`, which
on LP64 indexes 8 bytes per entry and reads two cells as one pointer.

pc/Makefile compiles every game source (matched / semi-matched / archive)
that declares one of PS1_POINTER_TABLES through this filter: each
`name[i]` access becomes a 4-byte cell read, zero-extended into the PS1
RAM window (stores narrow it back) -- the rewrite the overlay module
generator applies to every overlay's extern pointer arrays
(ps1_pointer_cells.adapt_pointer_cells).  The matched C is not edited;
the output starts with a #line directive naming it.

With --chains (pc/Makefile PS1_CHAIN_SRCS, the main-binary sources
audit_ps1_pointer_casts.py reports as CELL-READ) it also reads every
`((T **)x)[i]` / `*(T **)(addr)` through a 4-byte cell.

usage: adapt_ps1_cells.py [--tables NAME[,NAME...]] [--chains] SRC OUT
A source that declares a listed table but has no access left to rewrite,
or one the adapter cannot express (`&name[i]`), fails the build.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps1_pointer_cells import adapt_chains, adapt_pointer_cells, close_bracket, mask  # noqa: E402


def _edit(text, edits):
    for a, b, rep in sorted(edits, reverse=True):
        text = text[:a] + rep + text[b:]
    return text


def adapt_cell_locals(text, table):
    """The `T **v = &table[i]` shape: a local that holds a CELL address and
    is only ever dereferenced (`*v`, or `*v[0]` for `T **v[1]`).  It
    becomes a `u32 *` over the 4-byte cell and each `*v` reads the cell,
    widened.  Any other use of the local (v++, v[k], passing it on) raises:
    that needs a PC override."""
    code = mask(text)
    addr_re = re.compile(r'&\s*' + re.escape(table) + r'\s*\[')
    names = set()
    for m in addr_re.finditer(code):
        tail = code[close_bracket(code, m.end()) + 1:].lstrip()
        if tail.startswith(('[', '.', '->')):
            continue                       # &table[i][j]: an element address
        head = code[max(0, code.rfind('\n', 0, m.start())):m.start()]
        g = re.search(r'(?:\*\s*\*\s*)?\b(\w+)\s*(\[\s*0\s*\])?\s*=\s*$', head)
        if not g:
            raise ValueError('&%s[i] not assigned to a local' % table)
        names.add(g.group(1))
    edits = []
    for v in sorted(names):
        decl = re.search(r'^(\s*)((?:(?:const|volatile)\s+)*(?:struct\s+)?\w+)\s*\*\s*\*\s*'
                         + re.escape(v) + r'\b(\s*\[\s*1\s*\])?', code, re.M)
        if not decl:
            raise ValueError('cell-address local %s: no `T **%s` declaration' % (v, v))
        typ, arr = decl.group(2), decl.group(3)
        edits.append((decl.start(2), decl.end(3) if arr else decl.end(),
                      'u32 *%s%s' % (v, arr or '')))
        sub = r'\s*\[\s*0\s*\]' if arr else ''
        use = re.compile(r'\b' + re.escape(v) + r'\b')
        for m in use.finditer(code):
            in_decl = decl.start() <= m.start() < decl.end()
            after = code[m.end():]
            sm = re.match(sub, after) if arr and not in_decl else None
            if in_decl:
                if arr:
                    continue
            elif arr and not sm:
                raise ValueError('cell-address local %s used as %s' % (v, after[:12]))
            end = m.end() + (sm.end() if sm else 0)
            rest = code[end:].lstrip()
            before = code[:m.start()].rstrip()
            if rest.startswith('=') and not rest.startswith('=='):
                # the assignment: &table[i] -> the cell's address
                am = addr_re.match(code, end + (len(code[end:]) - len(rest)) + 1
                                   + (len(rest[1:]) - len(rest[1:].lstrip())))
                if not am and in_decl:
                    continue
                if not am:
                    raise ValueError('cell-address local %s assigned from %s' % (v, rest[:20]))
                close = close_bracket(code, am.end())
                edits.append((am.start(), close + 1, '((u32 *)%s + (%s))'
                              % (table, text[am.end():close])))
                continue
            if in_decl:
                continue
            if before.endswith('*') and not before.endswith('**') and \
                    not rest.startswith(('++', '--', '[', '=')):
                star = len(before) - 1
                edits.append((star, end, '((%s *)(unsigned long)*%s)' % (typ, text[m.start():end])))
                continue
            raise ValueError('cell-address local %s used as `%s`' % (
                v, code[max(0, m.start() - 8):end + 8].strip()))
    return _edit(text, edits)


def adapt(text, tables, chains=False):
    for t in tables:
        text = adapt_cell_locals(text, t)
    out = adapt_pointer_cells(text, tables=tables, cells=False)
    if chains:
        # `((T **)x)[i]` chains and `*(T **)(addr)` cells (the overlay
        # generator's rewrites; audit_ps1_pointer_casts.py CELL-READ)
        out = adapt_pointer_cells(adapt_chains(out), tables=(), cells=True)
    left = [m for t in tables for m in re.finditer(
        r'^(?!\s*extern\b).*\b' + re.escape(t) + r'\s*\[', mask(out), re.M)]
    if left:
        raise ValueError('%d %s access(es) not rewritten' % (len(left), tables))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tables', default='')
    ap.add_argument('--chains', action='store_true',
                    help='also read pointer chains / *(T **) casts as cells')
    ap.add_argument('src')
    ap.add_argument('out')
    a = ap.parse_args(argv)
    tables = [t for t in a.tables.split(',') if t]
    text = open(a.src, encoding='utf-8', errors='replace').read()
    try:
        out = adapt(text, tables, a.chains)
    except ValueError as ex:
        print('adapt_ps1_cells: %s: %s' % (a.src, ex), file=sys.stderr)
        return 1
    tmp = a.out + '.tmp'
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    with open(tmp, 'w', encoding='utf-8') as f:
        # an input from an earlier filter stage already names the source
        lead = '' if re.match(r'#line 1 "[^"]+"\n', out) else '#line 1 "%s"\n' % a.src
        f.write(lead + out)
    os.replace(tmp, a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
