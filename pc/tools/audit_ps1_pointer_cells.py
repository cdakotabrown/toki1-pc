#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS1 pointer tables walked through a host `T **`: 8-byte cells over 4.

The console's pointer cells are 4 bytes.  On LP64 a dereference through a
`T **` (or a `long *`, or a pointer to a function pointer) reads, writes
and strides 8.  The other audits see this only where the access names the
PS1 memory itself: audit_ps1_wide_access.py an access relocated against a
PS1-resident symbol, audit_ps1_pointer_casts.py CELL-READ a dereference
through an explicit `(T **)` cast.  Three defects of 2026-11-01 had
neither (d87ec505f4, 108fd1e22f):

  ENDING ending_serifu_cmd   `s32 ***tbl` (K&R) receives sss_so, an s32 PS1
                             address, through the generator's `((s32
                             (*)())f)` view: tbl[row][col][0] read two
                             console cells as one pointer at each level.
  DATE2 date2_serifu_prefix_set
                             `pp = (u8 **)(unsigned long)*(u32 *)(...)`
                             then `p = *pp`: the cast is on the assignment,
                             the 8-byte read on a later line.
  TACO taco_fan_disp         `Prim **out` receives taco_burst_disp's `s32
                             buf[40]`: forty 8-byte stores into 160 bytes
                             (a stack-protector abort).

This audit reads the game C as the compiler types it (clang's AST of the
exact translation unit pc/Makefile compiles, like audit_ps1_pointer_casts.
py) and follows each pointer variable's value back to where it came from:
its initialiser and every assignment in the function, and -- for a
parameter -- the argument of every call of the function in the whole
build (calls through the generator's casts included), to a fixed point.
A value's origin is labelled

  INT     an integer converted to a pointer (a PS1 address: a cast, or an
          integer argument handed to an unprototyped / K&R parameter)
  PS1     a PS1-resident global (its address or its value), or a pointer
          loaded from a cell of an INT / PS1 table (the next level of a
          chain: a PS1 table's cells hold PS1 addresses)
  NARROW  a pointer to 4-byte-or-narrower elements reinterpreted as a
          pointer to pointers (`(T **)u8p`, or an `s32 buf[N]` handed to a
          `T **` parameter): the buffer's cells are the console's width

and every dereference (`*p`, `p[i]`, `&p[i]`) of a base whose type has an
8-byte pointee is a row when the base can carry one of them:

  PS1-CELL     INT or PS1: a PS1 table read / written 8 bytes wide
  NARROW-CELL  NARROW: a 4-byte-cell buffer walked as 8-byte cells

Not reported here (the other audits' rows): a base that is itself an
explicit wide cast (CELL-READ) or a PS1-resident array named directly
(wide_access WIDE-INDEX).  Origins this audit cannot see (a call's result,
a record member, a parameter only reached through a function pointer) give
no label, so the audit can miss a defect; it does not invent one.

Rows name the function and whether it wins the link (`live`).  Gate: a
live row whose function is not in --allow (pc/tools/pointer_cell_allow.txt:
`function` or `function:SHAPE`, then `# reason`; it may only shrink) exits
1, as does a stale allow line or a clang failure.

usage: audit_ps1_pointer_cells.py [--build-dir pc/build] [--allow FILE]
                                  [--json OUT] [--all] [--jobs N]
                                  [--source FILE ...]
Run after `make -f pc/Makefile all`.  --source audits single files with
the game compile flags, every extern treated as PS1-resident and the calls
resolved among the given files (the tests' entry point).
"""
import argparse
import collections
import itertools
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_pc_stubs as stubs  # noqa: E402
import audit_ps1_pointer_casts as apc  # noqa: E402
from pc_paths import decomp_key  # noqa: E402

ROOT = stubs.ROOT
SHAPES = ('PS1-CELL', 'NARROW-CELL')
BAD = ('INT', 'PS1', 'NARROW')

FUNC_REF = re.compile(r"\bFunction (0x[0-9a-f]+) '(\w+)'")
VAR_REF = re.compile(r"\b(Var|ParmVar) (0x[0-9a-f]+) '(\w+)'")
INTEGER = re.compile(r'(?:(?:unsigned|signed|const|volatile)\s+)*'
                     r'(?:char|short|int|long|long long|short int|long int|long long int|'
                     r'unsigned|signed)(?:\s+(?:int|unsigned|signed))*$')


def clean(t):
    return re.sub(r'\b(const|volatile|restrict)\b', '', t or '').strip()


def is_pointer(t):
    t = clean(t)
    return t.endswith('*') or bool(re.search(r'\(\*+\)\s*\(', t))


def is_integer(t):
    return bool(INTEGER.match(clean(t))) or clean(t) in ('_Bool',)


def narrow_pointee(t):
    """A pointer to a non-pointer element of at most 4 bytes on both
    machines (char / short / int and their unsigned kinds)."""
    t = clean(t)
    if not t.endswith('*'):
        return False
    inner = clean(t[:-1])
    return bool(INTEGER.match(inner)) and 'long' not in inner


def array_element(t):
    m = re.match(r'(.*?)\s*\[\d*\]', clean(t))
    return m.group(1).strip() if m else None


# ------------------------------------------------------------- one unit

class Unit(object):
    """Facts of one translation unit, plain data (returned by a worker)."""

    def __init__(self, obj):
        self.obj = obj
        self.defs = {}       # name -> dict(params=[(name, type)], file, line)
        self.nodes = {}      # key -> dict(labels={label: evidence}, deps=[key], chain)
        self.args = []       # dict(callee, idx, key, argtype, evidence)
        self.derefs = []     # dict(func, file, line, key, base, op)


def _expr_kids(n):
    return [k for k in n.kids if k.kind.endswith(('Expr', 'Operator', 'Literal'))]


class Scanner(object):
    def __init__(self, unit, main_file):
        self.u = unit
        self.main = main_file
        self.anon = itertools.count()

    def norm(self, t):
        """t with typedef names at its head replaced (s32 * -> int *)."""
        t = clean(t)
        for _ in range(8):
            m = re.match(r'(\w+)\b(.*)$', t)
            if not m or m.group(1) not in self.typedefs:
                break
            t = clean(self.typedefs[m.group(1)] + m.group(2))
        return t

    def new_node(self, labels=None, deps=None, chain=False):
        key = ('A', self.u.obj, next(self.anon))
        self.u.nodes[key] = dict(labels=dict(labels or {}), deps=list(deps or []), chain=chain)
        return key

    def var_key(self, func, vid):
        if vid in self.params:
            return ('P', func, self.params[vid])
        return ('L', self.u.obj, func, vid)

    def node(self, key):
        return self.u.nodes.setdefault(key, dict(labels={}, deps=[], chain=False))

    # classify(E) -> (labels, deps)
    def classify(self, n, func):
        labels, deps = {}, []
        n = apc.strip(n, ('ParenExpr',))
        if n is None:
            return labels, deps
        where = '%s:%s' % (os.path.basename(n.file or '?'), n.line)
        k = n.kind
        if k in ('ImplicitCastExpr', 'CStyleCastExpr') and n.kids:
            child = n.kids[0]
            if '<NullToPointer>' in n.text:
                return labels, deps
            if '<IntegralToPointer>' in n.text:
                v = apc.literal_value(child)
                if v is not None and v == 0:
                    return labels, deps
                labels['INT'] = "(%s) of '%s' at %s" % (n.type, child.type, where)
                return labels, deps
            if '<BitCast>' in n.text and is_pointer(n.dtype) and \
                    apc.wide_pointee(self.norm(n.dtype)) and narrow_pointee(self.norm(child.dtype)):
                l2, d2 = self.classify(child, func)
                labels.update(l2)
                labels.setdefault('NARROW', "(%s) of '%s' at %s" % (n.type, child.type, where))
                return labels, deps + d2
            if '<ArrayToPointerDecay>' in n.text:
                c = apc.strip(child, ('ParenExpr',))
                if c is not None and c.kind == 'DeclRefExpr':
                    m = VAR_REF.search(c.text)
                    if m and m.group(2) in self.globals_:
                        labels['G:' + m.group(3)] = 'the array %s at %s' % (m.group(3), where)
                        return labels, deps
                    if m:
                        labels['HOST'] = 'the local array %s' % m.group(3)
                        return labels, deps
                return self.classify(child, func)
            return self.classify(child, func)
        if k == 'DeclRefExpr':
            m = VAR_REF.search(n.text)
            if not m:
                return labels, deps
            vid, name = m.group(2), m.group(3)
            if vid in self.globals_:
                if is_pointer(n.dtype):
                    labels['G:' + name] = 'the value of %s at %s' % (name, where)
                return labels, deps
            deps.append(self.var_key(func, vid))
            return labels, deps
        if k == 'UnaryOperator' and n.kids:
            if "prefix '&'" in n.text:
                c = apc.strip(n.kids[0], ('ParenExpr',))
                while c is not None and c.kind in ('ArraySubscriptExpr', 'MemberExpr') and c.kids:
                    c = apc.strip(c.kids[0])
                if c is not None and c.kind == 'DeclRefExpr':
                    m = VAR_REF.search(c.text)
                    if m and m.group(2) in self.globals_:
                        labels['G:' + m.group(3)] = '&%s at %s' % (m.group(3), where)
                    elif m:
                        labels['HOST'] = '&%s' % m.group(3)
                return labels, deps
            if "'++'" in n.text or "'--'" in n.text:
                return self.classify(n.kids[0], func)
            if "prefix '*'" in n.text:
                return self.chain(n.kids[0], func)
            return labels, deps
        if k == 'ArraySubscriptExpr' and len(n.kids) >= 2:
            base = n.kids[0] if is_pointer(n.kids[0].dtype) else n.kids[1]
            return self.chain(base, func)
        if k in ('BinaryOperator', 'CompoundAssignOperator') and len(n.kids) == 2:
            text = n.text
            if re.search(r"'(\+|-|\+=|-=)'", text):
                for c in n.kids:
                    if is_pointer(c.dtype):
                        return self.classify(c, func)
            if re.search(r"'='", text) or re.search(r"','", text):
                return self.classify(n.kids[1], func)
            return labels, deps
        if k in ('ConditionalOperator', 'BinaryConditionalOperator'):
            for c in _expr_kids(n)[1:]:
                l2, d2 = self.classify(c, func)
                labels.update(l2)
                deps += d2
            return labels, deps
        if k == 'StringLiteral':
            labels['HOST'] = 'a string literal'
        return labels, deps

    def chain(self, base, func):
        """The value loaded from a cell of `base`: a pointer read from an
        INT / PS1 table is a PS1 address."""
        l2, d2 = self.classify(base, func)
        key = self.new_node(l2, d2, chain=True)
        return {}, [key]

    def scan(self, top):
        u = self.u
        if top.kind == 'TypedefDecl':
            m = re.search(r"(\w+) '([^']*)'(?::'([^']*)')?", top.text)
            if m:
                self.typedefs[m.group(1)] = m.group(3) or m.group(2)
            return
        if top.kind == 'VarDecl':
            m = re.search(r"(\w+) '([^']*)'", top.text)
            if m:
                self.globals_.add(top.id)
            return
        if top.kind != 'FunctionDecl':
            return
        body = [k for k in top.kids if k.kind == 'CompoundStmt']
        nm = re.search(r"(\w+) '", top.text)
        if not nm:
            return
        func = nm.group(1)
        parms = [k for k in top.kids if k.kind == 'ParmVarDecl']
        if not body:
            return
        pinfo = []
        self.params = {}
        for i, p in enumerate(parms):
            pm = re.search(r"(\w+) '([^']*)'(?::'([^']*)')?", p.text)
            pinfo.append((pm.group(1) if pm else '?',
                          self.norm(pm.group(3) or pm.group(2)) if pm else ''))
            self.params[p.id] = i
        u.defs[func] = dict(params=pinfo, file=top.file, line=top.line)
        mine = not (top.file and self.main and
                    os.path.normpath(top.file) != os.path.normpath(self.main))
        for x in apc.walk(body[0]):
            if x.kind == 'VarDecl':
                if re.search(r'\bextern\b', x.text):
                    self.globals_.add(x.id)
                    continue
                init = _expr_kids(x)
                if init:
                    l2, d2 = self.classify(init[-1], func)
                    nd = self.node(self.var_key(func, x.id))
                    for a, b in l2.items():
                        nd['labels'].setdefault(a, b)
                    nd['deps'] += d2
            elif x.kind in ('BinaryOperator', 'CompoundAssignOperator') and len(x.kids) == 2 \
                    and re.search(r"'(=|\+=|-=)'", x.text):
                lhs = apc.strip(x.kids[0], ('ParenExpr',))
                if lhs is not None and lhs.kind == 'DeclRefExpr' and is_pointer(lhs.dtype):
                    m = VAR_REF.search(lhs.text)
                    if m and m.group(2) not in self.globals_:
                        l2, d2 = self.classify(x.kids[1], func)
                        nd = self.node(self.var_key(func, m.group(2)))
                        for a, b in l2.items():
                            nd['labels'].setdefault(a, b)
                        nd['deps'] += d2
            elif x.kind == 'CallExpr' and x.kids:
                callee = apc.strip(x.kids[0], ('ParenExpr', 'ImplicitCastExpr', 'CStyleCastExpr'))
                if callee is None or callee.kind != 'DeclRefExpr':
                    continue
                m = FUNC_REF.search(callee.text)
                if not m:
                    continue
                for i, arg in enumerate(x.kids[1:]):
                    l2, d2 = self.classify(arg, func)
                    key = self.new_node(l2, d2)
                    lit = apc.literal_value(arg)
                    u.args.append(dict(callee=m.group(2), idx=i, key=key,
                                       argtype=self.norm(arg.dtype), literal=lit,
                                       evidence='arg %d of %s in %s at %s:%s' % (
                                           i + 1, m.group(2), func,
                                           os.path.basename(x.file or '?'), x.line)))
            if not mine:
                continue
            if x.file and self.main and os.path.normpath(x.file) != os.path.normpath(self.main):
                continue
            base = None
            if x.kind == 'UnaryOperator' and "prefix '*'" in x.text and x.kids:
                base, op = x.kids[0], '*'
            elif x.kind == 'ArraySubscriptExpr' and len(x.kids) >= 2:
                base = x.kids[0] if is_pointer(x.kids[0].dtype) else x.kids[1]
                op = '[]'
            if base is None or not apc.wide_pointee(self.norm(base.dtype)):
                continue
            s = apc.strip(base)
            if s is not None and s.kind == 'CStyleCastExpr':
                continue                    # pointer_casts CELL-READ
            s2 = apc.strip(base, ('ParenExpr',))
            if s2 is not None and s2.kind == 'ImplicitCastExpr' and \
                    '<ArrayToPointerDecay>' in s2.text and s2.kids:
                c = apc.strip(s2.kids[0], ('ParenExpr',))
                if c is not None and c.kind == 'DeclRefExpr':
                    continue                # an array named directly
            l2, d2 = self.classify(base, func)
            key = self.new_node(l2, d2)
            u.derefs.append(dict(func=func, file=x.file, line=x.line, key=key,
                                 base=describe(base), op=op, type=base.type))


def describe(n):
    n = apc.strip(n)
    if n is None:
        return '?'
    if n.kind == 'DeclRefExpr':
        m = VAR_REF.search(n.text)
        return m.group(3) if m else '?'
    if n.kind == 'ArraySubscriptExpr' and n.kids:
        return describe(n.kids[0]) + '[]'
    if n.kind == 'UnaryOperator' and n.kids:
        return '*' + describe(n.kids[0])
    if n.kind == 'BinaryOperator' and n.kids:
        return describe(n.kids[0]) + '+..'
    return n.kind


def scan_source(argv, src, main_file=None, obj=None):
    """Unit facts of one translation unit; (unit, error)."""
    main_file = main_file or src
    u = Unit(obj or os.path.basename(src))
    try:
        p = subprocess.Popen(apc.clang_argv(argv, src), cwd=ROOT, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, errors='replace')
    except OSError as ex:
        return u, str(ex)
    sc = Scanner(u, main_file)
    sc.globals_ = set()
    sc.params = {}
    sc.typedefs = {}
    try:
        for top in apc.parse_dump(p.stdout):
            sc.scan(top)
    finally:
        p.stdout.close()
        err = p.stderr.read()
        p.stderr.close()
        p.wait()
    errs = [l for l in err.splitlines() if ': error:' in l
            and 'conflicting types for' not in l]
    if p.returncode not in (0, 1) or (p.returncode == 1 and errs):
        return u, '\n'.join(errs)[-400:] or err[-400:]
    return u, None


# ------------------------------------------------------------- the build

def resolve(units, param_type, resident):
    """Fixed point over every unit's nodes; -> {key: {label: evidence}}.
    param_type(callee, idx) -> the winning definition's parameter type or
    None; resident(name) -> True for a PS1-resident global."""
    nodes = {}
    for u in units:
        for k, v in u.nodes.items():
            nd = nodes.setdefault(k, dict(labels={}, deps=[], chain=v['chain']))
            for a, b in v['labels'].items():
                if a.startswith('G:'):
                    a, b = ('PS1' if resident(a[2:]) else 'HOST'), b
                nd['labels'].setdefault(a, b)
            nd['deps'] += v['deps']
    # parameters from every call's argument
    for u in units:
        for c in u.args:
            pt = param_type(c['callee'], c['idx'])
            if pt is None:
                continue
            key = ('P', c['callee'], c['idx'])
            nd = nodes.setdefault(key, dict(labels={}, deps=[], chain=False))
            nd['deps'].append(c['key'])
            an = nodes[c['key']]
            an['labels'] = {a: '%s: %s' % (c['evidence'], b) for a, b in an['labels'].items()}
            if apc.wide_pointee(pt):
                at = c['argtype']
                if is_integer(at) and c['literal'] != 0:
                    an['labels'].setdefault('INT', "%s: '%s' into '%s'" % (c['evidence'], at, pt))
                elif narrow_pointee(at):
                    an['labels'].setdefault('NARROW', "%s: '%s' into '%s'" % (
                        c['evidence'], at, pt))

    def exported(nd):
        if nd['chain']:
            if any(a in nd['labels'] for a in ('INT', 'PS1')):
                ev = nd['labels'].get('INT') or nd['labels'].get('PS1')
                return {'PS1': 'a cell of ' + ev}
            return {}
        return nd['labels']
    changed = True
    while changed:
        changed = False
        for k, nd in nodes.items():
            for d in nd['deps']:
                dn = nodes.get(d)
                if dn is None:
                    continue
                for a, b in exported(dn).items():
                    if a not in nd['labels']:
                        nd['labels'][a] = b
                        changed = True
    return {k: v['labels'] for k, v in nodes.items()}


def judge(units, labels):
    rows = []
    for u in units:
        for d in u.derefs:
            lab = labels.get(d['key'], {})
            for shape, keys in (('PS1-CELL', ('INT', 'PS1')), ('NARROW-CELL', ('NARROW',))):
                hit = [a for a in keys if a in lab]
                if hit:
                    r = dict(d)
                    r.pop('key')
                    r.update(shape=shape, obj=u.obj, origin=lab[hit[0]],
                             detail="%s%s through '%s' <- %s" % (
                                 d['op'], d['base'], d['type'], lab[hit[0]]))
                    rows.append(r)
    return rows


def audit(build_dir='pc/build', jobs=8, binary=None):
    import audit_host_globals as hg
    order, _srcmap, _tiers, defs_nm, _undef = stubs.build_index(build_dir)
    binary = binary or os.path.join(ROOT, build_dir, 'tokimeki-pc')
    resident_names = {n for n, ds in hg.nm_symbols(binary).items()
                      if any(hg.in_ram(v) for v, _s, _k in ds)}
    cmds = apc.compile_commands(build_dir)
    winner = {}
    for o in order:
        b = os.path.basename(o)
        for typ, name in defs_nm.get(b, []):
            if typ in 'TW' and (name not in winner or (typ == 'T' and winner[name][1] != 'T')):
                if name not in winner or winner[name][1] != 'T':
                    winner[name] = (b, typ)
    work, seen = [], set()
    for o in order:
        b = os.path.basename(o)
        if b in cmds and b not in seen:
            seen.add(b)
            argv, src = cmds[b]
            work.append((b, argv, src, apc.tier(src)))

    def one(item):
        b, argv, src, t = item
        main = apc.first_line_target(src)
        u, err = scan_source(argv, src, main, obj=b)
        if t not in apc.GAME_TIERS:
            u.derefs = []
        u.src, u.tier = decomp_key(main), t
        return u, (b, err) if err else None
    units, errors = [], []
    with ThreadPoolExecutor(jobs) as ex:
        for u, e in ex.map(one, work):
            units.append(u)
            if e:
                errors.append(e)
    by_obj = {u.obj: u for u in units}

    def param_type(callee, idx):
        w = winner.get(callee)
        d = by_obj[w[0]].defs.get(callee) if w and w[0] in by_obj else None
        if d is None or idx >= len(d['params']):
            return None
        return d['params'][idx][1]
    labels = resolve(units, param_type, lambda n: n in resident_names)
    rows = judge(units, labels)
    for r in rows:
        if r.get('file'):
            r['file'] = decomp_key(r['file'])    # repository-relative, as in the monorepo
        u = by_obj[r['obj']]
        r.update(src=u.src, tier=u.tier, live=winner.get(r['func'], (None,))[0] == r['obj'])
    rows.sort(key=lambda r: (r['shape'], not r['live'], r['func'], r['file'] or '', r['line']))
    return rows, errors, len(work)


def audit_sources(paths, argv=None):
    """--source: (rows, errors) for these files; calls resolved among them,
    every extern global PS1-resident."""
    units, errors = [], []
    for i, s in enumerate(paths):
        u, err = scan_source(argv or ['gcc'] + apc.GAME_FLAGS, s, obj='%d:%s' % (
            i, os.path.basename(s)))
        units.append(u)
        if err:
            errors.append((s, err))
    defs = {}
    for u in units:
        defs.update(u.defs)

    def param_type(callee, idx):
        d = defs.get(callee)
        return d['params'][idx][1] if d and idx < len(d['params']) else None
    labels = resolve(units, param_type, lambda n: True)
    rows = judge(units, labels)
    for r in rows:
        if r.get('file'):
            r['file'] = decomp_key(r['file'])    # repository-relative, as in the monorepo
    return rows, errors


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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default='pc/build')
    ap.add_argument('--allow', default=os.path.join(ROOT, 'pc/tools/pointer_cell_allow.txt'))
    ap.add_argument('--json')
    ap.add_argument('--all', action='store_true', help='also print dead and allowed rows')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--source', nargs='+', help='audit these files with the game flags')
    a = ap.parse_args(argv)

    if a.source:
        rows, errors = audit_sources(a.source)
        for s, e in errors:
            print('%s: clang failed: %s' % (s, e), file=sys.stderr)
        for r in rows:
            print('%-11s %s:%d %s  %s' % (r['shape'], r['file'], r['line'], r['func'],
                                        r['detail']))
        return 1 if rows or errors else 0

    rows, errors, n = audit(a.build_dir, a.jobs)
    allow = load_allow(a.allow)
    gated = [r for r in rows if r['live'] and not allowed(r, allow)]
    shown = rows if a.all else gated
    for r in shown:
        print('%-11s %-5s %-34s %s:%d  %s | %s' % (
            r['shape'], 'live' if r['live'] else 'dead', r['func'], r['file'], r['line'],
            r['detail'][:160], apc.source_line(r)[:90]))
    for b, e in errors:
        print('CLANG-FAILED %s: %s' % (b, ' '.join(e.split())[:200]), file=sys.stderr)
    used = {k for k in allow if any(allowed(r, {k: 1}) for r in rows if r['live'])}
    stale = sorted(set(allow) - used)
    for k in stale:
        print('STALE-ALLOW %s (no live row; delete the line)' % k)
    c = collections.Counter((r['shape'], r['live']) for r in rows)
    funcs = len({r['func'] for r in rows if r['live']})
    print('%d translation units; rows: %s; live functions %d; gated %d; clang failures %d' % (
        n, ', '.join('%s %s %d' % (s, 'live' if l else 'dead', v)
                     for (s, l), v in sorted(c.items())) or 'none', funcs, len(gated),
        len(errors)))
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)
    return 1 if gated or errors or stale else 0


if __name__ == '__main__':
    sys.exit(main())
