#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Calls that rely on MIPS argument registers the C does not name.

IDO passes the first four arguments in $a0-$a3, and a register keeps its
value until something writes it.  Matched C can therefore call a function
with FEWER arguments than the function reads and still reproduce the
console's bytes: `taco_actor_exit()` declared `(void)` while the definition
reads `id`, or `taco_actor_disp_switch(0)` for a (mode, id) callee -- the
console runs the callee with whatever $a0/$a1 hold at the jal (the
caller's own argument, a value IDO kept there).  On x86-64 the missing
argument is whatever %edi/%esi/... last held: TACO gameplay released
garbage actor slots (370,448 recovered crashes, 09ffb5ac3a).

The shapes, as the compiler types them (clang's AST of the exact
translation unit pc/Makefile compiles, like audit_ps1_pointer_casts.py):

  a call through a prototype that lists fewer parameters than the
  definition (`extern void f(void);` for `void f(s32 id)`), a call to an
  unprototyped / K&R / implicitly declared function, and a call through a
  cast of the function (`((s32 (*)())f)(...)`, the overlay generator's
  view of a file's own prototype) -- whenever the call passes fewer
  arguments than the DEFINITION that wins the link declares.

The same holds for the result register: a `void` function whose last
act is a call (`TmdTG3`: `GsTMDfastTG3L(...); }`) leaves the callee's $v0
in place, and a caller that declares it as returning a value
(GsSortObject4's `GsOUT_PACKET_P = TmdTG3(...)`) reads that $v0 on the
console.  On x86-64 %rax after a void function is whatever it last held:
gcc pops a stack argument into %rax on the way out of TmdTG3, and TACO's
packets were written over the ordering-table headers.

Rows (one per call site):

  SHORT-READ    the callee's body reads a parameter the call does not
                pass.  Gated: a live row outside --allow exits 1.
  SHORT-UNUSED  the missing parameters are never read by the body (the
                register is dead; harmless).  Reported with --all.
  VOID-READ     the call's value is used, and the winning definition
                returns void.  Reported (counted; rows with --all), not
                gated: 2026-11-01 census 345 live rows (with the calls
                through header macros counted, naked aliases not), most of them a
                dispatcher's `ret = step();` handed back to callers that
                ignore it.  Fixed where the value is consumed:
                pc/tools/return_plan.json.

  WIN64-STACK   (--width) an argument from the fifth on, passed as a 4-byte
                integer (int, s32, u32, long on Windows, a promoted char or
                short) where the winning definition declares an 8-byte
                parameter (a pointer, long long).  System V passes the
                first six arguments in registers, where a 32-bit write
                zero-extends; the Microsoft x64 convention passes the
                fifth on in 8-byte stack slots, and the caller stores 4
                bytes: the callee reads the slot's stale upper half
                (SetDrawMode's `RECT *tw`, declared `s32` by its callers:
                the first Windows new_game faulted on 0x00000b7300000002).
  STACK-WIDTH   (--width) the same from the seventh argument on, which is
                on the stack on Linux too.

Columns: shape, live (the calling function wins the link), caller,
file:line, callee (the winning definition's object), passed / declared
parameters, the missing parameters the body reads.

usage: audit_ps1_call_args.py [--build-dir pc/build] [--allow FILE]
                              [--json OUT] [--all] [--jobs N]
                              [--source FILE ...]
Run after `make -f pc/Makefile all`.  --allow defaults to
pc/tools/call_args_allow.txt (`caller` or `caller:callee`, then `#
reason`; it may only shrink).  --source audits single files with the game
compile flags, callee definitions taken from the same files (the tests'
entry point).
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_pc_stubs as stubs  # noqa: E402
import audit_ps1_pointer_casts as apc  # noqa: E402
from pc_paths import decomp_key, spell  # noqa: E402

ROOT = stubs.ROOT
FUNC_REF = re.compile(r"\bFunction (0x[0-9a-f]+) '(\w+)'")
PARM_REF = re.compile(r"\bParmVar (0x[0-9a-f]+) ")
# A matched overlay source may call another function of its own overlay by
# its address name (`func_80137CCC()` for taco_xa_sector_set, the name its
# callers kept when the head was absorbed and the symbol named).  The PC
# build links that name to overlay_symtab.c's trampoline, which runs the
# loaded overlay's entry at that address: no definition carries the name,
# so until 2026-11-06 such calls were never judged (TACO taco_main7_main1 /
# main2 passed the narration index in $a0 only).
TRAMP_NAME = re.compile(r'^func_([0-9A-Fa-f]{8})$')
OVERLAY_OBJ = re.compile(r'^overlay_([A-Z0-9_]+)\.o$')
SYMTAB_ENTRY = re.compile(r'^\s*\{ 0x([0-9A-Fa-f]{8})u, \(void \*\)(\w+) \},')


def overlay_entries(path=None):
    """{(OVERLAY, address): definition name} from overlay_symtab.c's entry
    tables (names carry the overlay's prefix, `TACO_taco_xa_sector_set`)"""
    path = path or os.path.join(ROOT, 'pc/src/overlay_symtab.c')
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, errors='replace') as fh:
        lines = fh.readlines()
    for line in lines:
        m = SYMTAB_ENTRY.match(line)
        if m:
            name = m.group(2)
            ov = name.split('_', 1)[0]
            for cand in OVERLAY_PREFIXES:
                if name.startswith(cand + '_'):
                    ov = cand
                    break
            out.setdefault((ov, int(m.group(1), 16)), name)
    return out


# overlay names that contain an underscore (BUNKA_SD, EN_NICHI, ...): the
# longest prefix wins, so list them before the plain split
OVERLAY_PREFIXES = sorted(
    (os.path.basename(p)[len('overlay_'):-2]
     for p in __import__('glob').glob(os.path.join(ROOT, 'pc/src/overlay_*.c'))
     if re.match(r'overlay_[A-Z0-9_]+\.c$', os.path.basename(p))),
    key=len, reverse=True)


def scan(lines, main_file):
    """-> (definitions, calls) of one translation unit.
    definitions: {name: dict(params=[name], read=[bool], variadic, line)}
    calls: [dict(func, callee, nargs, file, line)] (calls written in
    main_file, callee named directly or through casts / parens)."""
    defs, calls = {}, []
    for top in apc.parse_dump(lines):
        for fn in apc.walk(top):
            if fn.kind != 'FunctionDecl':
                continue
            body = [k for k in fn.kids if k.kind == 'CompoundStmt']
            nm = re.search(r"(\w+) '", fn.text)
            if not nm:
                continue
            name = nm.group(1)
            if body:
                parms = [k for k in fn.kids if k.kind == 'ParmVarDecl']
                ids = [p.id for p in parms]
                used = set()
                for x in apc.walk(body[0]):
                    if x.kind == 'DeclRefExpr':
                        m = PARM_REF.search(x.text)
                        up = x.parent
                        while up is not None and up.kind in ('ParenExpr', 'ImplicitCastExpr'):
                            up = up.parent
                        if up is not None and '<ToVoid>' in up.text:
                            continue          # `(void)arg;`: marks it unused
                        if m:
                            used.add(m.group(1))
                pn = [(re.search(r"(?:used |referenced )*(\w+) '", p.text) or
                       re.search(r"(\w+)", p.text)).group(1) if p.text else '?' for p in parms]
                defs[name] = dict(params=pn, read=[i in used for i in ids],
                                  ptypes=[p.dtype for p in parms],
                                  variadic='...' in fn.type, line=fn.line,
                                  file=fn.file,
                                  # a naked `jmp target` alias returns the
                                  # target's value (missing_funcs_gen.c)
                                  void=fn.type.startswith('void (') and
                                  not any(k.kind == 'NakedAttr' for k in fn.kids))
                for x in apc.walk(body[0]):
                    if x.kind != 'CallExpr' or not x.kids:
                        continue
                    callee = apc.strip(x.kids[0], ('ParenExpr', 'ImplicitCastExpr',
                                                   'CStyleCastExpr'))
                    if callee is None or callee.kind != 'DeclRefExpr':
                        continue
                    m = FUNC_REF.search(callee.text)
                    if not m:
                        continue
                    # calls of the functions defined in the unit's own text
                    # (the enclosing definition's file, not the call's: a
                    # call through a macro -- sony_bios_alias.h's
                    # `#define strncpy func_800B4830` -- is located in the
                    # header, and was skipped until 2026-11-01)
                    if fn.file and main_file and \
                            os.path.normpath(fn.file) != os.path.normpath(main_file):
                        continue
                    calls.append(dict(func=name, callee=m.group(2),
                                      nargs=len(x.kids) - 1, file=x.file, line=x.line,
                                      used=value_used(x),
                                      atypes=[k.dtype for k in x.kids[1:]]))
    return defs, calls


STMT_COND_FIRST = ('IfStmt', 'WhileStmt', 'SwitchStmt')


def value_used(call):
    """True when the program reads the call's result: its first enclosing
    non-paren node is an expression (not a `(void)` cast), a return, an
    initialiser, or the condition of an if / while / switch / do"""
    if call.type.startswith('void') and not call.type.startswith('void *'):
        return False
    x, up = call, call.parent
    while up is not None and up.kind == 'ParenExpr':
        x, up = up, up.parent
    if up is None:
        return False
    if up.kind.endswith(('Expr', 'Operator')):
        return '<ToVoid>' not in up.text
    if up.kind in ('ReturnStmt', 'VarDecl'):
        return True
    if up.kind in STMT_COND_FIRST:
        return bool(up.kids) and up.kids[0] is x
    if up.kind == 'DoStmt':
        return bool(up.kids) and up.kids[-1] is x
    return False


SYMBOL_START = re.compile(r'@SymbolStart:\s*0x([0-9A-Fa-f]{8})')


def alias_address_name(defs, src, main_file):
    """A matched source of ONE function with an `@SymbolStart: 0xADDR`
    window also answers to `func_ADDR`, the name its callers may still use
    (the --source / test path: TRAMP_NAME, without overlay_symtab.c)"""
    try:
        with open(os.path.join(ROOT, spell(src)), errors='replace') as fh:
            text = fh.read()
    except OSError:
        return
    m = SYMBOL_START.search(text)
    if not m:
        return
    own = [n for n, d in defs.items() if d.get('file') and
           os.path.normpath(d['file']) == os.path.normpath(main_file or src)]
    if len(own) == 1:
        defs.setdefault('func_%s' % m.group(1).upper(), defs[own[0]])


def scan_source(argv, src, main_file=None):
    main_file = main_file or src
    try:
        p = subprocess.Popen(apc.clang_argv(argv, src), cwd=ROOT, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, errors='replace')
    except OSError as ex:
        return {}, [], str(ex)
    try:
        defs, calls = scan(p.stdout, main_file)
    finally:
        p.stdout.close()
        err = p.stderr.read()
        p.stderr.close()
        p.wait()
    alias_address_name(defs, src, main_file)
    # files repository-relative (pc_paths.decomp_key), as in the monorepo
    for x in list(defs.values()) + list(calls):
        if x.get('file'):
            x['file'] = decomp_key(x['file'])
    errs = [l for l in err.splitlines() if ': error:' in l
            and 'conflicting types for' not in l]
    if p.returncode not in (0, 1) or (p.returncode == 1 and errs):
        return defs, calls, '\n'.join(errs)[-400:] or err[-400:]
    return defs, calls, None


def judge(calls, defs_of, by_call=False):
    """Rows for calls that pass fewer arguments than the callee's
    definition declares.  defs_of(callee) -> definition dict or None
    (by_call: defs_of(call))."""
    rows = []
    for c in calls:
        d = defs_of(c) if by_call else defs_of(c['callee'])
        if d is not None and d.get('void') and c.get('used'):
            r = dict(c)
            r.update(shape='VOID-READ', declared=len(d['params']), missing=[], read=['$v0'],
                     callee_file=d.get('file'))
            rows.append(r)
        if d is None or c['nargs'] >= len(d['params']):
            continue
        missing = [(i, d['params'][i]) for i in range(c['nargs'], len(d['params']))]
        read = [n for i, n in missing if d['read'][i]]
        r = dict(c)
        r.update(shape='SHORT-READ' if read else 'SHORT-UNUSED',
                 declared=len(d['params']), missing=[n for _, n in missing], read=read,
                 callee_file=d.get('file'))
        rows.append(r)
    return rows


def wide_param(t):
    """an 8-byte parameter on both ABIs: a pointer (arrays decay), a
    function pointer, long long"""
    return '*' in t or '(' in t or 'long long' in t


def narrow_arg(t):
    """a 4-byte (or promoted smaller) integer argument on Windows: int,
    unsigned, long (32 bits there), enums, char/short after promotion"""
    if '*' in t or '(' in t or '[' in t or 'long long' in t:
        return False
    return bool(re.search(r'\b(int|long|short|char|_Bool|enum)\b', t))


# callees whose definition takes the low 32 bits of the parameter itself
# (every address the port hands out is below 4 GB): their callers are fine
WIDTH_SAFE_CALLEES = {
    'SetDrawMode': 'pc/hal/gpu.c zero-extends tw',
}


def judge_width(calls, defs_of):
    """WIN64-STACK / STACK-WIDTH rows (see the docstring)"""
    rows = []
    for c in calls:
        if c['callee'] in WIDTH_SAFE_CALLEES:
            continue
        d = defs_of(c['callee'])
        if d is None or not d.get('ptypes'):
            continue
        for i, at in enumerate(c.get('atypes', [])):
            if i < 4 or i >= len(d['ptypes']):
                continue
            if wide_param(d['ptypes'][i]) and narrow_arg(at):
                r = dict(c)
                r.update(shape='WIN64-STACK' if i < 6 else 'STACK-WIDTH',
                         declared=len(d['params']), missing=[],
                         read=[f"{d['params'][i]}#{i + 1}: {at} -> {d['ptypes'][i]}"],
                         callee_file=d.get('file'))
                rows.append(r)
    return rows


def audit(build_dir='pc/build', jobs=8, width=False):
    order, _srcmap, _tiers, defs_nm, _undef = stubs.build_index(build_dir)
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
        if b in cmds and b not in seen:       # ALL_OBJS lists overlay modules twice
            seen.add(b)
            argv, src = cmds[b]
            work.append((b, argv, src, apc.tier(src)))

    def one(item):
        b, argv, src, t = item
        main = apc.first_line_target(src)
        defs, calls, err = scan_source(argv, src, main)
        if t not in apc.GAME_TIERS:
            calls = []
        return b, main, t, defs, calls, err
    by_obj, calls, errors = {}, [], []
    with ThreadPoolExecutor(jobs) as ex:
        for b, main, t, defs, cs, err in ex.map(one, work):
            by_obj[b] = defs
            for c in cs:
                c.update(obj=b, src=decomp_key(main), tier=t,
                         live=winner.get(c['func'], (None,))[0] == b)
            calls += cs
            if err:
                errors.append((b, err))

    def defs_of(name):
        w = winner.get(name)
        if w and name in by_obj.get(w[0], {}):
            return by_obj[w[0]][name]
        return None
    entries = overlay_entries()

    def defs_of_call(c):
        """the definition a call reaches: by name, or -- for an overlay's
        call of `func_<ADDR>`, which links to overlay_symtab.c's trampoline
        and runs the loaded overlay's function at ADDR -- the definition
        that the calling overlay's entry table names at that address"""
        d = defs_of(c['callee'])
        if d is not None:
            return d
        m = TRAMP_NAME.match(c['callee'])
        ov = OVERLAY_OBJ.match(c['obj'])
        if not (m and ov):
            return None
        target = entries.get((ov.group(1), int(m.group(1), 16)))
        if target is None:
            return None
        c['callee_alias'] = target
        return by_obj.get(c['obj'], {}).get(target) or defs_of(target)
    rows = judge(calls, defs_of_call, by_call=True)
    if width:
        rows += judge_width(calls, defs_of)
    for r in rows:
        r['callee_obj'] = winner.get(r['callee'], ('?',))[0]
    rows.sort(key=lambda r: (r['shape'], not r['live'], r['func'], r['file'] or '', r['line']))
    return rows, errors, len(work)


GATED = ('SHORT-READ',)
# with --width: a live row fails too (pc/tools/stack_width_plan.json fixes them)
WIDTH_GATED = ('WIN64-STACK', 'STACK-WIDTH')


def load_allow(path):
    out = {}
    if path and os.path.exists(path):
        for line in open(path):
            body = line.split('#')[0].strip()
            if body:
                out[body] = line.split('#', 1)[1].strip() if '#' in line else ''
    return out


def allowed(r, allow):
    return r['func'] in allow or '%s:%s' % (r['func'], r['callee']) in allow


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default='pc/build')
    ap.add_argument('--allow', default=os.path.join(ROOT, 'pc/tools/call_args_allow.txt'))
    ap.add_argument('--json')
    ap.add_argument('--all', action='store_true',
                    help='also print SHORT-UNUSED, dead and allowed rows')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--source', nargs='+', help='audit these files with the game flags')
    ap.add_argument('--width', action='store_true',
                    help='also check WIN64-STACK / STACK-WIDTH rows (a live one fails)')
    a = ap.parse_args(argv)

    if a.source:
        bad, defs, calls = 0, {}, []
        for s in a.source:
            d, cs, err = scan_source(['gcc'] + apc.GAME_FLAGS, s)
            if err:
                print('%s: clang failed: %s' % (s, err), file=sys.stderr)
                bad = 1
            defs.update(d)
            calls += cs
        for r in judge(calls, defs.get):
            print('%-12s %s:%d %s -> %s(%d of %d) reads %s' % (
                r['shape'], r['file'], r['line'], r['func'], r['callee'], r['nargs'],
                r['declared'], ','.join(r['read']) or '-'))
            bad |= r['shape'] in GATED
        return 1 if bad else 0

    rows, errors, n = audit(a.build_dir, a.jobs, width=a.width)
    allow = load_allow(a.allow)
    gated = [r for r in rows if r['live'] and not allowed(r, allow) and
             (r['shape'] in GATED or r['shape'] in WIDTH_GATED)]
    shown = rows if a.all else gated
    for r in shown:
        print('%-12s %-5s %-34s %s:%d  -> %s(%d of %d) reads %s [%s]' % (
            r['shape'], 'live' if r['live'] else 'dead', r['func'], r['file'], r['line'],
            r['callee'], r['nargs'], r['declared'], ','.join(r['read']) or '-',
            r['callee_obj']))
    for b, e in errors:
        print('CLANG-FAILED %s: %s' % (b, ' '.join(e.split())[:200]), file=sys.stderr)
    live_read = [r for r in rows if r['live'] and r['shape'] in GATED]
    used = {k for k in allow if any(allowed(r, {k: 1}) for r in live_read)}
    stale = sorted(set(allow) - used)
    for k in stale:
        print('STALE-ALLOW %s (no live SHORT-READ row; delete the line)' % k)
    c = collections.Counter((r['shape'], r['live']) for r in rows)
    print('%d translation units; rows: %s; gated %d; clang failures %d' % (
        n, ', '.join('%s %s %d' % (s, 'live' if l else 'dead', v)
                     for (s, l), v in sorted(c.items())) or 'none', len(gated), len(errors)))
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)
    return 1 if gated or errors or stale else 0


if __name__ == '__main__':
    sys.exit(main())
