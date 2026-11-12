#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_pc_stubs.py -- find PC stubs and shims that hide the real game code.

The PC link (pc/Makefile) uses --allow-multiple-definition: the FIRST
definition on the link line wins (pc-port, matched, semi, archive, HAL,
pc/src, overlay modules).  A `return 0` stub in pc/src/missing_funcs.c wins
whenever no game tier defines the NAME -- including when the matched C exists
but defines the body under ANOTHER name (a head-absorbed window whose C names
the prologue address, a renamed file).  That is how week_day_bg_load
(0x80073AF4, matched body `func_80073AFC`) stayed a `return 0` until
2026-10-30 and scene 64 never loaded its background.

For every function defined by a PC-side object -- pc/src (stub files,
thunk/alias files, trampolines; not the generated overlay modules) and the
src/main/pc-port overrides -- this tool reports:

  * wins     -- is this the definition the linker keeps?
  * body     -- the compiled x86-64 body: empty / const / passthru (a trivial
                stub), forward (a tail jump to another symbol), real;
  * matched  -- does a matched C body exist: the same name in a game tier,
                or an ALIAS: the registry file for this name (or for the
                window containing its address) defines the body under
                another name (config/level3_functions.json,
                config/symbol_names.json, config/main_symbol_boundaries.json,
                config/overlay/<OV>.sym);
  * reached  -- who refers to it: game objects (undefined refs in matched/
                semi/archive/overlay objects), pc-port/pc code, the PS1
                address table (pc/src/psx_main_map.c), and retail (a jal in
                SLPS_000.65 or a loaded overlay EXN, or a 32-bit address word
                in SLPS_000.65's tables).  "Reached by game code" = a game
                object or retail reference; EVENT/GYOZI (never loaded) and
                chance address words inside EXNs do not count.  This is a
                static call graph: a runtime probe is still the evidence that
                a scene executes it.

Classes (one per definition):
  DATA-AS-FUNC        a PC-side FUNCTION definition that game code reads or
                      writes as DATA: a load/store relocation (not a call, a
                      tail jump or an address taken) against the name in a
                      game object, or a name that spells the address of a
                      main-binary PS1 data symbol (pc/ld/psx_globals.syms)
                      and is not a main-binary function.  The game then reads host code
                      bytes and its stores go nowhere (`void
                      step_counter_u() {}` made title_fade_out's
                      128-frame wait on step_counter end after one frame,
                      2026-10-31).  The name must resolve to PS1 RAM (a
                      PROVIDE() in pc/src/missing_aliases.ld).  Gated like
                      STUB-HIDES-MATCHED.
  STUB-HIDES-MATCHED  trivial body wins the link, a matched C body exists,
                      something reaches it.  Exit status 1 when any is
                      reached by GAME code (game object or retail).
  STUB-NO-MATCH       trivial body wins, no C body (asm_only / unmatched /
                      no registry entry) -- noted with the status.
  OVERRIDE            non-trivial body wins over a matched body (pc-port
                      override); `doc` says whether its header documents
                      the divergence.
  SHADOWS-MATCHED     a non-stub (forwarder or override) wins over a matched
                      body of the SAME name that is also on the link line --
                      a multi-symbol pc-port file, not a basename override;
                      usually a stale head forwarder.  Gated like
                      STUB-HIDES-MATCHED.
  FORWARDER           wins, tail-jumps to another symbol (alias/thunk).
  PC-IMPL             non-trivial, no matched body (PC-only code).
  SHADOWED            loses the link to another definition: inert text.
  DEAD-OVERLAY        referenced only by the never-loaded EVENT/GYOZI modules
                      (it satisfies their link; no game path reaches it).
  DEAD                no reference from anywhere (PC or retail): removable.

    python3 pc/tools/audit_pc_stubs.py                # summary + gating list
    python3 pc/tools/audit_pc_stubs.py --all          # every row
    python3 pc/tools/audit_pc_stubs.py --json out.json
    python3 pc/tools/audit_pc_stubs.py --class OVERRIDE --undocumented

A matched file kept out of the link (pc/Makefile HAL_EXCLUDES, a pc-port
basename override) shows as `<file>(unlinked)`; a STUB-HIDES-MATCHED row
with that note is a stale exclude (cursor_select, hizuke_advance until
2026-10-31).  `audit_pc_link.py --hal-excludes` lists excludes that name
nothing.

Run after `make -f pc/Makefile all` (it reads the objects in BUILD_DIR).
"""
import argparse
import bisect
import collections
import json
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP, spell, decomp_key, disc_input  # noqa: E402

GAME_TIERS = ('matched', 'semi', 'archive', 'overlay', 'overlay_init')
AUDITED_TIERS = ('pcport', 'pcsrc')
TRIVIAL = ('empty', 'const', 'passthru')
# The retail loader never loads these EXNs (docs: EVENT/GYOZI are dead
# overlays); their jal targets into main are phantoms, not evidence.
DEAD_OVERLAYS = ('EVENT', 'GYOZI')
# Generated trampolines (pc/tools/gen_overlay_symtab.py): address spellings
# that tail-jump into the loaded overlay module, not stubs.
TRAMPOLINE_OBJS = ('overlay_symtab.o',)
OVERLAY_BASE = 0x80132000

# Words in an override header that say it documents a divergence from the
# PS1 body (PC Port Policies: "Intentional divergence MUST be documented").
DOC_WORDS = re.compile(
    r'diverg|differ|instead|replac|unlike|PC[- ]only|PC[- ]side|PC[- ]port '
    r'(?:shim|adapter|override|replacement)|native|host|64-bit|pointer[- ]width|'
    r'neutraliz|adapter|shim|deviat|not (?:the|a) (?:faithful|retail)|'
    r'PC override|on the PC|PS1 (?:handle|offset|width|layout)|entry point', re.I)


def tier_of(src):
    s = src.replace('\\', '/')
    if s.startswith('src/main/pc-port/'):
        return 'pcport'
    if s.startswith('src/main/matched/'):
        return 'matched'
    if s.startswith('src/main/semi-matched/'):
        return 'semi'
    if s.startswith('src/main/archive/'):
        return 'archive'
    if s.startswith('pc/hal/'):
        return 'hal'
    if s.startswith('pc/src/overlay_inits/'):
        return 'overlay_init'
    if re.match(r'pc/src/overlay_[A-Z0-9_]+\.c$', s) and not s.endswith('overlay_symtab.c'):
        return 'overlay'
    if s.startswith('pc/src/'):
        return 'pcsrc'
    return 'other'


# ---------------------------------------------------------------- body shape

_SKIP = re.compile(r'^(endbr64|nop|xchg\s+%ax,%ax|data16|cs nop|int3$)')


def classify_body(insns):
    """insns: list of (mnemonic+operands, reloc_target or None).

    Returns (kind, detail): kind in empty/const/passthru/forward/real.
    """
    body = []
    for text, reloc in insns:
        t = text.strip()
        if not t or _SKIP.match(t):
            continue
        if reloc == '__fentry__' and t.startswith('call'):
            continue    # -pg -mfentry entry hook (pc/hal/cpu_cost.c), not the body
        if re.match(r'(sub|add)\s+\$0x[0-9a-f]+,%rsp$', t):
            continue    # the stack alignment -pg adds around that call
        body.append((t, reloc))
        if t.startswith(('ret', 'jmp', 'ud2')):
            break
    if not body:
        return 'empty', ''
    last, reloc = body[-1]
    pre = body[:-1]
    if last.startswith('jmp'):
        if reloc and not pre:
            return 'forward', reloc
        if reloc and all(p[0].startswith(('mov', 'xor', 'lea', 'movs', 'movz', 'cltq', 'cdqe'))
                         and not p[1] for p in pre):
            return 'forward', reloc
        return 'real', ''
    if not last.startswith('ret'):
        return 'real', ''
    if not pre:
        return 'empty', ''
    # `call X; ret` is the same forwarder with sibling calls off (cost
    # objects build with -fno-optimize-sibling-calls, timing round 6)
    if pre[-1][0].startswith('call') and pre[-1][1] and \
            all(p[0].startswith(('mov', 'xor', 'lea', 'movs', 'movz', 'cltq', 'cdqe'))
                and not p[1] for p in pre[:-1]):
        return 'forward', pre[-1][1]
    if len(pre) == 1:
        p = pre[0][0]
        m = re.match(r'xor\s+%(e|r)ax,%(e|r)ax$', p)
        if m:
            return 'const', '0'
        m = re.match(r'mov[lq]?\s+\$(0x[0-9a-f]+|-?\d+),%(e|r)ax$', p)
        if m:
            return 'const', m.group(1)
        if re.match(r'mov[lq]?\s+%[re]?(di|si|dx|cx|8|9)d?,%[re]ax$', p) or \
           re.match(r'mov\s+%(rdi|rsi|rdx|rcx|r8|r9),%rax$', p):
            return 'passthru', p.split()[1].split(',')[0]
    return 'real', ''


def parse_objdump(text):
    """objdump -d -r --no-show-raw-insn output -> {sym: [(insn, reloc)]}."""
    out = {}
    cur = None
    for line in text.splitlines():
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
        if m:
            cur = out.setdefault(m.group(1), [])
            continue
        if cur is None:
            continue
        m = re.match(r'^\s+[0-9a-f]+:\s+R_X86_64_\w+\s+(\S+?)(?:[-+]0x[0-9a-f]+)?$', line)
        if m:
            if cur:
                t, _ = cur[-1]
                cur[-1] = (t, m.group(1))
            continue
        m = re.match(r'^\s+[0-9a-f]+:\s+(.*)$', line)
        if m:
            cur.append((m.group(1), None))
    return out


# ---------------------------------------------------------------- build data

def make_var(build_dir, var):
    cmd = ['make', '-s', '--no-print-directory', '-f', 'pc/Makefile',
           'BUILD_DIR=%s' % build_dir,
           '--eval', 'print-objs: ; @echo $(%s)' % var, 'print-objs']
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=120)
    if r.returncode:
        sys.exit('make print-objs failed: %s' % r.stderr[-2000:])
    return r.stdout.split()


def load_srcmap(obj_dir):
    m = {}
    p = os.path.join(obj_dir, '.srcmap')
    with open(p) as fh:
        text = fh.read()
    for line in text.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            m[parts[0]] = decomp_key(parts[1].strip())
    return m


def nm_all(objs, flags):
    """{obj_basename: [(type, name)]} via one nm per chunk."""
    res = collections.defaultdict(list)
    for i in range(0, len(objs), 800):
        chunk = objs[i:i + 800]
        r = subprocess.run(['nm', '-A'] + flags + chunk, cwd=ROOT,
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            path, _, rest = line.partition(':')
            f = rest.split()
            if len(f) >= 2:
                res[os.path.basename(path)].append((f[-2], f[-1]))
    return res


# ---------------------------------------------------------------- registries

def read_text(path):
    with open(os.path.join(ROOT, spell(path)), errors='replace') as fh:
        return fh.read()


def read_json(path):
    return json.loads(read_text(path))


def load_registry():
    reg = {}          # name -> dict(address,size,source)
    try:
        for f in read_json('config/level3_functions.json')['functions']:
            reg[f['name']] = dict(address=int(f['address'], 16), size=f['size'],
                                  source=f['source'])
    except (OSError, ValueError, KeyError):
        pass
    return reg


def load_symbol_names():
    out = {}
    try:
        d = read_json('config/symbol_names.json')
        for a, v in d.get('symbols', {}).items():
            out.setdefault(v.get('name'), int(a, 16))
    except (OSError, ValueError):
        pass
    return out


def load_boundaries():
    out = {}
    try:
        d = read_json('config/main_symbol_boundaries.json')
        for a, v in d.get('boundaries', {}).items():
            if v.get('true_start'):
                out[int(a, 16)] = int(v['true_start'], 16)
    except (OSError, ValueError):
        pass
    return out


def load_overlay_syms():
    """(OV, name) -> (addr, size), and (OV, addr) -> name.

    config/overlay/<OV>.sym lines are `0xADDR:SIZE:NAME`."""
    out, by_addr = {}, {}
    d = os.path.join(ROOT, spell('config/overlay'))
    if not os.path.isdir(d):
        return out, by_addr
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.sym'):
            continue
        ov = fn[:-4]
        for line in read_text(os.path.join('config/overlay', fn)).splitlines():
            p = line.strip().split(':')
            if len(p) < 3 or line.startswith('#'):
                continue
            try:
                a, n = int(p[0], 16), int(p[1], 0)
            except ValueError:
                continue
            out.setdefault((ov, p[2].strip()), (a, n))
            by_addr.setdefault((ov, a), p[2].strip())
    # a window widened over an absorbed head records its true start only in
    # the C header (`@SymbolStart: 0x...`); the .sym keeps the labelled entry
    src = os.path.join(ROOT, spell('src/overlay'))
    for ov in sorted(os.listdir(src)) if os.path.isdir(src) else ():
        dd = os.path.join(src, ov)
        if not os.path.isdir(dd):
            continue
        for fn in os.listdir(dd):
            if not fn.endswith('.c'):
                continue
            with open(os.path.join(dd, fn), errors='replace') as fh:
                head = fh.read(3000)
            m = re.search(r'(?m)^[ \t]*(?:/\*|\*)?[ \t]*@SymbolStart:[ \t]*0x([0-9A-Fa-f]{8})', head)
            if m:
                by_addr[(ov, int(m.group(1), 16))] = fn[:-2]
    return out, by_addr


def load_psx_map():
    out = {}
    p = os.path.join(ROOT, 'pc/src/psx_main_map.c')
    if os.path.exists(p):
        for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{8})u,\s*\(void \*\)(\w+)\s*\}',
                             read_text(p)):
            out.setdefault(m.group(2), int(m.group(1), 16))
    return out


def asm_only_names():
    out = set()
    p = os.path.join(ROOT, spell('config/c_unmatchable.txt'))
    if os.path.exists(p):
        for line in read_text(p).splitlines():
            s = line.strip()
            if s and not s.startswith('#'):
                out.add(s.split()[0])
    return out


# ---------------------------------------------------------------- retail refs

class Retail:
    def __init__(self):
        self.images = []          # (label, base, bytes)
        exe = str(disc_input('disks/SLPS_000.65'))
        if os.path.exists(exe):
            with open(exe, 'rb') as fh:
                b = fh.read()
            t_addr, t_size = struct.unpack_from('<II', b, 0x18)
            self.images.append(('main', t_addr, b[0x800:0x800 + t_size]))
        for d in ('assets/overlays', 'pc/assets/overlays'):
            p = str(disc_input(d))
            if os.path.isdir(p):
                for fn in sorted(os.listdir(p)):
                    if fn.upper().endswith('.EXN'):
                        with open(os.path.join(p, fn), 'rb') as fh:
                            self.images.append((fn[:-4].upper(), OVERLAY_BASE, fh.read()))
                break
        self.words = {}
        for label, base, data in self.images:
            n = len(data) // 4
            self.words[label] = struct.unpack_from('<%dI' % n, data, 0)

    @property
    def available(self):
        return bool(self.images)

    def refs(self, addr, only=None):
        """[(image, 'jal'|'word', count)] for references to addr."""
        jal = (3 << 26) | ((addr >> 2) & 0x3FFFFFF)
        out = []
        for label, words in self.words.items():
            if only and label not in only:
                continue
            # an overlay-window address means something else in each EXN
            if addr >= OVERLAY_BASE and only is None and label != 'main':
                continue
            if label in DEAD_OVERLAYS:
                continue
            nj = words.count(jal)
            # an address word in an overlay is mostly chance; main's data
            # holds the code-pointer tables
            nw = words.count(addr) if label == 'main' or only else 0
            if nj:
                out.append((label, 'jal', nj))
            if nw:
                out.append((label, 'word', nw))
        return out


# ---------------------------------------------------------------- audit

def header_comment(path, sym=None):
    try:
        t = read_text(path)
    except OSError:
        return ''
    m = re.match(r'\s*/\*(.*?)\*/', t, re.S)
    head = m.group(1) if m else ''
    if sym:
        # the comment immediately above this symbol's definition
        mm = re.search(r'/\*((?:(?!\*/).)*)\*/\s*(?:[\w\s\*]+?)\b%s\s*\(' % re.escape(sym), t, re.S)
        if mm:
            head += mm.group(1)
    return head


# a PS1 address spelled inside a symbol name (pad_trig_w, step_counter_u)
ADDR_IN_NAME = re.compile(r'(?<![0-9A-Fa-f])(80[0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
CALL_RELOCS = ('R_X86_64_PLT32',)


def load_psx_data_addrs():
    """{address: [names]} of the PS1 data symbols (pc/ld/psx_globals.syms)."""
    out = collections.defaultdict(list)
    for name, val in re.findall(r'^(\w+) = (0x[0-9A-Fa-f]+);',
                                read_text('pc/ld/psx_globals.syms'), re.M):
        out[int(val, 16)].append(name)
    return out


def text_reloc_candidates(path, names):
    """Names in `names` that a .text relocation other than a call/jmp
    (R_X86_64_PLT32) refers to in the object at `path`."""
    r = subprocess.run(['objdump', '-r', path], cwd=ROOT, capture_output=True, text=True)
    hits, section = set(), None
    for line in r.stdout.splitlines():
        m = re.match(r'RELOCATION RECORDS FOR \[(.+)\]:', line)
        if m:
            section = m.group(1)
            continue
        if not section or not section.startswith('.text'):
            continue
        m = re.match(r'^[0-9a-f]+ (R_X86_64_\w+)\s+([^\s+-]+)', line)
        if m and m.group(2) in names and m.group(1) not in CALL_RELOCS:
            hits.add(m.group(2))
    return hits


def data_accesses(obj_dir, objs_names):
    """{name: [(obj, kind)]}: the load/store ('rw') and register-indexed
    ('index') accesses game objects make to function names.

    objs_names: {obj basename: set(names)}.  A call, a tail jump, an lea or
    an immediate (a function pointer) is not a data access."""
    import audit_host_globals as hg

    def one(item):
        b, names = item
        path = os.path.join(ROOT, obj_dir, b)
        cand = text_reloc_candidates(path, names)
        if not cand:
            return b, []
        return b, [(n, k) for n, k, _lo, _hi in hg.object_accesses(path, cand)
                   if k in ('rw', 'index')]
    out = collections.defaultdict(list)
    with ThreadPoolExecutor(8) as ex:
        for b, acc in ex.map(one, sorted(objs_names.items())):
            for n, k in acc:
                if (b, k) not in out[n]:
                    out[n].append((b, k))
    return out


def build_index(build_dir):
    obj_dir = os.path.join(build_dir, 'obj')
    order = make_var(build_dir, 'ALL_OBJS')
    order = [o for o in order if os.path.exists(os.path.join(ROOT, o))]
    srcmap = load_srcmap(os.path.join(ROOT, obj_dir))
    tiers = {}
    for o in order:
        b = os.path.basename(o)
        tiers[b] = tier_of(srcmap.get(b, ''))
    defs = nm_all(order, ['--defined-only'])
    undef = nm_all(order, ['-u'])
    return order, srcmap, tiers, defs, undef


def audit(build_dir, retail=None, include_trampolines=False):
    order, srcmap, tiers, defs, undef = build_index(build_dir)
    obj_dir = os.path.join(build_dir, 'obj')
    # definitions in link order
    definers = collections.defaultdict(list)   # name -> [(obj, strong)]
    obj_funcs = collections.defaultdict(set)
    for o in order:
        b = os.path.basename(o)
        for typ, name in defs.get(b, []):
            if typ in 'TtWw':          # functions (V/v are weak data)
                if typ in 'TW':
                    definers[name].append((b, typ == 'T'))
                obj_funcs[b].add(name)
    winner = {}
    for name, lst in definers.items():
        strong = [o for o, s in lst if s]
        winner[name] = strong[0] if strong else lst[0][0]
    refs = collections.defaultdict(set)        # name -> {obj}
    for b, lst in undef.items():
        for _, name in lst:
            refs[name].add(b)

    reg = load_registry()
    symnames = load_symbol_names()
    bounds = load_boundaries()
    ovsyms, ovaddr = load_overlay_syms()
    psxmap = load_psx_map()
    asmonly = asm_only_names()
    src_to_obj = {v: k for k, v in srcmap.items()}
    overlays = sorted({ov for ov, _ in ovsyms})
    by_window = sorted(((v['address'], v['size'], n) for n, v in reg.items()))
    try:
        audit_addrs = read_json('config/main_function_audit_by_addr.json')['addresses']
    except (OSError, ValueError, KeyError):
        audit_addrs = {}
    audit_starts = sorted(int(k, 16) for k in audit_addrs)

    def address_of(name):
        ov = None
        base = name
        for o in overlays:
            if name.startswith(o + '_'):
                ov, base = o, name[len(o) + 1:]
                break
        m = re.match(r'func_([0-9A-Fa-f]{8})$', name)
        if not ov and m and int(m.group(1), 16) >= OVERLAY_BASE:
            # an unprefixed overlay-window address: it means something in
            # the overlay whose module refers to it
            users = sorted(o[len('overlay_'):-2] for o in refs.get(name, ())
                           if tiers.get(o) == 'overlay')
            if users:
                ov = users[0]
        if ov:
            if (ov, base) in ovsyms:
                return ov, base, ovsyms[(ov, base)][0]
            m = re.match(r'func_([0-9A-Fa-f]{8})$', base)
            return ov, base, int(m.group(1), 16) if m else None
        if name in reg:
            return None, name, reg[name]['address']
        if name in symnames:
            return None, name, symnames[name]
        if name in psxmap:
            return None, name, psxmap[name]
        m = re.match(r'(?:func_|FUN_)([0-9A-Fa-f]{8})$', name)
        if m:
            return None, name, int(m.group(1), 16)
        return None, name, None

    def matched_body(name, ov, base, addr):
        """-> (status, body_names, source, linked).

        status: 'c' (matched C), 'asm_only', 'unmatched', None (no registry).
        """
        # 1. the same name in a game tier
        game = [o for o, _ in definers.get(name, []) if tiers.get(o) in GAME_TIERS]
        if game:
            return 'c', [name], srcmap.get(game[0], ''), True
        if ov:
            # the overlay function whose window STARTS at the address (an
            # absorbed head keeps the address spelling at its call sites)
            host = ovaddr.get((ov, addr)) if addr is not None else None
            if host and host != base:
                p = 'src/overlay/%s/%s.c' % (ov, host)
                mod = '%s_%s' % (ov, host)
                if os.path.exists(os.path.join(ROOT, spell(p))):
                    return 'c', [mod], p, mod in definers
            p = 'src/overlay/%s/%s.c' % (ov, base)
            if os.path.exists(os.path.join(ROOT, spell(p))):
                body = read_text(p)
                code = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
                if '{' in code:
                    return 'c', [name], p, False
                return 'asm_only', [], p, False
            return None, [], '', False
        # 2. registry entry for the name, or the window holding its address
        entries = []
        if name in reg:
            entries.append(name)
        if addr is not None:
            a = bounds.get(addr, addr)
            for start, size, n in by_window:
                if start in (a, addr):
                    if n not in entries:
                        entries.append(n)
                elif start < addr < start + size and not entries:
                    # an address INSIDE another window: an alt-entry or a
                    # phantom, never that window's body
                    return 'interior', [], reg[n]['source'], False
        for n in entries:
            src = reg[n]['source']
            if not src.endswith('.c'):
                return 'asm_only', [], src, False
            o = src_to_obj.get(src)
            if o is None:
                return 'c', [n], src, False
            funcs = sorted(f for f in obj_funcs.get(o, ())
                           if not f.startswith(('.', '__')))
            # the window's own name first
            names = ([n] if n in funcs else []) + [f for f in funcs if f != n]
            return 'c', names, src, True
        if name in asmonly:
            return 'asm_only', [], '', False
        # config/main_function_audit_by_addr.json: the asm_only window that
        # holds the address (Sony LIBGTE members, BIOS thunks)
        if addr is not None and audit_addrs:
            i = bisect.bisect_right(audit_starts, addr) - 1
            if i >= 0:
                start = audit_starts[i]
                v = audit_addrs['0x%08X' % start]
                if v.get('kind') == 'asm_only' and addr - start < 0x2000:
                    return 'asm_only', [], os.path.relpath(v.get('source', ''), str(DECOMP)) \
                        if v.get('source', '').startswith(str(DECOMP)) else v.get('source', ''), False
        return None, [], '', False

    # disassemble every audited object once
    audited = [os.path.basename(o) for o in order
               if tiers.get(os.path.basename(o)) in AUDITED_TIERS
               and (include_trampolines or os.path.basename(o) not in TRAMPOLINE_OBJS)]

    def dis(b):
        r = subprocess.run(['objdump', '-d', '-r', '--no-show-raw-insn',
                            os.path.join(obj_dir, b)], cwd=ROOT,
                           capture_output=True, text=True)
        return b, parse_objdump(r.stdout)
    with ThreadPoolExecutor(8) as ex:
        bodies = dict(ex.map(dis, audited))

    retail = retail if retail is not None else Retail()
    rows = []
    for b in audited:
        for name in sorted(obj_funcs[b]):
            if name.startswith(('.', '__')) or name not in definers:
                continue
            if b not in [o for o, _ in definers[name]]:
                continue
            kind, detail = classify_body(bodies[b].get(name, []))
            ov, base, addr = address_of(name)
            status, body_names, src, linked = matched_body(name, ov, base, addr)
            wins = winner.get(name) == b
            game_all = sorted(o for o in refs.get(name, ()) if tiers.get(o) in GAME_TIERS)
            game_refs = [o for o in game_all
                         if o not in ['overlay_%s.o' % d for d in DEAD_OVERLAYS]]
            dead_ov_refs = [o for o in game_all if o not in game_refs]
            pc_refs = sorted(o for o in refs.get(name, ()) if tiers.get(o) not in GAME_TIERS and o != b)
            rret = []
            if addr is not None and retail.available:
                rret = retail.refs(addr, only=[ov] if ov else None)
            in_table = name in psxmap
            row = dict(name=name, obj=b, src=srcmap.get(b, ''), tier=tiers[b],
                       wins=wins, winner=winner.get(name), body=kind, detail=detail,
                       address=('0x%08X' % addr) if addr is not None else None,
                       overlay=ov, status=status, body_names=body_names,
                       body_src=src, body_linked=linked,
                       game_refs=game_refs, pc_refs=pc_refs, dead_overlay_refs=dead_ov_refs,
                       retail_refs=['%s:%s*%d' % r for r in rret], table=in_table)
            # a game-tier object defining the SAME name is on the link line
            # and loses to this one (a multi-symbol pc-port file; a basename
            # override removes the matched object from the link instead)
            row['shadows_linked'] = any(tiers.get(o) in GAME_TIERS
                                        for o, _ in definers.get(name, ()))
            row['reached_by_game'] = bool(game_refs or rret)
            row['reached'] = bool(game_refs or rret or pc_refs or in_table)
            row['cls'] = classify(row)
            if row['cls'] == 'OVERRIDE':
                head = header_comment(row['src'], name)
                row['doc'] = bool(DOC_WORDS.search(head))
            rows.append(row)

    # DATA-AS-FUNC: game objects that load/store a PC-defined function name
    psx_data = load_psx_data_addrs()
    func_addrs = set(psxmap.values())
    objs_names = collections.defaultdict(set)
    for row in rows:
        for o in row['game_refs'] + row['dead_overlay_refs']:
            objs_names[o].add(row['name'])
    acc = data_accesses(obj_dir, objs_names)
    for row in rows:
        row['data_access'] = ['%s:%s' % a for a in acc.get(row['name'], [])]
        m = ADDR_IN_NAME.search(row['name'])
        row['data_name'] = None
        # ps1_ref_<name> (pc/src/ps1_alias_refs.c) returns the address of
        # the PS1 global <name>: called by game code, never read as data
        if m and not re.match(r'(?:func|FUN)_|ps1_ref_', row['name']):
            a = int(m.group(1), 16)
            # main-binary data only: an overlay-window address means
            # different things in different overlays (D_8013E830 is the
            # ENDING entry ending_main stores as game_mode_step)
            if a < OVERLAY_BASE and a in psx_data and a not in func_addrs:
                row['data_name'] = '0x%08X=%s' % (a, ','.join(sorted(psx_data[a])[:2]))
        row['cls'] = classify(row)
    return rows


def classify(row):
    if not row['wins']:
        return 'SHADOWED'
    if row.get('data_access') or (row.get('data_name') and row['reached']):
        return 'DATA-AS-FUNC'
    if not row['reached']:
        return 'DEAD-OVERLAY' if row.get('dead_overlay_refs') else 'DEAD'
    trivial = row['body'] in TRIVIAL
    if row.get('shadows_linked') and not trivial:
        return 'SHADOWS-MATCHED'
    if row['body'] == 'forward':
        return 'FORWARDER'
    if trivial:
        if row['status'] == 'c' and row['body_names']:
            return 'STUB-HIDES-MATCHED'
        return 'STUB-NO-MATCH'
    if row['status'] == 'c':
        return 'OVERRIDE'
    return 'PC-IMPL'


def gating(rows):
    return [r for r in rows if r['cls'] in GATED and r['reached_by_game']]


GATED = ('STUB-HIDES-MATCHED', 'SHADOWS-MATCHED', 'DATA-AS-FUNC')


def fmt(r):
    extra = []
    if r['status']:
        extra.append('C=%s' % r['status'])
    if r['body_names'] and r['body_names'] != [r['name']]:
        extra.append('body=%s' % ','.join(r['body_names'][:3]))
    if r['body_src']:
        extra.append(os.path.basename(r['body_src']) + ('' if r['body_linked'] else '(unlinked)'))
    refs = []
    if r['game_refs']:
        refs.append('game:%d' % len(r['game_refs']))
    if r['pc_refs']:
        refs.append('pc:%d' % len(r['pc_refs']))
    if r['retail_refs']:
        refs.append('retail:' + ','.join(r['retail_refs'][:3]))
    if r['table']:
        refs.append('psx_map')
    if r.get('dead_overlay_refs'):
        refs.append('dead-ov:' + ','.join(o[8:-2] for o in r['dead_overlay_refs']))
    if r.get('data_access'):
        extra.append('data-access=' + ','.join(r['data_access'][:3]))
    if r.get('data_name'):
        extra.append('data-addr=' + r['data_name'])
    if 'doc' in r:
        extra.append('doc=%s' % ('yes' if r['doc'] else 'NO'))
    return '%-19s %-32s %-10s %-24s %-9s %s  [%s]' % (
        r['cls'], r['name'], r['address'] or '-', r['obj'], r['body'] + (
            '=' + r['detail'] if r['detail'] and r['body'] != 'forward' else ''),
        ' '.join(extra), ' '.join(refs) or 'no refs')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default='pc/build')
    ap.add_argument('--json')
    ap.add_argument('--include-trampolines', action='store_true',
                    help='also audit the generated overlay_symtab.c trampolines')
    ap.add_argument('--all', action='store_true', help='print every row')
    ap.add_argument('--class', dest='cls', nargs='+')
    ap.add_argument('--undocumented', action='store_true',
                    help='with OVERRIDE: only headers that do not document divergence')
    a = ap.parse_args()
    if not os.path.exists(os.path.join(ROOT, a.build_dir, 'obj', '.srcmap')):
        sys.exit('no %s/obj/.srcmap: build first (make -f pc/Makefile all)' % a.build_dir)
    rows = audit(a.build_dir, include_trampolines=a.include_trampolines)
    if a.json:
        with open(a.json, 'w') as fh:
            json.dump(rows, fh, indent=1)
    counts = collections.Counter(r['cls'] for r in rows)
    sel = rows
    if a.cls:
        sel = [r for r in sel if r['cls'] in a.cls]
    if a.undocumented:
        sel = [r for r in sel if r.get('doc') is False]
    if a.all or a.cls:
        for r in sel:
            print(fmt(r))
    print('census (%d definitions in pc/src + src/main/pc-port):' % len(rows))
    for c in ('DATA-AS-FUNC', 'STUB-HIDES-MATCHED', 'STUB-NO-MATCH', 'SHADOWS-MATCHED', 'OVERRIDE', 'FORWARDER',
              'PC-IMPL', 'SHADOWED', 'DEAD-OVERLAY', 'DEAD'):
        print('  %-19s %d' % (c, counts.get(c, 0)))
    undoc = [r for r in rows if r.get('doc') is False]
    print('  OVERRIDE without a divergence note in its header: %d' % len(undoc))
    bad = gating(rows)
    if bad:
        print('\nmatched code hidden from the game (%d):' % len(bad))
        for r in bad:
            print('  ' + fmt(r))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
