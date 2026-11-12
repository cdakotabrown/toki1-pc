#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find pointer-width (8-byte) host accesses to PS1-resident globals.

The PC build resolves the game's data symbols into the emulated PS1 RAM at
0x80000000 (pc/ld/psx_globals.syms via --just-symbols, PROVIDE() views in
pc/src/*.ld).  The console laid those objects out with 4-byte pointers; a
function that declares one of them as a pointer (`extern u8
*bustup_img3;`) or `long` loads and stores it 8 bytes wide on LP64.  A
store wipes the 4 bytes after the console cell (bust_up_ptr_set stored
bustup_img2 over bustup_img3, 5f74087e80) and a load returns those bytes
in the high half (a non-canonical "pointer" unless they happen to be 0).

For every object on the link line (pc/Makefile ALL_OBJS) this reads each
.text relocation against a PS1-resident name (objdump -dr; the access
model of audit_host_globals.object_accesses) and takes the direct
accesses made through a 64-bit general register (the console has no
8-byte integer access).  Register-indexed accesses with scale 8 through
a 64-bit register (`sym(,%rax,8)`: a pointer array striding 8 where the
console strides 4) are taken as WIDE-INDEX.

GCC usually reaches an indexed or offset element through a base register
instead: `lea sym(%rip),%rax` and then `mov (%rax,%rdx,8),%rsi` or
`mov 0x10(%rax),%rax`.  The relocation sits on the lea, so the access is
found by following the lea's register forward in the same function
(lea_derived_accesses) until it is overwritten, a call clobbers it or
control leaves (an unconditional jump or return); each memory operand based
on it is an access to sym at the lea's offset plus the displacement.  These
rows carry `via` (the lea).  A WIDE-INDEX row whose element type is a
pointer or long is gated like the direct rows (date_name_set's
`date_name_set_tbl0[i]` and telephone_choice_menu_init's
`telephone_choice_menu_init_tbl0[i]`: 4-byte PS1 string pointer tables
read at stride 8).

An 8-byte access is not by itself the defect: GCC merges adjacent u8/u16/
s32 member stores and copies into one 64-bit move, which writes the same
bytes the console does.  So each candidate is typed from DWARF: the
object's own compile command (`make -n -B`) is re-run with -g into a
scratch object, and the C type the symbol has at the accessed offset is
read from its debug info (typedefs, arrays, struct members resolved
down to the leaf):

  POINTER  the leaf is a pointer: 8 bytes on the host, 4 on the console
  LONG     the leaf is an 8-byte integer (`long`): 4 bytes on the console
  MERGE    the leaf is narrower; the 8 bytes span several leaves (a
           merged store/copy -- the same bytes as the console, benign)
  UNTYPED  no debug info for the symbol at that offset

POINTER and LONG rows are kept whatever follows the symbol (the console
cell is 4 bytes); MERGE and UNTYPED rows only where the next PS1-resident
symbol starts inside the 8 bytes (`cell` < 8: the distance from the
accessed address to the next PS1-resident symbol).

  WIDE-STORE   an 8-byte store
  WIDE-LOAD    an 8-byte load
  WIDE-INDEX   a scale-8 indexed access to a PS1-resident array

Columns: class, type, direction, symbol(+offset), PS1 address, cell,
function, object, live (the function wins the link), reached (--reach:
the function is named in a REACH_LIST_FILE list), leaf C type.

Exit status 1 when a live POINTER/LONG/UNTYPED WIDE-STORE or WIDE-LOAD row
is reported outside --allow (pc/tools/wide_access_allow.txt: `function`
or `object:function` lines, each with the reason it is left -- another
lane's file, or not reached), unless --no-fail.

usage: audit_ps1_wide_access.py [--build-dir pc/build] [--json OUT]
                                [--all] [--allow FILE] [--reach FILE...]
                                [--build-reach OUT] [--no-fail]

Run after `make -f pc/Makefile all`.  Reach lists: `--build-reach OUT`
links a copy of the binary whose reported objects alone are compiled with
-finstrument-functions; run it (playthrough.py --bin OUT, scene_matrix)
with REACH_LIST_FILE=<path> (pc/hal/instrument.c appends the names of the
functions entered) and pass the lists to --reach.
"""
import argparse
import bisect
import collections
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_host_globals as hg  # noqa: E402
import audit_pc_stubs as stubs  # noqa: E402

ROOT = stubs.ROOT
RAM, RAM_END = hg.RAM, hg.RAM_END
GPR64 = {n for n, w in hg.REG_WIDTH.items() if w == 8}
GATED = ('WIDE-STORE', 'WIDE-LOAD')
BENIGN = ('MERGE',)
DEFECT_TYPES = ('POINTER', 'LONG')


def resident_map(binary):
    """{name: address} for every PS1-resident symbol of the linked binary."""
    out = {}
    for name, defs in hg.nm_symbols(binary).items():
        for v, _s, _k in defs:
            if hg.in_ram(v):
                out[name] = v
    return out


def wide_register(text):
    """True when the instruction moves a 64-bit general register (or has a
    `q` integer suffix with no register operand)."""
    mnem, _, ops = text.partition(' ')
    ops = ops.split('#')[0]
    # registers outside the memory operand's parentheses
    regs = re.findall(r'%(\w+)', re.sub(r'\([^)]*\)', '', ops))
    if any(r in GPR64 for r in regs):
        return True
    if not any(r in hg.REG_WIDTH or r.startswith(('xmm', 'mm')) for r in regs):
        # an immediate store: `movq $imm, sym` is a 64-bit integer write
        return mnem.endswith('q') and not mnem.startswith('cvt')
    return False


def classify_access(name, kind, lo, hi, info, addrs, starts):
    """-> (class, address, cell) or None."""
    base = addrs[name]
    a = base + lo
    if kind == 'rw':
        if hi - lo != 8 or not wide_register(info['text']):
            return None
        i = bisect.bisect_right(starts, a)
        cell = (starts[i] - a) if i < len(starts) else None
        return ('WIDE-STORE' if info['store'] else 'WIDE-LOAD'), a, cell
    if kind == 'index':
        if re.search(r',8\)', info['text']) and wide_register(info['text']):
            return 'WIDE-INDEX', a, None
    return None


def declaration(src_text, name):
    """The source's own declaration of `name` (one line), or ''."""
    if not src_text:
        return ''
    code = re.sub(r'/\*.*?\*/', ' ', src_text, flags=re.S)
    m = re.search(r'^[^;{}()#]*\b%s\b[^;]*;' % re.escape(name), code, re.M)
    return ' '.join(m.group(0).split()) if m else ''


# ---------------------------------------------------------------- DWARF types

def compile_command(build_dir, obj):
    """The gcc command pc/Makefile uses for `obj` (a basename), as argv."""
    target = os.path.join(build_dir, 'obj', obj)
    r = subprocess.run(['make', '-s', '-n', '-B', '-f', 'pc/Makefile',
                        'BUILD_DIR=%s' % build_dir, target],
                       cwd=ROOT, capture_output=True, text=True, timeout=300)
    for line in reversed(r.stdout.splitlines()):
        if ' -c ' in line and target in line:
            return shlex.split(line)
    return None


def debug_object(argv, out, extra=('-g',)):
    """Re-run a compile command with `extra` flags into `out`; True on
    success."""
    cmd, skip = [], False
    for i, a in enumerate(argv):
        if skip:
            skip = False
            continue
        if a in ('-MF', '-MT', '-MQ', '-o'):
            skip = True
            continue
        if a in ('-MMD', '-MD', '-MP'):
            continue
        cmd.append(a)
    cmd += list(extra) + ['-o', out]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return r.returncode == 0


NAKED_RE = re.compile(r'__attribute__\s*\(\(\s*naked\b[^)]*\)\)[\w\s\*]*?\b(\w+)\s*\(')


def reach_flags(cmd):
    """-finstrument-functions for one compile, never for a naked function:
    GCC still puts the __cyg_profile_func_enter call (this_fn in %rdi) in
    front of a naked body, so `naked f() { jmp strlen }` became
    strlen(f's own address) -- SHOUGATU's dialogue laid out a 20-byte
    "name" at scene 91 in the reach binary only.  pc/include's inline
    helpers (ps1_view_addr) are not instrumented either."""
    flags = ['-finstrument-functions', '-finstrument-functions-exclude-file-list=pc/include']
    src = cmd[cmd.index('-c') + 1] if '-c' in cmd else None
    if src and os.path.exists(os.path.join(ROOT, src)):
        with open(os.path.join(ROOT, src), errors='replace') as fh:
            naked = sorted(set(NAKED_RE.findall(fh.read())))
        if naked:
            flags.append('-finstrument-functions-exclude-function-list=' + ','.join(naked))
    return tuple(flags)


def build_reach_binary(build_dir, objs, out):
    """Link a copy of the PC binary at `out` in which only the objects
    `objs` (basenames) are compiled with -finstrument-functions, so a run
    with REACH_LIST_FILE lists which of their functions execute
    (pc/hal/instrument.c).  The rest of the link is the normal build's
    objects; nothing in BUILD_DIR changes."""
    target = os.path.join(build_dir, 'tokimeki-pc')
    r = subprocess.run(['make', '-s', '-n', '-B', '-f', 'pc/Makefile',
                        'BUILD_DIR=%s' % build_dir, target],
                       cwd=ROOT, capture_output=True, text=True, timeout=300)
    link = [line for line in r.stdout.splitlines()
            if ' -o %s' % target in line and ' -c ' not in line]
    if not link:
        sys.exit('no link command for %s' % target)
    argv = shlex.split(link[-1])
    odir = os.path.join(os.path.dirname(os.path.abspath(out)), 'reach-obj')
    os.makedirs(odir, exist_ok=True)
    swap = {}

    def one(b):
        cmd = compile_command(build_dir, b)
        dst = os.path.join(odir, b)
        ok = cmd is not None and debug_object(cmd, dst, reach_flags(cmd))
        return b, dst if ok else None
    with ThreadPoolExecutor(8) as ex:
        for b, dst in ex.map(one, sorted(objs)):
            if dst is None:
                sys.exit('could not instrument %s' % b)
            swap[os.path.join(build_dir, 'obj', b)] = dst
    argv = [swap.get(a, a) for a in argv]
    argv[argv.index('-o') + 1] = os.path.abspath(out)
    subprocess.run(argv, cwd=ROOT, check=True)
    return len(swap)


def _attr(die, name, default=None):
    a = die.attributes.get(name)
    return a.value if a is not None else default


def _strip(die):
    while die is not None and die.tag in ('DW_TAG_typedef', 'DW_TAG_const_type',
                                          'DW_TAG_volatile_type', 'DW_TAG_restrict_type',
                                          'DW_TAG_atomic_type'):
        if 'DW_AT_type' not in die.attributes:
            return None
        die = die.get_DIE_from_attribute('DW_AT_type')
    return die


def _size(die):
    die = _strip(die)
    if die is None:
        return None
    if die.tag == 'DW_TAG_array_type':
        elem = die.get_DIE_from_attribute('DW_AT_type')
        n = 1
        for sub in die.iter_children():
            if sub.tag == 'DW_TAG_subrange_type':
                if 'DW_AT_count' in sub.attributes:
                    n *= _attr(sub, 'DW_AT_count')
                elif 'DW_AT_upper_bound' in sub.attributes:
                    n *= _attr(sub, 'DW_AT_upper_bound') + 1
                else:
                    return None
        es = _size(elem)
        return es * n if es is not None else None
    return _attr(die, 'DW_AT_byte_size')


def _name(die):
    die0 = die
    while die is not None and 'DW_AT_name' not in die.attributes and \
            'DW_AT_type' in die.attributes and die.tag != 'DW_TAG_pointer_type':
        die = die.get_DIE_from_attribute('DW_AT_type')
    n = _attr(die, 'DW_AT_name') if die is not None else None
    if isinstance(n, bytes):
        n = n.decode()
    if die0 is not None and _strip(die0) is not None and \
            _strip(die0).tag == 'DW_TAG_pointer_type':
        return (n + ' *') if n and die0.tag != 'DW_TAG_pointer_type' else 'pointer'
    return n or die0.tag[7:] if die0 is not None else '?'


def leaf_type(die, off):
    """(kind, size, c_name) of the scalar the type `die` holds at byte `off`.

    kind: 'pointer', 'int' (base/enum), or None when the offset falls in
    padding or the type is unknown."""
    named = die
    die = _strip(die)
    if die is None:
        return None, None, '?'
    tag = die.tag
    if tag == 'DW_TAG_pointer_type':
        return 'pointer', _attr(die, 'DW_AT_byte_size', 8), _name(named)
    if tag in ('DW_TAG_base_type', 'DW_TAG_enumeration_type'):
        return 'int', _attr(die, 'DW_AT_byte_size'), _name(named)
    if tag == 'DW_TAG_array_type':
        elem = die.get_DIE_from_attribute('DW_AT_type')
        es = _size(elem)
        if not es:
            return None, None, '?'
        return leaf_type(elem, off % es)
    if tag in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
        for m in die.iter_children():
            if m.tag != 'DW_TAG_member' or 'DW_AT_type' not in m.attributes:
                continue
            loc = _attr(m, 'DW_AT_data_member_location', 0)
            if not isinstance(loc, int):
                continue
            t = m.get_DIE_from_attribute('DW_AT_type')
            sz = _size(t)
            if sz is not None and loc <= off < loc + sz:
                return leaf_type(t, off - loc)
        return None, None, 'padding'
    return None, None, tag


def variable_types(path, names):
    """{name: type DIE} for the variables `names` in a debug object."""
    from elftools.elf.elffile import ELFFile
    out = {}
    with open(path, 'rb') as fh:
        elf = ELFFile(fh)
        if not elf.has_dwarf_info():
            return out
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag != 'DW_TAG_variable' or 'DW_AT_type' not in die.attributes:
                    continue
                n = _attr(die, 'DW_AT_name')
                if isinstance(n, bytes):
                    n = n.decode()
                if n in names and n not in out:
                    out[n] = die.get_DIE_from_attribute('DW_AT_type')
    return out



_ADDR_NAMES = None


def _address_names():
    """{PS1 address: {names}} from pc/ld/psx_globals.syms (every name the
    port binds at that address: the D_ spelling and any landed name)."""
    global _ADDR_NAMES
    if _ADDR_NAMES is None:
        _ADDR_NAMES = {}
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ld', 'psx_globals.syms')
        try:
            for line in open(path):
                m = re.match(r'^(\w+) = 0x([0-9A-Fa-f]{8});', line)
                if m:
                    _ADDR_NAMES.setdefault(int(m.group(2), 16), set()).add(m.group(1))
        except OSError:
            pass
    return _ADDR_NAMES


def type_rows(build_dir, obj, rows, argv=None):
    """Set each row's `type` (POINTER/LONG/MERGE/UNTYPED) and `ctype`.

    argv: the compile command (default: the one pc/Makefile uses for obj)."""
    argv = argv or compile_command(build_dir, obj)
    types = {}
    if argv:
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, 'g.o')
            if debug_object(argv, out):
                # a renamed object is declared under its landed name while
                # the access may resolve to its D_<ADDR> spelling (or the
                # reverse): look the type up under every name bound to the
                # row's address (pc/ld/psx_globals.syms)
                alias = _address_names()
                want = {r['name'] for r in rows}
                for r in rows:
                    want |= alias.get(r.get('address'), set())
                types = variable_types(out, want)
                for r in rows:
                    t = types.get(r['name'])
                    if t is None:
                        for n in sorted(alias.get(r.get('address'), ())):
                            if n in types:
                                t = types[n]
                                break
                    kind, size, cname = leaf_type(t, r['offset']) if t is not None \
                        else (None, None, '?')
                    r['ctype'] = cname
                    if kind == 'pointer':
                        r['type'] = 'POINTER'
                    elif kind == 'int' and size == 8:
                        r['type'] = 'LONG'
                    elif kind == 'int' and size is not None and size < 8:
                        r['type'] = 'MERGE'
                    else:
                        r['type'] = 'UNTYPED'
                    if r.get('pair'):
                        # two 4-byte constants merged into one store: the
                        # PS1 bytes, whatever the declared type
                        r['type'], r['ctype'] = 'MERGE', 'u32 pair (%s)' % cname
                return
    for r in rows:
        r['type'], r['ctype'] = ('MERGE', 'u32 pair') if r.get('pair') else ('UNTYPED', '?')


CALLER_SAVED = {'rax', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11'}
MEMREF = re.compile(r'(-?0x[0-9a-f]+|-?\d+)?\(%(\w+)(?:,%(\w+),(\d))?\)')
LEA = re.compile(r'^lea[q]?$')


def _reg64(name):
    """the 64-bit register a general register name belongs to, or None"""
    name = name.lstrip('%')
    if name in GPR64:
        return name
    m = re.fullmatch(r'e?([abcd])[xlh]', name)
    if m:
        return 'r' + m.group(1) + 'x'
    m = re.fullmatch(r'e?(si|di|bp|sp)l?', name)
    if m:
        return 'r' + m.group(1)
    m = re.fullmatch(r'(r\d+)[dwb]?', name)
    if m:
        return m.group(1)
    return None


def parse_insns(path, names):
    """[[address, mnemonic, operands, function, lea, reloc]] of the object's
    text; lea = (name, offset) for `lea name+offset(%rip),%reg` against
    `names`; reloc = the symbol of the instruction's first relocation."""
    insns, func = [], None
    for line in hg.sh(['objdump', '-dr', str(path)]).splitlines():
        f = hg.FUNC_LABEL.match(line)
        if f:
            func = f.group(1)
            continue
        m = hg.INSN.match(line)
        if m:
            if m.group(3):
                insns.append([int(m.group(1), 16), m.group(3), (m.group(4) or '').split('#')[0].strip(),
                              func, None, None])
            continue
        r = hg.RELOC.match(line)
        if r and insns and insns[-1][5] is None:
            insns[-1][5] = r.group(3)
        if r and insns and r.group(2) in ('R_X86_64_PC32',) and r.group(3) in names \
                and LEA.match(insns[-1][1]):
            addend = int(r.group(4), 16) if r.group(4) else 0
            # PC32 addend is -4 relative to the end of the 7-byte lea
            insns[-1][4] = (r.group(3), addend + 4)
        # `call ps1_ref_NAME` (pc/include/game/ps1_alias_refs.h) returns
        # NAME's address in %rax: the same base as `lea NAME(%rip),%rax`
        if r and insns and insns[-1][1].startswith('call') and r.group(3).startswith(REF_PREFIX) \
                and r.group(3)[len(REF_PREFIX):] in names:
            insns[-1][4] = (r.group(3)[len(REF_PREFIX):], 0)
    return insns


REF_PREFIX = 'ps1_ref_'


MOVABS = re.compile(r'^\$(0x[0-9a-f]+),%(\w+)$')
LITERAL = re.compile(r'^0x0\(%rip\),%(\w+)$')


def constant_pair(insns, at):
    """True when the 8-byte store at insns[at] writes a register loaded
    (within the last few instructions of the function) by `movabs $X` with
    a non-zero high half: GCC merging two adjacent 4-byte constant stores
    (`*(u32 *)&tbl[i] = A; *(u32 *)&tbl[i+1] = B;`, the generated
    PS1-width view), not a pointer -- whose high half is 0 on the PC."""
    ops = hg.split_operands(insns[at][2])
    if len(ops) != 2 or not ops[0].startswith('%'):
        return False
    reg = _reg64(ops[0].lstrip('%'))
    for j in range(at - 1, max(-1, at - 8), -1):
        _a, mnem, jops, func, _l, rsym = insns[j]
        if func != insns[at][3]:
            return False
        m = MOVABS.match(jops) if mnem.startswith('movabs') else None
        if m and _reg64(m.group(2)) == reg:
            return int(m.group(1), 16) >> 32 != 0
        m = LITERAL.match(jops) if mnem in ('mov', 'movq') else None
        if m and _reg64(m.group(1)) == reg and (rsym or '').startswith('.LC'):
            # an 8-byte literal from .rodata: a PS1 pointer constant
            # (< 2^32) would be a zero-extending `mov $imm,%eax`
            return True
        jl = hg.split_operands(jops)
        if jl and jl[-1].startswith('%') and _reg64(jl[-1].lstrip('%')) == reg:
            return False
    return False


def lea_derived_accesses(path, names, insns=None):
    """[(name, kind, lo, hi, info)] for memory operands based on a register
    loaded by `lea name(%rip),%reg` (see the module docstring)."""
    insns = insns if insns is not None else parse_insns(path, names)
    out = []
    for i, (iaddr, mnem, ops, ifunc, lea, _r) in enumerate(insns):
        if not lea:
            continue
        opl = hg.split_operands(ops)
        if mnem.startswith('call'):
            opl = [ops, '%rax']          # call ps1_ref_NAME: the address in %rax
        if len(opl) != 2:
            continue
        base = _reg64(opl[1].lstrip('%'))
        if base is None:
            continue
        name, k = lea
        bases = {base}
        for j in range(i + 1, min(len(insns), i + 24)):
            jaddr, jm, jops, jfunc, _, _r = insns[j]
            if jfunc != ifunc:
                break
            jl = hg.split_operands(jops)
            for slot, o in enumerate(jl):
                mm = MEMREF.fullmatch(o)
                if not mm or mm.group(2) not in bases or jm.startswith('lea'):
                    continue
                disp = int(mm.group(1), 0) if mm.group(1) else 0
                text = (jm + ' ' + jops).strip()
                store = slot == len(jl) - 1 and len(jl) > 1 and not jm.startswith(hg.READ_ONLY)
                info = dict(func=ifunc, insn=jaddr, text=text, store=store,
                            via='%x: %s %s' % (iaddr, mnem, ops))
                if mm.group(4) == '8':
                    out.append((name, 'index', k + disp, k + disp, info))
                else:
                    w = hg.access_width(jm, jl, slot)
                    out.append((name, 'rw', k + disp, k + disp + w, info))
            if jm.startswith(('jmp', 'ret')):
                break
            if jm.startswith('call'):
                bases -= CALLER_SAVED
                if not bases:
                    break
                continue
            # a 64-bit copy of a base (`mov %rax,%rbx`, before a second
            # ps1_ref_ call) is a base too
            if jm == 'mov' and len(jl) == 2 and jl[0].startswith('%') and jl[1].startswith('%') \
                    and jl[0].lstrip('%') in bases and jl[1].lstrip('%') in GPR64:
                bases.add(jl[1].lstrip('%'))
                continue
            # the register is written: the last operand, a register of its family
            if jl and not jm.startswith(hg.READ_ONLY) and not jm.startswith(('push', 'j')):
                dst = jl[-1]
                if dst.startswith('%') and _reg64(dst.lstrip('%')) in bases:
                    bases.discard(_reg64(dst.lstrip('%')))
                    if not bases:
                        break
    return out


def scan_object(path, names, addrs, starts):
    rows, seen = [], set()
    insns = parse_insns(path, names)
    at = {ins[0]: i for i, ins in enumerate(insns)}
    accesses = hg.object_accesses(path, names, detail=True) + \
        lea_derived_accesses(path, names, insns)
    for name, kind, lo, hi, info in accesses:
        c = classify_access(name, kind, lo, hi, info, addrs, starts)
        if c:
            key = (info['func'], info['insn'], name, lo)
            if key in seen:
                continue       # two relocations on one instruction
            seen.add(key)
            rows.append(dict(cls=c[0], name=name, offset=lo, address=c[1], cell=c[2],
                             func=info['func'], insn=info['text'],
                             insn_addr=info['insn'], store=info['store'],
                             via=info.get('via'),
                             pair=info['store'] and info['insn'] in at and
                             constant_pair(insns, at[info['insn']])))
    return rows


def audit(build_dir='pc/build', binary=None, typed=True):
    order, srcmap, tiers, defs, undef = stubs.build_index(build_dir)
    binary = binary or os.path.join(ROOT, build_dir, 'tokimeki-pc')
    addrs = resident_map(binary)
    starts = sorted(set(addrs.values()))
    winner = {}
    for o in order:
        b = os.path.basename(o)
        for typ, name in defs.get(b, []):
            if typ in 'TW' and (name not in winner or
                                (typ == 'T' and winner[name][1] != 'T')):
                if name not in winner or winner[name][1] != 'T':
                    winner[name] = (b, typ)
    obj_dir = os.path.join(ROOT, build_dir, 'obj')
    work = []
    for o in order:
        b = os.path.basename(o)
        names = {n for _t, n in undef.get(b, []) if n in addrs}
        # a resident name the object DEFINES is a host shadow
        # (audit_host_globals.py), not this class
        if names:
            work.append((b, names))

    def one(item):
        b, names = item
        found = scan_object(os.path.join(obj_dir, b), names, addrs, starts)
        if found and typed:
            type_rows(build_dir, b, found)
        # a pointer/long is 4 bytes on the console whatever follows it; an
        # untyped or merged access matters only where the next PS1 symbol
        # starts inside the 8 bytes
        found = [r for r in found if r.get('type') in DEFECT_TYPES or
                 r['cls'] == 'WIDE-INDEX' or
                 (r['cell'] is not None and r['cell'] < 8)]
        return b, found
    rows = []
    with ThreadPoolExecutor(8) as ex:
        for b, found in ex.map(one, work):
            src = srcmap.get(b, '')
            text = None
            for r in found:
                if text is None:
                    text = stubs.read_text(src) if src else ''
                r.update(obj=b, src=src, tier=tiers.get(b),
                         live=winner.get(r['func'], (None,))[0] == b,
                         decl=declaration(text, r['name']))
                r.setdefault('type', 'UNTYPED')
                r.setdefault('ctype', '?')
                rows.append(r)
    rows.sort(key=lambda r: (r['cls'], not r['live'], r['func'] or '', r['address']))
    return rows


def load_allow(path):
    out = set()
    if path and os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                line = line.split('#')[0].strip()
                if line:
                    out.add(line)
    return out


def allowed(r, allow):
    return r['func'] in allow or '%s:%s' % (r['obj'], r['func']) in allow


def gated(r, allow):
    return r['live'] and not allowed(r, allow) and (
        (r['cls'] in GATED and r['type'] not in BENIGN) or
        (r['cls'] == 'WIDE-INDEX' and r['type'] in DEFECT_TYPES))


def fmt(r):
    reached = {True: 'reached', False: '-', None: ''}[r.get('reached')]
    return '%-10s %-7s %-5s %-24s 0x%08X %4s %-32s %-28s %-4s %-7s %s' % (
        r['cls'], r['type'], 'store' if r['store'] else 'load',
        r['name'] + ('+%d' % r['offset'] if r['offset'] else ''),
        r['address'], r['cell'] if r['cell'] is not None else '-',
        r['func'], r['obj'], 'live' if r['live'] else 'dead', reached,
        r['ctype'])


def load_reach(paths):
    out = set()
    for p in paths or ():
        with open(p) as fh:
            out |= {line.strip() for line in fh if line.strip()}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default='pc/build')
    ap.add_argument('--binary')
    ap.add_argument('--json')
    ap.add_argument('--all', action='store_true',
                    help='also print dead, MERGE and WIDE-INDEX rows')
    ap.add_argument('--allow', default=os.path.join(ROOT, 'pc/tools/wide_access_allow.txt'))
    ap.add_argument('--reach', nargs='+', help='REACH_LIST_FILE lists (function names)')
    ap.add_argument('--build-reach', metavar='OUT',
                    help='link OUT with the reported objects instrumented, then exit')
    ap.add_argument('--also', nargs='+', default=[], metavar='OBJ',
                    help='with --build-reach: instrument these objects too '
                         '(e.g. the pc-port overrides of fixed writers)')
    ap.add_argument('--no-fail', action='store_true')
    a = ap.parse_args(argv)
    if not os.path.exists(os.path.join(ROOT, a.build_dir, 'obj', '.srcmap')):
        sys.exit('no %s/obj/.srcmap: build first (make -f pc/Makefile all)' % a.build_dir)
    rows = audit(a.build_dir, a.binary)
    allow = load_allow(a.allow)
    reach = load_reach(a.reach)
    if a.build_reach:
        # game sources only: instrumenting pc/hal (libpress, gpu) stalls
        # the boot movie
        objs = {r['obj'] for r in rows if r['live'] and
                r['src'].startswith(('src/main/', 'pc/src/overlay_'))}
        objs |= set(a.also)
        n = build_reach_binary(a.build_dir, objs, a.build_reach)
        print('linked %s with %d instrumented objects; run it with '
              'REACH_LIST_FILE=<list>' % (a.build_reach, n))
        return 0
    for r in rows:
        r['reached'] = (r['func'] in reach) if a.reach else None
        r['allowed'] = allowed(r, allow)
    if a.json:
        with open(a.json, 'w') as fh:
            json.dump(rows, fh, indent=1)
    for r in rows:
        if a.all or (r['live'] and r['cls'] in GATED and r['type'] not in BENIGN) or \
                (r['live'] and r['cls'] == 'WIDE-INDEX' and r['type'] in DEFECT_TYPES):
            print(fmt(r) + ('  [allowed]' if r['allowed'] else ''))
    print('\ncensus (accesses / functions; live = the function wins the link):')
    for c in GATED + ('WIDE-INDEX',):
        for t in ('POINTER', 'LONG', 'UNTYPED', 'MERGE'):
            sel = [r for r in rows if r['cls'] == c and r['type'] == t]
            if not sel:
                continue
            live = [r for r in sel if r['live']]
            extra = ''
            if a.reach:
                rr = {r['func'] for r in live if r['reached']}
                extra = '  reached %d functions' % len(rr)
            print('  %-10s %-7s live %3d / %3d  dead %3d%s' % (
                c, t, len(live), len({r['func'] for r in live}), len(sel) - len(live), extra))
    bad = [r for r in rows if gated(r, allow)]
    if bad:
        print('\n%d live wide accesses outside --allow (%d functions)' % (
            len(bad), len({r['func'] for r in bad})))
        return 0 if a.no_fail else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
