#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Zero-extend signed 32-bit PS1 addresses where game C turns them into
pointers (PC build step).

The console is a 32-bit machine: `(u8 *)tbl` with an `s32 tbl` holding
0x800B9A00 is the address 0x800B9A00.  On LP64 gcc converts a signed
integer to a pointer by SIGN-extending it: 0xFFFFFFFF800B9A00, a kernel
address.  A dereference faults, and pc/src/main.c used to fold the value
back at the fault and retry; a comparison, a range check or a pointer
difference between such a value and a real pointer is simply wrong, and
no fault ever happens to repair it.

This filter makes the conversion zero-extend, which is what the console
computes.  It reads clang's AST of the exact translation unit (the flags
pc/Makefile compiles it with, given after `--`), finds every conversion
of a signed 32-bit integer (int/s32/short/s16/signed char/s8 and typedefs
of them) to a pointer -- audit_ps1_pointer_casts.py's SEXT-PTR shape --
and rewrites it in the source text:

  explicit  `(T *)expr`  -> `(T *)(unsigned int)expr`   (the cast's operand is
            a cast-expression, so `(unsigned int)expr` parses as the same
            operand; inserted after the cast's closing parenthesis)
  implicit  `p = expr` / `f(expr)` / `return expr` with a pointer target
            -> `(unsigned int)(expr)`
  call      an argument of a call through an unprototyped view of a
            prototyped function -- the overlay generator's
            `((s32 (*)())f)(..., expr)` -- where the argument is a signed
            32-bit integer and f's parameter is a pointer: no conversion
            exists in the AST (the int is passed as an int, and the callee
            reads the whole 64-bit register, which gcc filled by sign
            extension), so the argument becomes
            `(void *)(unsigned int)(expr)`, a pointer argument as f's
            prototype declares it

A cast written in a macro body is rewritten in the body; every expansion
of it must then be a SEXT conversion (or already unsigned), otherwise the
filter refuses.  An implicit conversion whose operand starts and ends in
different macro contexts cannot be wrapped and is refused too.  A refusal
fails the build: give the function a PC override instead.

pc/Makefile routes PS1_ADDR_SRCS through this filter (after
adapt_ps1_cells.py when a file is in both lists).  The matched C is not
edited; the output keeps (or adds) a leading `#line 1 "SRC"`, and the
rewrite never adds a line, so diagnostics and debug info name the
original lines.

usage: adapt_ps1_addresses.py [--list] SRC OUT -- COMPILER-FLAGS...
--list prints the rewritten sites instead of failing silently on none.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_ps1_pointer_casts as apc  # noqa: E402

ZEXT = '(unsigned int)'
ARG_ZEXT = '(void *)(unsigned int)'
NODE_RE = re.compile(r'^([| `]*)[-`]?(\w+) (0x[0-9a-f]+)?\s*(.*)$')
LOC_TOKEN = re.compile(r'([^\s<>,]+\.[ch]):(\d+):(\d+)|line:(\d+):(\d+)|col:(\d+)'
                       r'|<invalid sloc>|<scratch space>|<built-in>')
WIDE_SRC = re.compile(r'\*|\(\*|\blong\b')


class Refused(Exception):
    pass


class Loc(object):
    """Tracks clang's relative locations (`col:N` reuses the last line,
    `line:L:C` the last file) in print order."""

    def __init__(self):
        self.file, self.line = None, 0

    def read(self, text):
        """[(file, line, col) or None] for each location token in `text`."""
        out = []
        for m in LOC_TOKEN.finditer(text):
            if m.group(1):
                self.file, self.line = m.group(1), int(m.group(2))
                out.append((self.file, self.line, int(m.group(3))))
            elif m.group(4):
                self.line = int(m.group(4))
                out.append((self.file, self.line, int(m.group(5))))
            elif m.group(6):
                out.append((self.file, self.line, int(m.group(6))))
            else:
                out.append(None)
        return out


def nodes(lines):
    """Yield (depth, kind, text, begin, end, src_type) for each AST node;
    begin/end are (file, line, col) of the node's range."""
    loc = Loc()
    for raw in lines:
        m = NODE_RE.match(raw.rstrip('\n'))
        if not m or not m.group(2)[0].isupper():
            continue
        depth = (len(m.group(1)) + 1) // 2
        rest = m.group(4)
        begin = end = None
        # the range comes before any quoted type (`FunctionDecl 0x.. prev
        # 0x.. <range>` has text in front of it)
        lt, q = rest.find('<'), rest.find("'")
        if lt >= 0 and (q < 0 or lt < q):
            # the range: everything up to the matching '>'
            rest = rest[lt:]
            d, i = 0, 0
            for i, ch in enumerate(rest):
                if ch == '<':
                    d += 1
                elif ch == '>':
                    d -= 1
                    if d == 0:
                        break
            rng, tail = rest[1:i], rest[i + 1:]
            locs = loc.read(rng)
            if locs:
                begin, end = locs[0], locs[-1]
            # a declaration's own location follows the range
            own = re.match(r'\s*((?:[^\s<>,]+\.[ch]|line):\d+:\d+|col:\d+)', tail)
            if own:
                loc.read(own.group(1))
        else:
            tail = rest
        yield depth, m.group(2), tail, begin, end


def types_of(text):
    m = apc.TYPE_RE.search(text)
    if not m:
        return '', ''
    return m.group(1), (m.group(2) or m.group(1))


def signed32(dtype):
    return re.sub(r'\b(const|volatile)\b', '', dtype).strip() in apc.SIGNED32


FUNC_TYPE = re.compile(r"Function 0x[0-9a-f]+ '\w+' '([^']*)'")


def param_types(ftype):
    """['u8 *', 's32', ...] of a prototype type string `R (A, B)`, or None
    for an unprototyped `R ()`."""
    # the parameter list is the last top-level parenthesised group
    depth, start, end = 0, None, None
    for j in range(len(ftype) - 1, -1, -1):
        ch = ftype[j]
        if ch == ')':
            if depth == 0:
                end = j
            depth += 1
        elif ch == '(':
            depth -= 1
            if depth == 0:
                start = j
                break
    if start is None:
        return None
    inner = ftype[start + 1:end].strip()
    if not inner:
        return None
    out, depth, cur = [], 0, ''
    for ch in inner:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        depth += ch in '(['
        depth -= ch in ')]'
        cur += ch
    out.append(cur.strip())
    return [] if out == ['void'] else out


def unprototyped_callee(call):
    """the callee's prototype parameter list when `call` goes through a
    cast of a function to an unprototyped pointer type, else None"""
    if not call['kids']:
        return None
    n = call['kids'][0]
    seen_cast = False
    while n['kind'] in ('ParenExpr', 'ImplicitCastExpr', 'CStyleCastExpr') and n['kids']:
        if n['kind'] == 'CStyleCastExpr':
            t, _d = types_of(n['text'])
            if not re.search(r'\(\*\)\(\)$', t):
                return None
            seen_cast = True
        n = n['kids'][0]
    if not seen_cast or n['kind'] != 'DeclRefExpr':
        return None
    m = FUNC_TYPE.search(n['text'])
    return param_types(m.group(1)) if m else None


def collect(dump_lines, main_names):
    """[(kind, cast_begin, operand_begin, operand_end, src_type, sext)] for
    every IntegralToPointer conversion written in the main file, and every
    signed 32-bit argument passed to a pointer parameter through an
    unprototyped view (kind `CallArg`)."""
    found = []
    calls = []
    stack = []       # (depth, record)
    for depth, kind, text, begin, end in nodes(dump_lines):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        parent = stack[-1][1] if stack else None
        rec = dict(kind=kind, text=text, begin=begin, end=end, kids=[])
        if parent is not None:
            parent['kids'].append(rec)
        stack.append((depth, rec))
        if kind in ('CStyleCastExpr', 'ImplicitCastExpr') and '<IntegralToPointer>' in text:
            found.append(rec)
        if kind == 'CallExpr':
            calls.append(rec)
    out = []
    for call in calls:
        params = unprototyped_callee(call)
        if not params:
            continue
        for i, arg in enumerate(call['kids'][1:len(params) + 1]):
            if '*' not in params[i]:
                continue
            _t, dtype = types_of(arg['text'])
            sext = signed32(dtype)
            if sext:
                v = literal(arg)
                if v is not None and v >= 0:
                    sext = False
            if sext:
                out.append(dict(kind='CallArg', begin=arg['begin'], obegin=arg['begin'],
                                oend=arg['end'], dtype=dtype, sext=True,
                                main=bool(arg['begin'] and arg['begin'][0] in main_names)))
    for rec in found:
        if not rec['kids']:
            continue
        src = rec['kids'][0]
        _t, dtype = types_of(src['text'])
        sext = signed32(dtype)
        if sext:
            v = literal(src)
            if v is not None and v >= 0:
                sext = False
        out.append(dict(kind=rec['kind'], begin=rec['begin'], obegin=src['begin'],
                        oend=src['end'], dtype=dtype, sext=sext,
                        main=bool(rec['begin'] and rec['begin'][0] in main_names)))
    return out


def literal(rec):
    while rec['kind'] in ('ParenExpr', 'ImplicitCastExpr') and rec['kids']:
        rec = rec['kids'][0]
    if rec['kind'] == 'IntegerLiteral':
        try:
            return int(rec['text'].split()[-1], 0)
        except ValueError:
            return None
    if rec['kind'] == 'CharacterLiteral':
        return 0
    if rec['kind'] == 'UnaryOperator' and "'-'" in rec['text'] and rec['kids']:
        v = literal(rec['kids'][0])
        return -v if v is not None else None
    return None


# ---------------------------------------------------------------- text edits

def line_offsets(text):
    offs, pos = [0], 0
    for line in text.splitlines(True):
        pos += len(line)
        offs.append(pos)
    return offs


def token_end(text, i):
    """Offset just past the token that starts at `i`."""
    c = text[i]
    if c.isalnum() or c == '_' or (c == '.' and text[i + 1:i + 2].isdigit()):
        j = i + 1
        while j < len(text) and (text[j].isalnum() or text[j] in '_.'):
            j += 1
        return j
    if c in '"\'':
        j = i + 1
        while j < len(text) and text[j] != c:
            j += 2 if text[j] == '\\' else 1
        return j + 1
    if text[i:i + 2] in ('++', '--'):
        return i + 2
    return i + 1


def close_paren(text, i):
    """Offset of the ')' matching the '(' at `i`."""
    if text[i] != '(':
        raise Refused('cast does not start with ( at offset %d' % i)
    d = 0
    for j in range(i, len(text)):
        if text[j] == '(':
            d += 1
        elif text[j] == ')':
            d -= 1
            if d == 0:
                return j
    raise Refused('unbalanced cast at offset %d' % i)


def plan(sites, text, line_base=1):
    """{offset: insertion text} for the SEXT sites; refuses what it cannot
    express.  line_base: physical line of presumed line 1."""
    offs = line_offsets(text)

    def off(loc):
        line = loc[1] + line_base - 1
        if not 1 <= line < len(offs):
            raise Refused('location %s:%d:%d outside the file' % loc)
        return offs[line - 1] + loc[2] - 1

    by_cast = {}
    for s in sites:
        if s['kind'] == 'CStyleCastExpr' and s['begin']:
            by_cast.setdefault(s['begin'], []).append(s)
    casts, wraps = set(), set()
    for s in sites:
        if not s['sext']:
            continue
        where = 'line %d:%d' % s['begin'][1:] if s['begin'] else '?'
        if not s['main']:
            raise Refused('%s: SEXT conversion outside the source file (a header macro)'
                          % where)
        if s['kind'] == 'CStyleCastExpr':
            # every expansion of a macro-body cast must be SEXT or unsigned
            for o in by_cast[s['begin']]:
                if not o['sext'] and WIDE_SRC.search(o['dtype']):
                    raise Refused('%s: the cast also converts %s (a macro used with '
                                  'pointer and integer operands)' % (where, o['dtype']))
            casts.add(close_paren(text, off(s['begin'])) + 1)
        else:
            b, e = s['obegin'], s['oend']
            if not b or not e or b[0] != s['begin'][0]:
                raise Refused('%s: implicit conversion without a source range' % where)
            ob, oe = off(b), off(e)
            if define_name(text, ob) and not define_name(text, oe):
                # the operand starts with a macro (`MACRO(args) == p`): clang
                # names the body; the use is the nearest MACRO before the end
                ob = macro_use(text, define_name(text, ob), oe, where)
            if oe < ob or text[ob:oe].count('\n') > 20 or \
                    bool(define_name(text, ob)) != bool(define_name(text, oe)):
                raise Refused('%s: operand range spans macro contexts' % where)
            end = token_end(text, oe)
            if text[ob:end].count('(') != text[ob:end].count(')'):
                raise Refused('%s: operand %r is not balanced' % (where, text[ob:end]))
            wraps.add((ob, end, ARG_ZEXT if s['kind'] == 'CallArg' else ZEXT))
    edits = {}
    for at in casts:
        edits[at] = edits.get(at, '') + ZEXT
    for ob, end, how in sorted(wraps):
        edits[ob] = edits.get(ob, '') + how + '('
        edits[end] = ')' + edits.get(end, '')
    return edits


def define_name(text, at):
    """The macro a `#define` line containing offset `at` defines, else None."""
    start = text.rfind('\n', 0, at) + 1
    m = re.match(r'\s*#\s*define\s+(\w+)', text[start:])
    return m.group(1) if m else None


def macro_use(text, name, before, where):
    uses = [m.start() for m in re.finditer(r'\b%s\b' % re.escape(name), text[:before])]
    if not uses or define_name(text, uses[-1]):
        raise Refused('%s: no use of macro %s before the operand end' % (where, name))
    return uses[-1]


def apply(text, edits):
    for at in sorted(edits, reverse=True):
        text = text[:at] + edits[at] + text[at:]
    return text


def clang_dump(src, flags):
    argv = apc.clang_argv(['gcc'] + list(flags), src)
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, errors='replace')
    return p


def adapt(src, flags, work=None):
    """-> (adapted text, [site descriptions]).  clang reads a copy of SRC
    (in `work`, default a temporary directory) whose #line directives are
    comments, so every location it reports is a physical line of SRC; the
    output keeps the directives."""
    with open(src, encoding='utf-8', errors='replace') as fh:
        text = fh.read()
    first = re.match(r'#line 1 "([^"]+)"\n', text)
    neutral = re.sub(r'^([ \t]*#[ \t]*line\b[^\n]*)$',
                     lambda m: '/*' + m.group(1).replace('*/', '* /') + '*/', text, flags=re.M)
    tmpdir = None
    if work is None:
        tmpdir = tempfile.TemporaryDirectory(prefix='ps1_addr_')
        work = tmpdir.name
    os.makedirs(work, exist_ok=True)
    copy = os.path.join(work, '.ast.' + os.path.basename(src))
    try:
        with open(copy, 'w', encoding='utf-8') as fh:
            fh.write(neutral)
        here = os.path.dirname(first.group(1)) if first else os.path.dirname(src)
        p = clang_dump(copy, list(flags) + ['-iquote', here or '.',
                                            '-iquote', os.path.dirname(src) or '.'])
        try:
            sites = collect(p.stdout, {copy, os.path.normpath(copy)})
        finally:
            p.stdout.close()
            err = p.stderr.read()
            p.stderr.close()
            p.wait()
    finally:
        if os.path.exists(copy):
            os.remove(copy)
        if tmpdir is not None:
            tmpdir.cleanup()
    if p.returncode not in (0, 1):
        raise Refused('clang failed: %s' % err[-400:])
    edits = plan(sites, text, 1)
    seen = sorted({s['begin'][1:] for s in sites if s['sext']})
    desc = ['%s:%d:%d' % ((src,) + b) for b in seen]
    out = apply(text, edits)
    if not first:
        out = '#line 1 "%s"\n' % src + out
    return out, desc


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    flags = []
    if '--' in argv:
        i = argv.index('--')
        argv, flags = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--list', action='store_true', help='print the rewritten sites')
    ap.add_argument('src')
    ap.add_argument('out')
    a = ap.parse_args(argv)
    try:
        out, desc = adapt(a.src, flags, os.path.dirname(a.out) or '.')
    except Refused as ex:
        print('adapt_ps1_addresses: %s: %s' % (a.src, ex), file=sys.stderr)
        return 1
    if a.list:
        for d in desc:
            print(d)
    tmp = a.out + '.tmp'
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(out)
    os.replace(tmp, a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
