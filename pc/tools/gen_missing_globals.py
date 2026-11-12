#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate C definitions for the main-binary globals the PC port link cannot
resolve.  Type is taken from the most common `extern` declaration in the
source tree; initial bytes come from the retail binary when the address
lies inside the executable image (data/rodata), else zero (BSS).

A global is either an address name (`D_800EF123`) or a name landed by the
naming passes (config/symbol_names.json, `kind: data`, scope main), whose
address comes from that table.  An address outside the main image (main BSS,
or an overlay's static data at 0x80132000+) is a zeroed definition, which is
what pc/src/pc_port_stubs.c always did for overlay globals; on the PS1 all
overlays share that RAM, so one definition per address is the faithful model.

A typed ALIAS VIEW of a global -- `now_year_v`, `cw96_active_a`,
`girl0_flag_b`, `screen_w_b`: the decomp's spelling for "the object at
ADDR seen as another type" -- is not a second variable: it is written to
pc/src/missing_aliases.ld as `PROVIDE(view = D_ADDR);`, which pc/Makefile
passes to the linker, and the base `D_ADDR` is defined here when no object
defines it.  Symbols pc/build/obj already defines (nm) are never emitted.
Any other undefined symbol is not a global this tool knows and is listed on
stderr.

Usage:
    make -f pc/Makefile all -k 2>&1 | python3 pc/tools/gen_missing_globals.py
    (reads link errors on stdin; rewrites pc/src/missing_globals_gen.c and
    pc/src/missing_aliases.ld in place; `-o -` prints the C instead)

Incremental: the names the previous generated file and alias script provided
are kept (a link log only lists what was undefined WITH them linked), minus
any that another object now defines (nm over pc/build/obj).  A link that
exposes new alias bases may need a second round: run it until the link is
clean.

No retail bytes in the binary (2026-11-02): an initial value that is a
byte, halfword or word array (or an unnamed data blob a pointer refers to)
is NOT written out as a C initialiser.  The definition is left zero and
listed in pc_retail_init[] (address, size), and pc/src/retail_init.c copies
those bytes from the player's own PSX.EXE at start-up, right after main()
loads it -- before any game code runs.  Scalars and pointer tables (host
addresses) are still initialised here; pc/tools/audit_retail_bytes.py
checks the result.

Keep pc/src/missing_globals_manual.c disjoint: a symbol defined there must be
deleted from the generated output (the manual file holds the globals whose
retail value is a pointer into PS1 hardware).
"""
import re, sys, os, subprocess, collections, struct, pathlib, json, bisect
from safe_process import lines

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP, MONOREPO_DECOMP, disc_input  # noqa: E402


def git_grep(args, paths):
    """`git grep ARGS -- PATHS` over this checkout and, when toki1-decomp is a
    separate checkout, over its `src` too (the decompiled C lives there)."""
    out = subprocess.run(['git', 'grep'] + args + ['--'] + paths,
                         cwd=ROOT, capture_output=True, text=True).stdout
    if not MONOREPO_DECOMP and 'src' in paths:
        out += subprocess.run(['git', 'grep'] + args + ['--', 'src'],
                              cwd=DECOMP, capture_output=True, text=True).stdout
    return out
BIN = disc_input('disks/SLPS_000.65')
TEXT_START, FILE_BASE = 0x80041000, 0x800
img = BIN.read_bytes()
IMG_END = TEXT_START + len(img) - FILE_BASE

# landed data names <-> addresses (main scope)
names_doc = json.load(open(DECOMP / 'config' / 'symbol_names.json'))['symbols']
name_addr, addr_name = {}, {}
for key, e in names_doc.items():
    if ':' in key or e.get('kind') != 'data' or not e.get('landed'):
        continue
    a = int(key, 16)
    name_addr[e['name']] = a
    addr_name[a] = e['name']

manual = set(re.findall(r'^\s*(?:static\s+)?[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*=',
                        (ROOT / 'pc' / 'src' / 'missing_globals_manual.c').read_text(), re.M))

def defined_symbols():
    """every symbol an object in pc/build/obj already defines"""
    # the objects pc/Makefile links -- NOT the directory listing: an object of
    # a deleted source stays in pc/build/obj and would count as a definition
    r = subprocess.run(['make', '-f', 'pc/Makefile', '--eval', 'print-objs: ; @echo $(ALL_OBJS)', 'print-objs'],
                       capture_output=True, text=True, cwd=ROOT)
    out = set()
    for obj in [str(ROOT / o) for o in r.stdout.split()]:
        if not os.path.exists(obj) or os.path.basename(obj) == 'missing_globals_gen.o':
            continue                      # (own previous output)
        r = subprocess.run(['nm', '-g', '--defined-only', obj], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                out.add(parts[2])
    return out


OUT = ROOT / 'pc' / 'src' / 'missing_globals_gen.c'
ALIAS_OUT = ROOT / 'pc' / 'src' / 'missing_aliases.ld'
if '-o' in sys.argv:
    OUT = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
defined = defined_symbols() - {None}
# incremental: what the previous generated file and alias script provided
# stays provided (a link log only names what was undefined WITH them linked)
previous = set()
if OUT.exists():
    for line in OUT.read_text(errors='replace').splitlines():
        m = re.match(r'^(?!static\b|extern\b|#|/\*)[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?:=|;)', line)
        if m and not m.group(1).startswith('pc_retail_init'):   # the registry itself
            previous.add(m.group(1))
if ALIAS_OUT.exists():
    previous |= set(re.findall(r'PROVIDE\((\w+)\s*=', ALIAS_OUT.read_text()))
undef = set((name for line in lines('/dev/stdin') for name in re.findall(r"undefined reference to `([A-Za-z_]\w*)'", line))) | previous
undef = sorted(n for n in undef if n not in defined)
# a view's name carries its address: now_year_v, cw96_active_a, screen_w_b,
# and the decomp's CSE-breaking spellings D_800BCC48b, _sys_default_tpage_set_tbl0_w,
# coord_buf_r, kuchi_stop_w (pc/tools/audit_host_globals.py finds the
# ones a host C file defines instead)
# and the g_<ADDR> / g_<Word><ADDR> spellings (pad_trig_w, step_counter_u),
# which pc/src/missing_funcs.c once defined as empty FUNCTIONS
# (pc/tools/audit_pc_stubs.py DATA-AS-FUNC)
ALIAS_VIEW = re.compile(r'^(?:D(?:[a-z]|_arr|_byte|AT)?_|g_[A-Za-z]*)([0-9A-Fa-f]{8})(?:_?[A-Za-z0-9]\w*)?$')
# a view's base: its D_<ADDR> name while pc/ld/psx_globals.syms still
# carries it, else the object's landed name (D_800D07EC_b -> date_place)
SYMS = ROOT / 'pc' / 'ld' / 'psx_globals.syms'
psx_names = set(re.findall(r'^(\w+) = ', SYMS.read_text(), re.M)) if SYMS.exists() else set()


def view_base(addr_hex):
    base = 'D_' + addr_hex.upper()
    a = int(addr_hex, 16)
    if base not in psx_names and a in addr_name:
        return addr_name[a]
    return base


names, other, aliases = [], [], []
for n in undef:
    m = re.match(r'^D_([0-9A-Fa-f]{8})$', n)
    if m:
        names.append((n, int(m.group(1), 16)))
        continue
    m = ALIAS_VIEW.match(n)
    if m:
        aliases.append((n, view_base(m.group(1))))
        continue
    if n in name_addr:
        names.append((n, name_addr[n]))
    else:
        other.append(n)
# an alias view needs its base object; define the base here when nothing does
have = {n for n, _ in names} | defined
for view, base in aliases:
    if base not in have:
        names.append((base, name_addr[base] if base in name_addr else int(base[2:], 16)))
        have.add(base)
names = sorted({(n, a) for n, a in names if n not in manual})
alias_path = ROOT / 'pc' / 'src' / 'missing_aliases.ld'
with open(alias_path, 'w') as fh:
    fh.write('/* Generated by pc/tools/gen_missing_globals.py: typed alias views of globals.\n'
             ' * Each view is the SAME object as its D_<ADDR> base (linker-script symbol\n'
             ' * assignment; pc/Makefile passes this file to the link). */\n')
    for view, base in sorted(aliases):
        fh.write('PROVIDE(%s = %s);\n' % (view, base))
print('gen_missing_globals: %d alias views -> %s' % (len(aliases), alias_path.relative_to(ROOT)), file=sys.stderr)
if other:
    print('gen_missing_globals: %d undefined symbols are not main-binary globals (left alone): %s'
          % (len(other), ' '.join(other[:40]) + (' ...' if len(other) > 40 else '')), file=sys.stderr)
if not names:
    sys.exit(0)

# every known symbol address, to bound array sizes
known = set(addr_name)
out = git_grep(['-hoE', r'\bD_80[0-9A-F]{6}\b'], ['src', 'pc/src', 'pc/hal'])
for m in out.split():
    known.add(int(m[2:], 16))
known = sorted(known)
knownset = set(known)


_DECLS = None


def decl_for(n):
    """the most common `extern` declaration of n in the tree (one batched git
    grep for every extern, cached: thousands of names)"""
    global _DECLS
    if _DECLS is None:
        out = git_grep(['-hoE', r'extern +[A-Za-z0-9_ ]+[ *]+[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?;'],
                       ['src', 'pc'])
        _DECLS = collections.defaultdict(collections.Counter)
        for l in out.splitlines():
            m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^]]*\])?;$', l.strip())
            if m:
                _DECLS[m.group(1)][l.strip()] += 1
    c = _DECLS.get(n)
    if not c:
        return 'extern u32 %s;' % n
    # a declaration through a file-local typedef (CharBlock, Desc68, ...) is
    # unusable here: take the most common declaration whose type the generated
    # file can see (a primitive or a pc/include/game/types.h typedef), else a
    # byte array sized to the next known symbol
    for decl, _cnt in c.most_common():
        ty = re.sub(r'\b(extern|volatile|const|static)\b', '', decl.split(n)[0]).replace('*', '').strip()
        if ty in known_types():
            return decl
    return 'extern u8 %s[];' % n


_KNOWN_TYPES = None


def known_types():
    global _KNOWN_TYPES
    if _KNOWN_TYPES is None:
        _KNOWN_TYPES = {'u8', 's8', 'u16', 's16', 'u32', 's32', 'int', 'char', 'short', 'long', 'void',
                        'unsigned', 'unsigned char', 'unsigned short', 'unsigned int', 'unsigned long',
                        'signed char', 'long long', 'unsigned long long', 'float', 'double'}
        hdr = (ROOT / 'pc' / 'include' / 'game' / 'types.h').read_text(errors='replace')
        hdr = re.sub(r'/\*.*?\*/', ' ', hdr, flags=re.S)
        _KNOWN_TYPES |= set(re.findall(r'\btypedef\b[^;{}]*?\b(\w+)\s*(?:\[[^\]]*\])*\s*;', hdr))
        _KNOWN_TYPES |= set(re.findall(r'\}\s*(\w+)\s*;', hdr))
    return _KNOWN_TYPES


def elem_size(ty):
    return {'u8': 1, 's8': 1, 'char': 1, 'u16': 2, 's16': 2, 'short': 2, 'unsigned short': 2}.get(ty, 4)


def next_addr(a):
    i = bisect.bisect_right(known, a)
    return known[i] if i < len(known) else a + 4


def sym_name(v):
    return addr_name.get(v, 'D_%08X' % v)


extern_decls = []
# (C name, retail address, bytes): filled from the player's PSX.EXE at run
# time (pc/src/retail_init.c) instead of a retail-byte initialiser
runtime = []
RUNTIME_TYPES = {'u8': 1, 's8': 1, 'char': 1, 'u16': 2, 's16': 2, 'short': 2, 'unsigned short': 2,
                 'u32': 4, 's32': 4, 'int': 4, 'unsigned int': 4, 'unsigned': 4}


def ptr_ref(ty, v):
    """PS1 address -> native reference: a known symbol, else NULL."""
    if v in knownset:
        extern_decls.append('extern u8 %s[];' % sym_name(v))
        return '(%s*)%s' % (ty, sym_name(v))
    if TEXT_START <= v < IMG_END:
        # unnamed retail data blob: synthesize it from the image, bounded
        # by the next known symbol (cap 1 KiB)
        n2 = min(next_addr(v) - v, 1024)
        off = v - TEXT_START + FILE_BASE
        extern_decls.append('static u8 blob_%08X[%d]; /* retail 0x%08X, loaded at run time */'
                            % (v, n2, v))
        runtime.append(('blob_%08X' % v, v, n2))
        return '(%s*)blob_%08X' % (ty, v)
    return '(%s*)0 /* PS1 addr 0x%08X: unmapped on PC */' % (ty, v)


import io
buf = io.StringIO(); real_stdout = sys.stdout; sys.stdout = buf
print('/* Generated by pc/tools/gen_missing_globals.py -- retail initial values come from the player\'s PSX.EXE at run time (pc_retail_init) */')
print('#include "types.h"')
for n, addr in names:
    d = decl_for(n).replace('extern ', '').rstrip(';')
    m = re.match(r'([A-Za-z0-9_ ]+?)\s*(\**)\s*' + re.escape(n) + r'(\[([^\]]*)\])?$', d)
    ty, ptr, arr, dim = m.group(1).strip(), m.group(2), m.group(3), m.group(4)
    es = 4 if ptr else elem_size(ty)
    if arr:
        size = int(dim) * es if dim else max(es, next_addr(addr) - addr)
        size = min(size, 4096)
    else:
        size = es
    in_img = addr + size <= IMG_END
    if in_img:
        off = addr - TEXT_START + FILE_BASE
        raw = img[off:off + size]
    else:
        raw = None
    if arr:
        count = size // es
        if raw is None:
            print('%s %s%s[%d]; /* %s BSS */' % (ty, ptr, n, count, hex(addr)))
        elif not ptr and RUNTIME_TYPES.get(ty) == es:
            print('%s %s[%d]; /* %s retail, loaded at run time */' % (ty, n, count, hex(addr)))
            runtime.append((n, addr, count * es))
        elif es == 1:
            body = ','.join(str(b) for b in raw)
            print('%s %s%s[%d] = {%s}; /* %s retail */' % (ty, ptr, n, count, body, hex(addr)))
        else:
            fmt = {2: '<H', 4: '<I'}[es]
            vals = [struct.unpack_from(fmt, raw, i * es)[0] for i in range(count)]
            if ty.startswith('s'):
                vals = [v - (1 << (8 * es)) if v >= (1 << (8 * es - 1)) else v for v in vals]
            body = ','.join((hex(v) if not ptr else ptr_ref(ty, v)) for v in vals)
            print('%s %s%s[%d] = {%s}; /* %s retail */' % (ty, ptr, n, count, body, hex(addr)))
    else:
        if raw is None:
            print('%s %s%s; /* %s BSS */' % (ty, ptr, n, hex(addr)))
        else:
            fmt = {1: '<B', 2: '<H', 4: '<I'}[es]
            v = struct.unpack_from(fmt, raw)[0]
            if ptr:
                print('%s %s%s = %s; /* %s retail */' % (ty, ptr, n, ptr_ref(ty, v), hex(addr)))
            else:
                print('%s %s%s = 0x%X; /* %s retail */' % (ty, ptr, n, v, hex(addr)))

print('')
print('/* The initial values above that are retail bytes: copied from the')
print(' * player\'s own PSX.EXE at start-up (pc/src/retail_init.c). */')
print('#include "pc_retail_init.h"')
print('const PcRetailInit pc_retail_init[] = {')
for cname, a, size in sorted(set(runtime), key=lambda r: (r[1], r[0])):
    print('    { %s, 0x%08X, %d },' % (cname, a, size))
print('    { 0, 0, 0 }')
print('};')
print('const unsigned pc_retail_init_count = %d;' % len(set(runtime)))
sys.stdout = real_stdout
body = buf.getvalue().split('\n', 2)
text = body[0] + '\n' + body[1] + '\n' + ''.join(e + '\n' for e in sorted(set(extern_decls))) + body[2]
if str(OUT) == '-':
    sys.stdout.write(text)
else:
    OUT.write_text(text)
    print('gen_missing_globals: %d globals -> %s' % (len(names), OUT.relative_to(ROOT) if OUT.is_relative_to(ROOT) else OUT),
          file=sys.stderr)
