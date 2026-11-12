#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Two 64-bit pointer shapes the wide-access audit cannot see.

audit_ps1_wide_access.py finds 8-byte accesses to PS1-resident SYMBOLS (a
relocation names the cell).  Two defects reach PS1 memory with no symbol
on the access, so the objects carry no evidence of them; this audit reads
the game C itself, as the compiler types it (clang's AST of the exact
translation unit pc/Makefile compiles: same -D/-I/-include, the adapted
ps1_cells sources, the generated overlay modules):

  CELL-READ     `*(T **)(addr)` / `((T **)p)[i]` / `*(long *)...`: a
                dereference through an explicit cast to a pointer to a
                pointer (or to `long`).  The console cell is 4 bytes; the
                host reads 8 -- the save menu's album-name table
                (`*(u8 **)(card_file_save_tbl0 + offset)`, a65b35ca29).
                The address is usually computed from a 4-byte PS1 field,
                so no relocation marks it.
  CELL-MEMBER   `((Rec *)addr)->p`: a pointer/long member reached through
                an explicit cast of an address.
  RECORD-PTR    `rec->p` where `p` is a pointer/long member of a record
                type the source file defines itself (game records
                describe PS1 memory: InitSoundSeq's read/loop pointers,
                class_w slot handles).  Every later member moves too.
  SEXT-PTR      a signed 32-bit integer (s32/int/s16/s8) converted to a
                pointer, explicitly or implicitly: it sign-extends,
                0x800B9A00 -> 0xFFFFFFFF800B9A00 (the phone book's `(u8
                *)tbl` with an `s32 tbl`, 4715642f40).  Non-negative
                integer constants are exempt.

A local's address (`*(u8 **)&buf`) is host memory and is not reported.

Rows name the function and whether it wins the link (`live`, as
audit_pc_stubs.build_index sees the objects).  Gate: a live row whose
function is not in --allow (pc/tools/pointer_cast_allow.txt: `function`
or `function:SHAPE`, then `# reason`; it may only shrink) exits 1.

Census 2026-10-31 (1,724 translation units): before the fixes CELL-READ
161 rows / 26 functions, RECORD-PTR 258 / 56, SEXT-PTR 748 / 118 (live).
Fixed: the overlay generator and pc/tools/adapt_ps1_cells.py read pointer
chains and `*(T ***)` cells 4 bytes wide (CELL-READ 161 -> 8 rows: 24
functions, among them NAME_ENT's glyph table, SHUGAKU's serifu table,
serifu_cmd_exec, srn_vram_set, ENDING's ss_s tables, the save-slot names);
the mod_w / srn_w records are PS1-width overrides (mod_init, srn_init,
mod_set, mod_trans_vram) and InitSoundSep one too.  SEXT-PTR 748 -> 0:
pc/Makefile compiles every overlay module and the main-binary sources
that had a row through pc/tools/adapt_ps1_addresses.py, which makes each
such conversion zero-extend (the pc-port rows are written `(u32)` in
the overrides); pc/src/main.c's fault fold, which used to repair the
dereference only, then recovered 0 times in the playthroughs and the
matrix (from ~48,000).  RECORD-PTR, libgs geometry (2026-10-31): the
GsOT / divide work area / GsCOORDINATE / matrix-stack members of 42
functions (the GsTMDdiv* family, DivideG3, get_dstack(2), mul_dmat*,
mul_rdmat, GsInitCoordinate, GsLinkObject(2), GsSetRefView) are 4-byte
cells in the PC build (pc/tools/record_width_plan.json through
ps1_source_edits.py).  The rest is in the allow list by reason.

usage: audit_ps1_pointer_casts.py [--build-dir pc/build] [--allow FILE]
                                  [--json OUT] [--all] [--jobs N]
                                  [--source FILE ...]
Run after `make -f pc/Makefile all` (it reads the compile commands with
`make -n -B` and the objects' link order).  --source audits single files
with the game compile flags (the tests' entry point).
"""
import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_pc_stubs as stubs  # noqa: E402
from pc_paths import decomp, decomp_key, spell  # noqa: E402

ROOT = stubs.ROOT
CLANG = os.environ.get('CLANG', 'clang')
SHAPES = ('CELL-READ', 'CELL-MEMBER', 'RECORD-PTR', 'SEXT-PTR')

# gcc options clang does not take (or that change nothing for an AST)
DROP = {'-fpermissive', '-fno-aggressive-loop-optimizations', '-pg', '-mfentry',
        '-fno-tree-vectorize', '-fno-tree-loop-distribute-patterns', '-fcommon',
        '-MMD', '-MD', '-MP', '-c', '-w', '-g'}
DROP_ARG = {'-MF', '-MT', '-MQ', '-o'}
CLANG_FLAGS = ['-fsyntax-only', '-w', '-fno-color-diagnostics', '-ferror-limit=0',
               '-Wno-error=implicit-function-declaration', '-Wno-error=int-conversion',
               '-Wno-error=incompatible-pointer-types', '-Wno-error=implicit-int',
               '-Wno-error=return-type', '-Wno-error=incompatible-function-pointer-types',
               '-Wno-error=return-mismatch', '-Wno-error=deprecated-non-prototype',
               '-Xclang', '-ast-dump']
GAME_FLAGS = ['-std=gnu17', '-funsigned-char', '-D_GNU_SOURCE', '-DPC_PORT',
              '-include', 'pc/include/game/types.h', '-I', 'pc/include/game',
              '-I', 'pc/include', '-I', decomp('include'),
              '-include', 'pc/include/game/sony_bios_alias.h']

NODE_RE = re.compile(r'^([| `]*)[-`]?(\w+) (0x[0-9a-f]+)?\s*(.*)$')
LOC_RE = re.compile(r'([^\s<>,]+\.[ch]):(\d+):\d+|line:(\d+):\d+|col:\d+')
TYPE_RE = re.compile(r"'([^']*)'(?::'([^']*)')?")


def clang_argv(gcc_argv, src):
    out, skip = [CLANG], False
    for a in gcc_argv[1:]:
        if skip:
            skip = False
            continue
        if a in DROP_ARG:
            skip = True
            continue
        if a in DROP or a == src or a.endswith('.c') and not a.startswith('-'):
            continue
        out.append(a)
    return out + CLANG_FLAGS + [src]


def compile_commands(build_dir):
    """{object basename: (argv, source)} from one `make -n -B all`."""
    r = subprocess.run(['make', '-s', '-n', '-B', '-f', 'pc/Makefile',
                        'BUILD_DIR=%s' % build_dir, 'all'],
                       cwd=ROOT, capture_output=True, text=True, timeout=600)
    out = {}
    for line in r.stdout.splitlines():
        if ' -c ' not in line or ' -o ' not in line:
            continue
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        if not argv or os.path.basename(argv[0]) not in ('gcc', 'cc', 'clang'):
            continue
        obj = argv[argv.index('-o') + 1]
        src = argv[argv.index('-c') + 1]
        out[os.path.basename(obj)] = (argv, src)
    return out


# ---------------------------------------------------------------- AST walk

class Node(object):
    __slots__ = ('kind', 'id', 'text', 'type', 'dtype', 'file', 'line', 'kids', 'parent')

    def __init__(self, kind, nid, text, typ, dtype, file, line, parent):
        self.kind, self.id, self.text = kind, nid, text
        self.type, self.dtype = typ, dtype
        self.file, self.line = file, line
        self.kids, self.parent = [], parent


def parse_dump(lines):
    """Yield top-level declaration subtrees (Node) from clang's text dump."""
    stack = []
    cur_file, cur_line = None, 0
    top = None
    for raw in lines:
        m = NODE_RE.match(raw.rstrip('\n'))
        if not m or not m.group(2)[0].isupper():
            continue
        depth = (len(m.group(1)) + 1) // 2
        kind, nid, rest = m.group(2), m.group(3) or '', m.group(4)
        file_, line_ = cur_file, cur_line
        first = True
        lt = rest.find('<')
        gt = rest.find('>', lt) if lt >= 0 else -1
        locs = rest[lt + 1:gt] if lt >= 0 else ''
        # the range, then a declaration's own location after it
        tail = rest[gt + 1:] if gt >= 0 else rest
        own = re.match(r'\s*((?:[^\s<>,]+\.[ch]|line):\d+:\d+|col:\d+)', tail)
        for part in [p for p in locs.split(',')] + ([own.group(1)] if own else []):
            lm = LOC_RE.search(part)
            if not lm:
                continue
            if lm.group(1):
                cur_file, cur_line = lm.group(1), int(lm.group(2))
            elif lm.group(3):
                cur_line = int(lm.group(3))
            if first:
                file_, line_ = cur_file, cur_line
                first = False
        tm = TYPE_RE.search(tail)
        typ = tm.group(1) if tm else ''
        dtype = (tm.group(2) or tm.group(1)) if tm else ''
        while stack and stack[-1][0] >= depth:
            stack.pop()
        parent = stack[-1][1] if stack else None
        n = Node(kind, nid, tail, typ, dtype, file_, line_, parent)
        if parent is not None:
            parent.kids.append(n)
        if depth == 1:
            if top is not None:
                yield top
            top = n
        stack.append((depth, n))
    if top is not None:
        yield top


def strip(n, kinds=('ParenExpr', 'ImplicitCastExpr')):
    while n is not None and n.kind in kinds and n.kids:
        n = n.kids[0]
    return n


def wide_pointee(t):
    """True for a pointer to an object 8 bytes on LP64 and 4 on the
    console: a pointer to a pointer, to a function pointer, or to long."""
    t = t.strip()
    if not t.endswith('*'):
        return False
    inner = t[:-1].rstrip()
    inner = re.sub(r'\b(const|volatile|restrict)\b', '', inner).strip()
    if inner.endswith('*') or re.search(r'\(\*+\)\s*\(', inner):
        return True
    return re.fullmatch(r'(unsigned |signed )?long( int)?( unsigned)?|(unsigned )?long long( int)?',
                        inner) is not None


def wide_value(t):
    t = re.sub(r'\b(const|volatile|restrict)\b', '', t).strip()
    return t.endswith('*') or re.search(r'\(\*+\)\s*\(', t) is not None or \
        re.fullmatch(r'(unsigned |signed )?long( int)?|(unsigned )?long long( int)?', t) is not None


SIGNED32 = {'int', 'signed int', 'short', 'signed short', 'short int', 'signed char'}


def literal_value(n):
    n = strip(n)
    if n is None:
        return None
    if n.kind == 'IntegerLiteral':
        try:
            return int(n.text.split()[-1], 0)
        except ValueError:
            return None
    if n.kind == 'CharacterLiteral':
        return 0
    if n.kind == 'UnaryOperator' and "'-'" in n.text and n.kids:
        v = literal_value(n.kids[0])
        return -v if v is not None else None
    return None


def addr_of_local(n, locals_):
    n = strip(n)
    if n is not None and n.kind == 'UnaryOperator' and "prefix '&'" in n.text and n.kids:
        k = strip(n.kids[0], ('ParenExpr',))
        while k is not None and k.kind in ('ArraySubscriptExpr', 'MemberExpr') and k.kids:
            k = strip(k.kids[0])
        if k is not None and k.kind == 'DeclRefExpr':
            m = re.search(r'(?:Var|ParmVar) (0x[0-9a-f]+)', k.text)
            return bool(m and m.group(1) in locals_)
    if n is not None and n.kind == 'DeclRefExpr':     # a local array decays
        m = re.search(r'Var (0x[0-9a-f]+)', n.text)
        return bool(m and m.group(1) in locals_ and '[' in n.type)
    return False


def walk(n):
    stack = [n]
    while stack:
        x = stack.pop()
        yield x
        stack.extend(reversed(x.kids))


def scan_tree(top, main_file, records):
    """Rows for one top-level declaration (a FunctionDecl)."""
    rows = []
    if top.kind != 'FunctionDecl' or not any(k.kind == 'CompoundStmt' for k in top.kids):
        return rows
    fm = re.search(r"(\w+) '", top.text)
    func = fm.group(1) if fm else '?'
    locals_ = {x.id for x in walk(top) if x.kind in ('VarDecl', 'ParmVarDecl')
               and not re.search(r'\bextern\b', x.text)}

    def row(shape, n, detail):
        if n.file and os.path.normpath(n.file) != os.path.normpath(main_file):
            return
        rows.append(dict(shape=shape, func=func, file=n.file, line=n.line, detail=detail))

    for n in walk(top):
        if n.kind in ('CStyleCastExpr', 'ImplicitCastExpr') and '<IntegralToPointer>' in n.text \
                and n.kids:
            src = n.kids[0]
            st = re.sub(r'\b(const|volatile)\b', '', src.dtype).strip()
            if st in SIGNED32:
                v = literal_value(src)
                if v is None or v < 0:
                    row('SEXT-PTR', n, "(%s) from '%s'" % (n.type, src.type))
        elif (n.kind == 'UnaryOperator' and "prefix '*'" in n.text) or \
                n.kind == 'ArraySubscriptExpr':
            for k in n.kids[:2]:
                c = strip(k)
                if c is not None and c.kind == 'CStyleCastExpr' and wide_pointee(c.dtype) \
                        and not addr_of_local(c.kids[0] if c.kids else None, locals_):
                    row('CELL-READ', n, "through (%s)" % c.type)
                    break
        elif n.kind == 'MemberExpr' and n.kids and wide_value(n.dtype):
            arrow = re.search(r"lvalue (->|\.)(\w+) (0x[0-9a-f]+)", n.text) or \
                re.search(r" (->|\.)(\w+) (0x[0-9a-f]+)", n.text)
            if not arrow:
                continue
            base = strip(n.kids[0])
            if base is not None and base.kind == 'CStyleCastExpr' and arrow.group(1) == '->' \
                    and not addr_of_local(base.kids[0] if base.kids else None, locals_):
                row('CELL-MEMBER', n, '((%s)...)->%s' % (base.type, arrow.group(2)))
            elif arrow.group(1) == '->' and arrow.group(3) in records:
                row('RECORD-PTR', n, '->%s (%s, record %s)' % (
                    arrow.group(2), n.type, records[arrow.group(3)]))
    return rows


def scan_source(argv, src, main_file=None):
    """Rows for one translation unit; (rows, error)."""
    main_file = main_file or src
    try:
        p = subprocess.Popen(clang_argv(argv, src), cwd=ROOT, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, errors='replace')
    except OSError as ex:
        return [], str(ex)
    rows, records = [], {}
    rec_stack = []
    try:
        for top in parse_dump(p.stdout):
            # records defined in the source file itself: their pointer/long
            # fields (a FieldDecl's id is what a MemberExpr names)
            for x in walk(top):
                if x.kind == 'RecordDecl' and x.file and \
                        os.path.normpath(x.file) == os.path.normpath(main_file):
                    name = re.search(r"\b(?:struct|union) (\w+) definition", x.text)
                    for f in x.kids:
                        if f.kind == 'FieldDecl' and wide_value(f.dtype):
                            records[f.id] = name.group(1) if name else 'line %d' % x.line
            rows += scan_tree(top, main_file, records)
    finally:
        p.stdout.close()
        err = p.stderr.read()
        p.stderr.close()
        p.wait()
    # rows name files repository-relative (pc_paths.decomp_key): the same
    # spelling with toki1-decomp as a sibling checkout as in the monorepo
    for r in rows:
        if r['file']:
            r['file'] = decomp_key(r['file'])
    if p.returncode not in (0, 1):
        return rows, err[-400:]
    # clang keeps the body of a function whose definition conflicts with an
    # earlier implicit declaration (`conflicting types for`: gcc accepts it),
    # so that error alone does not lose rows; any other error may have
    errs = [l for l in err.splitlines() if ': error:' in l
            and 'conflicting types for' not in l]
    if p.returncode == 1 and errs:
        return rows, '\n'.join(errs)[-400:]
    return rows, None


# ---------------------------------------------------------------- driver

GAME_TIERS = ('matched', 'semi', 'archive', 'pc-port', 'overlay')


def tier(src):
    if 'ps1_cells/' in src or 'ps1_addr/' in src or 'ps1_edit/' in src:
        # a build-time adapted copy (adapt_ps1_cells.py /
        # adapt_ps1_addresses.py / ps1_source_edits.py): the tier of the file
        # its #line names
        orig = first_line_target(src)
        return tier(orig) if orig != src else 'matched'
    src = decomp_key(src)
    if src.startswith('src/main/pc-port/'):
        return 'pc-port'
    if src.startswith('src/main/matched/'):
        return 'matched'
    if src.startswith('src/main/semi-matched/'):
        return 'semi'
    if src.startswith('src/main/archive/'):
        return 'archive'
    if re.match(r'pc/src/overlay_[A-Z0-9_]+\.c$', src):
        return 'overlay'
    return None


def first_line_target(src):
    """The file a `#line 1 "X"` at the top of a generated source names."""
    try:
        with open(os.path.join(ROOT, spell(src))) as fh:
            m = re.match(r'#line 1 "([^"]+)"', fh.readline())
            return m.group(1) if m else src
    except OSError:
        return src


def audit(build_dir='pc/build', jobs=8):
    order, _srcmap, _tiers, defs, _undef = stubs.build_index(build_dir)
    cmds = compile_commands(build_dir)
    winner = {}
    for o in order:
        b = os.path.basename(o)
        for typ, name in defs.get(b, []):
            if typ in 'TW' and (name not in winner or (typ == 'T' and winner[name][1] != 'T')):
                if name not in winner or winner[name][1] != 'T':
                    winner[name] = (b, typ)
    work = []
    for o in order:
        b = os.path.basename(o)
        if b not in cmds:
            continue
        argv, src = cmds[b]
        t = tier(src)
        if t in GAME_TIERS:
            work.append((b, argv, src, t))

    def one(item):
        b, argv, src, t = item
        rows, err = scan_source(argv, src, first_line_target(src))
        for r in rows:
            r.update(obj=b, src=decomp_key(first_line_target(src)), tier=t,
                     live=winner.get(r['func'], (None,))[0] == b)
        return rows, (b, err) if err else None
    rows, errors = [], []
    with ThreadPoolExecutor(jobs) as ex:
        for rs, e in ex.map(one, work):
            rows += rs
            if e:
                errors.append(e)
    rows.sort(key=lambda r: (r['shape'], not r['live'], r['func'], r['file'] or '', r['line']))
    return rows, errors, len(work)


def load_allow(path):
    out = {}
    if path and os.path.exists(path):
        for line in open(path):
            body = line.split('#')[0].strip()
            if body:
                out[body] = line.split('#', 1)[1].strip() if '#' in line else ''
    return out


def allowed(r, allow):
    return r['func'] in allow or '%s:%s' % (r['func'], r['shape']) in allow


def source_line(r, cache={}):
    path = r.get('file') or ''
    if path not in cache:
        try:
            cache[path] = open(os.path.join(ROOT, spell(path)), errors='replace').read().splitlines()
        except OSError:
            cache[path] = []
    lines = cache[path]
    return lines[r['line'] - 1].strip() if 0 < r['line'] <= len(lines) else ''


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default='pc/build')
    ap.add_argument('--allow', default=os.path.join(ROOT, 'pc/tools/pointer_cast_allow.txt'))
    ap.add_argument('--json')
    ap.add_argument('--all', action='store_true', help='also print dead and allowed rows')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--source', nargs='+', help='audit these files with the game flags')
    a = ap.parse_args(argv)

    if a.source:
        bad = 0
        for s in a.source:
            rows, err = scan_source(['gcc'] + GAME_FLAGS, s)
            if err:
                print('%s: clang failed: %s' % (s, err), file=sys.stderr)
                bad = 1
            for r in rows:
                print('%-11s %s:%d %s  %s' % (r['shape'], r['file'], r['line'], r['func'],
                                            r['detail']))
            bad |= bool(rows)
        return 1 if bad else 0

    rows, errors, n = audit(a.build_dir, a.jobs)
    allow = load_allow(a.allow)
    gated = [r for r in rows if r['live'] and not allowed(r, allow)]
    shown = rows if a.all else gated
    for r in shown:
        print('%-11s %-5s %-34s %s:%d  %s | %s' % (
            r['shape'], 'live' if r['live'] else 'dead', r['func'], r['file'], r['line'],
            r['detail'], source_line(r)[:90]))
    for b, e in errors:
        print('CLANG-FAILED %s: %s' % (b, ' '.join(e.split())[:200]), file=sys.stderr)
    used = {k for k in allow if any(allowed(r, {k: 1}) for r in rows if r['live'])}
    stale = sorted(set(allow) - used)
    for k in stale:
        print('STALE-ALLOW %s (no live row; delete the line)' % k)
    c = collections.Counter((r['shape'], r['live']) for r in rows)
    print('%d translation units; rows: %s; gated %d; clang failures %d' % (
        n, ', '.join('%s %s %d' % (s, 'live' if l else 'dead', v)
                     for (s, l), v in sorted(c.items())), len(gated), len(errors)))
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)
    return 1 if gated or errors or stale else 0


if __name__ == '__main__':
    sys.exit(main())
