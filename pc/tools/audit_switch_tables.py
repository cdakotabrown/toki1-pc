#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""C `switch` case labels against the retail jump tables (overlays + main).

A matched function whose jump table is retail .rodata (`@RodataBase`, or a
table the build links from the retail image) is byte-perfect whatever its C
`case` labels say: the labels only choose which .rodata words the COMPILER
would have written, and the PS1 build never ships those words.  The PC port
compiles the C natively, so there the labels ARE the dispatch.

The audit is exact, not heuristic: each switch-bearing C function is
compiled with the PS1 compiler and NO PostProc (the same compile the
equivalence gates use).  Its .rodata jump table holds the .text offset of
the case body the C's labels select for every slot; retail's table, at the
address retail's own `lui`/`lw` pair names, holds the body retail selects.
The code is byte-identical, so the two tables must be equal word for word,
relative to the function start.  A slot where they differ is a wrong C
label.

The fix is systematic: `pc/tools/switch_labels.json` records, per function
and switch, the label rewrite that turns the C's labels into retail's
(`case K:` -> the retail labels of the body it heads).  The PC build
applies it at the generation boundary (`relabel_switches`, called by
generate_overlay_modules.py on every matched overlay file it reads).
Matched C is never edited.  The gate re-compiles the RELABELED C with the
PS1 compiler and requires the same code and a table equal to retail's
slot for slot -- so the relabel is checked by the same exact compare that
found the defect, and the matched C remains the negative control.  A
disagreement a relabel cannot express (retail's slot enters a body no
label of that switch heads) needs a PC override.

Census 2026-10-31: 1,551 switch-bearing functions (1,347 overlay, 204
main); the matched C disagrees with retail in 1,465 slots of 124 overlay
functions (BUNKASAI 96, OLH 8, BUNKAKEN 4, TEL 3, DATE / ENDING /
RENSYU / RPG_BAT 2, GEKO / OMIMAI / OPTION / TACO / TT 1); main-binary
tables: 89, all agree.  123 are relabeled, RENSYU rensyu_bustup_read is a
PC override.  Not verifiable here: tables the loader relocates at run time
(EVENT / GYOZI, overlays the retail loader never loads).
2026-11-06: TACO and then the 27 non-BUNKASAI functions were fixed in the
matched C itself (labels relabeled, body order kept, still byte-perfect
and PROVEN); their plan entries are pruned and the rensyu_bustup_read
override is retired (its matched C now carries the override's labels).

PC overrides (pc/src/overlay/<OV>/<f>.c, src/main/pc-port/<f>.c) are what
the port compiles where they exist.  An override is compiled with its
matched file's compile directives; if its code words equal the matched
compile's it is audited exactly, otherwise by a first-call comparison of
each `case K:` body with retail's table[K] (reported, `override-heuristic`).

usage:
  audit_switch_tables.py [ID ...] [--matched] [--jobs N] [--write-plan] [--json OUT]
      ID: an overlay name, OV/func, main, or main/<name>; default: everything.
      Needs the IDO 5.2 recomp, the EXNs and disks/SLPS_000.65; results
      are cached in build/switch_audit/ by source + tool hash.
  audit_switch_tables.py --relabel SRC OUT KEY
      write SRC with the recorded relabel for KEY (a starting point for an
      override).

Exit status: 0 when every measured disagreement is resolved by a verified
plan entry (or a correct override) and no plan entry is stale or
unverified; 1 otherwise.  --matched reports the matched C without plans
(the negative control) and always exits 0 unless --strict.
"""
import argparse
import contextlib
import glob
import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP, decomp_key, disc_input, spell  # noqa: E402
for _p in ('tools/overlay', 'tools/build', 'tools/analysis', 'tools/postproc', 'tools'):
    _q = str(DECOMP / _p)
    if _q not in sys.path:
        sys.path.append(_q)

BASE = 0x80132000
PLAN = ROOT / 'pc/tools/switch_labels.json'
CACHE = ROOT / 'build/switch_audit/cache.json'
TOOL_VERSION = 3          # bump to invalidate the measurement cache


# ---------------------------------------------------------------------------
# retail words
# ---------------------------------------------------------------------------

_MAIN = None


def main_word(a):
    global _MAIN
    if _MAIN is None:
        p = disc_input('disks/SLPS_000.65')
        _MAIN = p.read_bytes() if p.exists() else b''
    o = a - 0x80041000 + 0x800        # PS-X EXE t_addr 0x80041000
    if 0x800 <= o <= len(_MAIN) - 4:
        return struct.unpack_from('<I', _MAIN, o)[0]
    return 0


def exn_path(ov):
    for p in (ROOT / ('pc/assets/raw/CDROM/EXEDIR/%s.EXN' % ov),
              disc_input('assets/overlays/%s.EXN' % ov)):
        if p.exists():
            return p
    return None


_EXN = {}


def overlay_word_fn(ov):
    if ov not in _EXN:
        p = exn_path(ov)
        _EXN[ov] = p.read_bytes() if p else None
    exn = _EXN[ov]

    def word(a):
        if exn is not None and BASE <= a < BASE + len(exn) - 3:
            return struct.unpack_from('<I', exn, a - BASE)[0]
        return main_word(a)
    return word


def have_inputs(ov=None):
    if not disc_input('disks/SLPS_000.65').exists():
        return False
    return ov is None or exn_path(ov) is not None


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


# ---------------------------------------------------------------------------
# C source: switches and their case labels
# ---------------------------------------------------------------------------

def mask(text):
    """Comments, strings and char constants blanked (same length)."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group())
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  blank, text, flags=re.S)


CASE_RE = re.compile(r'\bcase\s+([^:;{}]+?)\s*:(?!:)')
DEFAULT_RE = re.compile(r'\bdefault\s*:')


def _int(expr):
    e = expr.strip()
    while e.startswith('(') and e.endswith(')'):
        e = e[1:-1].strip()
    m = re.fullmatch(r'(-?)\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*', e)
    if not m:
        return None
    v = int(m.group(2), 0)
    return -v if m.group(1) else v


def _match(t, i, o, c):
    d = 0
    for k in range(i, len(t)):
        if t[k] == o:
            d += 1
        elif t[k] == c:
            d -= 1
            if d == 0:
                return k
    return None


def switches(text):
    """[{start, body: (lo, hi), labels: [(value or None, span)]}] for each
    switch statement in source order; labels are the switch's own (depth 1,
    not those of a nested switch)."""
    t = mask(text)
    out = []
    for m in re.finditer(r'\bswitch\s*\(', t):
        pe = _match(t, m.end() - 1, '(', ')')
        if pe is None:
            continue
        b = re.match(r'\s*\{', t[pe + 1:])
        if not b:
            continue
        lo = pe + 1 + b.end() - 1
        hi = _match(t, lo, '{', '}')
        if hi is None:
            continue
        out.append(dict(start=m.start(), body=(lo, hi), labels=[], has_default=False))
    for s in out:
        lo, hi = s['body']
        inner = [o['body'] for o in out if lo < o['start'] < hi]
        for m in CASE_RE.finditer(t, lo, hi):
            if any(a <= m.start() <= b for a, b in inner):
                continue
            s['labels'].append((_int(m.group(1)), (m.start(), m.end())))
        for m in DEFAULT_RE.finditer(t, lo, hi):
            if not any(a <= m.start() <= b for a, b in inner):
                s['has_default'] = True
    return out


def apply_relabel(text, entry):
    """`text` with the relabel `entry` (a switch_labels.json value) applied.
    Raises ValueError when the source does not carry the labels the entry
    was made for (a stale plan must never be applied silently).

    relabel keys: a C label value K -> the labels that replace `case K:`
    ([] deletes it); '@default' -> values labelled in front of `default:`;
    '@end' -> values given `case V: break;` at the end of the switch body
    (both keep the table's span when retail sends its lowest/highest value
    to the unlabeled target)."""
    sws = switches(text)
    t = mask(text)
    edits = []
    for sw in entry['switches']:
        i = sw['switch']
        if i >= len(sws):
            raise ValueError('switch %d not in source' % i)
        s = sws[i]
        spans = {}
        for v, span in s['labels']:
            spans.setdefault(v, []).append(span)
        for k, new in sw['relabel'].items():
            if k == '@default':
                lo, hi = s['body']
                m = [d for d in DEFAULT_RE.finditer(t, lo, hi)
                     if not any(o['start'] > lo and o['body'][0] <= d.start() <= o['body'][1]
                                for o in sws if o is not s)]
                if len(m) != 1:
                    raise ValueError('switch %d: no single default label' % i)
                edits.append((m[0].start(), m[0].start(),
                              ''.join('case %d: ' % v for v in new)))
                continue
            if k == '@end':
                hi = s['body'][1]
                edits.append((hi, hi, ''.join('case %d: break; ' % v for v in new)))
                continue
            sp = spans.get(int(k), [])
            if len(sp) != 1:
                raise ValueError('switch %d: case %s appears %d times' % (i, k, len(sp)))
            a, b = sp[0]
            rep = ' '.join('case %d:' % v for v in new)
            edits.append((a, b, rep))
    for a, b, rep in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        text = text[:a] + rep + text[b:]
    return text


def relabel_switches(key, text, plan=None):
    """`text` with the recorded relabel for `key` (OV/func or main/name)
    applied, or unchanged when the plan has none.  The PC generators call
    this; a plan that no longer fits the source raises."""
    entry = (plan if plan is not None else load_plan()).get('functions', {}).get(key)
    if entry is None:
        return text
    try:
        return apply_relabel(text, entry)
    except ValueError as ex:
        raise ValueError('%s: stale pc/tools/switch_labels.json entry (%s); re-run '
                         'audit_switch_tables.py --write-plan' % (key, ex))


# ---------------------------------------------------------------------------
# compiled object: jump tables
# ---------------------------------------------------------------------------

def object_tables(obj, fn):
    """(words, tables) for function `fn` of an object.  tables: one dict per
    `jr rX` (rX != ra) that loads its target from a .rodata table: jr / hi /
    lo word indices relative to the function, the table's entries as byte
    offsets relative to the function, the sltiu bound and the case bias."""
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    with open(obj, 'rb') as f:
        e = ELFFile(f)
        secs = list(e.iter_sections())
        text_i = next(k for k, s in enumerate(secs) if s.name == '.text')
        tdata = secs[text_i].data()
        symtab = e.get_section_by_name('.symtab')
        F = size = None
        funcs = [s for s in symtab.iter_symbols()
                 if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] == text_i]
        for s in funcs:
            if s.name == fn:
                F, size = s['st_value'], s['st_size']
        if F is None and len(funcs) == 1:          # registry name != C name
            F, size = funcs[0]['st_value'], funcs[0]['st_size']
        if F is None:
            return None, None
        words = [struct.unpack_from('<I', tdata, F + i)[0] for i in range(0, size, 4)]
        ro_idx = {k for k, s in enumerate(secs) if s.name in ('.rodata', '.rdata')}
        ro_data = {k: secs[k].data() for k in ro_idx}
        text_rel = []                      # (offset, type, sym)
        ro_entries = {}                    # (sec, off) -> text offset
        for s in secs:
            if not isinstance(s, RelocationSection):
                continue
            tgt = s['sh_info']
            for r in s.iter_relocations():
                sym = symtab.get_symbol(r['r_info_sym'])
                if tgt == text_i:
                    text_rel.append((r['r_offset'], r['r_info_type'], sym))
                elif tgt in ro_idx and r['r_info_type'] == 2 and sym['st_shndx'] == text_i:
                    v = struct.unpack_from('<I', ro_data[tgt], r['r_offset'])[0] + sym['st_value']
                    ro_entries[(tgt, r['r_offset'])] = v
    # rodata address of each LO16 against a rodata symbol (paired HI16 is the
    # closest preceding HI16 against the same symbol, REL convention)
    lo_addr = {}                           # text word index -> (sec, off, hi index)
    for n, (off, ty, sym) in enumerate(text_rel):
        if ty != 6 or sym['st_shndx'] not in ro_idx:
            continue
        hi = next((text_rel[m] for m in range(n - 1, -1, -1)
                   if text_rel[m][1] == 5 and text_rel[m][2] is sym or
                   (text_rel[m][1] == 5 and text_rel[m][2].name == sym.name and
                    text_rel[m][2]['st_shndx'] == sym['st_shndx'])), None)
        if hi is None:
            continue
        hw = struct.unpack_from('<I', tdata, hi[0])[0]
        lw = struct.unpack_from('<I', tdata, off)[0]
        addr = ((hw & 0xFFFF) << 16) + sx(lw & 0xFFFF) + sym['st_value']
        lo_addr[(off - F) // 4] = (sym['st_shndx'], addr, (hi[0] - F) // 4)
    # a call names its callee in the compared words (the raw jal is 0 + a
    # relocation): two compiles agree only if they call the same functions
    import zlib
    for off, ty, sym in text_rel:
        k = (off - F) // 4
        if ty == 4 and 0 <= k < len(words) and sym.name:
            words[k] = (words[k] & 0xFC000000) | (zlib.crc32(sym.name.encode()) & 0x3FFFFFF)
    starts = {(s, a) for s, a, _ in lo_addr.values() if (s, a) in ro_entries}
    tables = []
    for j, w in enumerate(words):
        if (w >> 26) != 0 or (w & 0x3F) != 8 or ((w >> 21) & 31) == 31:
            continue
        cand = [l for l in lo_addr if j - 16 <= l < j and
                (lo_addr[l][0], lo_addr[l][1]) in ro_entries]
        if not cand:
            continue
        l = max(cand)
        sec, a, h = lo_addr[l]
        ent = []
        k = a
        while (sec, k) in ro_entries and (k == a or (sec, k) not in starts):
            ent.append(ro_entries[(sec, k)] - F)
            k += 4
        bound = bias = deflt = None
        for q in range(j - 1, max(-1, j - 24), -1):
            x = words[q]
            if (x >> 26) == 0x0B and bound is None:          # sltiu at, sel, N
                bound, sel, at = x & 0xFFFF, (x >> 21) & 31, (x >> 16) & 31
                for p in range(q + 1, j):                   # beq(z) at, zero, default
                    y = words[p]
                    if (y >> 26) == 4 and ((y >> 21) & 31) == at and ((y >> 16) & 31) == 0:
                        deflt = 4 * (p + 1 + sx(y & 0xFFFF))
                        break
                for p in range(q - 1, max(-1, q - 12), -1):
                    y = words[p]
                    if (y >> 26) == 0x09 and ((y >> 16) & 31) == sel and ((y >> 21) & 31) != sel:
                        bias = -sx(y & 0xFFFF)
                        break
                    if ((y >> 26) == 0 and ((y >> 11) & 31) == sel) or \
                            ((y >> 26) not in (0, 2, 3, 4, 5, 0x2B, 0x29, 0x28) and
                             ((y >> 16) & 31) == sel):
                        break                  # sel defined otherwise: bias 0
                break
        tables.append(dict(jr=j, hi=h, lo=l, entries=ent, bound=bound, bias=bias or 0,
                           deflt=deflt))
    return words, tables


def retail_table_addr(rwords, t, cwords):
    """Retail's table address for compiled table t: the lui/lo pair at the
    compiled indices, or the nearest pair with the same opcode and registers
    (a rescheduled PostProc chain moves them a few words)."""
    def ok_hi(i):
        return 0 <= i < len(rwords) and (rwords[i] >> 26) == 0x0F and \
            ((rwords[i] >> 16) & 31) == ((cwords[t['hi']] >> 16) & 31)

    def ok_lo(i):
        c = cwords[t['lo']]
        return 0 <= i < len(rwords) and (rwords[i] & 0xFFE00000) == (c & 0xFFE00000)
    for dh in sorted(range(-8, 9), key=abs):
        if not ok_hi(t['hi'] + dh):
            continue
        for dl in sorted(range(-8, 9), key=abs):
            if ok_lo(t['lo'] + dl):
                return ((rwords[t['hi'] + dh] & 0xFFFF) << 16) + sx(rwords[t['lo'] + dl] & 0xFFFF)
    return None


# ---------------------------------------------------------------------------
# compile
# ---------------------------------------------------------------------------

DIRECTIVE_RE = re.compile(r'/\*\s*@(CompileFlags|CompileEnv|Compiler)\b[^*]*\*/')


def _overlay_obj(ov, fn, src_text, header_from, td):
    """Raw (PostProc-free) compile of an overlay function; returns the object."""
    import resolve_overlay_relocs as R
    import audit_raw_count as A
    import ido_flags
    src = td / ('%s.c' % fn)
    body = A.strip_postproc(src_text)
    if header_from is not None:
        body = '\n'.join(m.group() for m in DIRECTIVE_RE.finditer(header_from)) + '\n' + body
    src.write_text(body)
    from parse_postproc import parse_compiler
    extra = []
    if not parse_compiler(src).startswith('gcc'):
        extra = ido_flags.stripped_pic_extra(body, R.parse_compile_flags(src))
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return R.compile_object(ov, fn, src, 0, td, extra_cflags=extra)


def _main_obj(entry, src_text, header_from, td):
    import audit_plain_vs_shipped as P
    from parse_postproc import parse_compile_flags, parse_compiler, parse_compile_env
    src = td / ('%s.c' % entry['name'])
    body = src_text
    if header_from is not None:
        body = '\n'.join(m.group() for m in DIRECTIVE_RE.finditer(header_from)) + '\n' + body
    src.write_text(body)
    parsed = (parse_compile_flags(str(src)), parse_compiler(str(src)), parse_compile_env(str(src)))
    keep = td / 'k.o'
    e = dict(entry, source=str(src))
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            P._main_compile(e, [], parsed, keep=str(keep))
    except BaseException:                       # noqa: BLE001
        return None
    return keep if keep.exists() else None


def _compile(kind, ident, src_text, header_from=None):
    td = Path(tempfile.mkdtemp(prefix='swaud_'))
    try:
        if kind == 'ov':
            ov, fn = ident
            obj = _overlay_obj(ov, fn, src_text, header_from, td)
            name = fn
        else:
            obj = _main_obj(ident, src_text, header_from, td)
            name = ident['name']
        if obj is None or not Path(obj).exists():
            return None, None
        return object_tables(obj, name)
    except Exception:                           # noqa: BLE001
        return None, None
    finally:
        shutil.rmtree(td, ignore_errors=True)


# ---------------------------------------------------------------------------
# measure one function
# ---------------------------------------------------------------------------

def _window_overlay(ov, fn, text):
    m = re.search(r'@SymbolStart:\s*(0x[0-9A-Fa-f]+)', text)
    start = int(m.group(1), 16) if m else None
    m = re.search(r'@SymbolSize:\s*(\d+)', text)
    size = int(m.group(1)) if m else None
    for line in (DECOMP / 'config/overlay' / ('%s.sym' % ov)).read_text().splitlines():
        p = line.strip().split(':')
        if len(p) >= 3 and p[2].strip() == fn and p[0].startswith('0x'):
            start = start or int(p[0], 16)
            size = size or int(p[1], 0)
    return start, size


def compare(cwords, ctables, word, start, text):
    """Compare a compile's tables with retail's.  Returns a list of switch
    results: {jr, bias, n, c, r, default, slots: [diff slot indices],
    switch: source switch index or None, status}."""
    rwords = [word(start + 4 * i) for i in range(len(cwords))]
    sws = switches(text)
    used = set()
    out = []
    for t in ctables:
        n = len(t['entries'])
        ra = retail_table_addr(rwords, t, cwords)
        res = dict(jr=t['jr'], bias=t['bias'], n=n, c=t['entries'], switch=None)
        if ra is None:
            res.update(status='no-retail-table')
            out.append(res)
            continue
        r = [word(ra + 4 * i) - start for i in range(n)]
        res.update(r=r, retail_table='0x%08X' % ra)
        # the slot an unlabeled value takes: the target of the bound check
        # (a beqz/beq on the sltiu result) -- else any slot with no C label
        si = None
        for k, s in enumerate(sws):
            vals = [v for v, _ in s['labels']]
            if k not in used and vals and None not in vals and \
                    max(vals) - min(vals) + 1 == n:
                si = k
                break
        res['switch'] = si
        res['deflt'] = t['deflt']
        if si is not None:
            used.add(si)
            # IDO's table spans the switch's lowest..highest label
            res['bias'] = min(v for v, _ in sws[si]['labels'])
        if not all(0 <= x < 4 * len(cwords) for x in r):
            res.update(status='retail-outside-window')
        else:
            res['slots'] = [i for i in range(n) if r[i] != t['entries'][i]]
            res['status'] = 'disagree' if res['slots'] else 'agree'
        out.append(res)
    return out


def plan_for(text, results):
    """The relabel that gives each disagreeing switch retail's labels:
    {'switches': [{'switch': i, 'relabel': {K: [labels]}}]} or (None, why)."""
    sws = switches(text)
    plan = []
    for res in results:
        if res.get('status') != 'disagree':
            continue
        si = res.get('switch')
        if si is None:
            return None, 'jump table %d not matched to a C switch' % res['jr']
        sw = sws[si]
        if any(v is None for v, _ in sw['labels']):
            return None, 'switch %d has non-literal case labels' % si
        bias, c, r = res['bias'], res['c'], res['r']
        n = len(c)
        cl = {v for v, _ in sw['labels']}
        # where a value with no label goes: the bound check's target (the
        # default label, or the end of the switch); a value with no C label
        # inside the range goes there too
        d = res.get('deflt')
        unl = {c[i] for i in range(n) if bias + i not in cl}
        if d is None and len(unl) == 1:
            d = next(iter(unl))
        if d is None or any(x != d for x in unl):
            return None, 'switch %d: cannot tell where unlabeled values go' % si
        heads = {}                             # body offset -> C labels heading it
        for v in cl:
            heads.setdefault(c[v - bias], set()).add(v)
        want = {}
        for i in range(n):
            want.setdefault(r[i], set()).add(bias + i)
        relabel = {}
        for x in set(heads) | set(want):
            have = heads.get(x, set())
            need = want.get(x, set())
            if x == d:
                need = need & have      # unlabeled values reach d anyway
            if have == need:
                continue
            if not have:
                return None, 'switch %d: retail body at +0x%X has no C label' % (si, x)
            # the source-last label of the group carries the new labels
            order = sorted(have, key=lambda v: next(sp for vv, sp in sw['labels'] if vv == v))
            for v in order[:-1]:
                relabel[str(v)] = []
            relabel[str(order[-1])] = sorted(need)
        # the table must keep its span: a lowest/highest value retail sends to
        # the unlabeled target keeps a label there
        final = set()
        for v in cl:
            final.update(relabel.get(str(v), [v]))
        keep = sorted({bias, bias + n - 1} - final)
        if keep:
            if any(r[k - bias] != d for k in keep):
                return None, 'switch %d: span value has no retail body' % si
            relabel['@default' if sw['has_default'] else '@end'] = keep
        plan.append(dict(switch=si, relabel=dict(sorted(
            relabel.items(), key=lambda kv: (kv[0].startswith('@'), int(kv[0]) if kv[0][0] != '@' else 0)))))
    return dict(switches=plan), None


def sha(text):
    return hashlib.sha1(text.encode('utf-8', 'replace')).hexdigest()


def measure(task):
    """Worker: measure one function.  task = (key, kind, ident, src_path,
    matched_path, plan_entry)."""
    key, kind, ident, src_path, matched_path, plan_entry = task
    text = (ROOT / spell(src_path)).read_text(errors='replace')
    is_override = src_path != matched_path
    mtext = (ROOT / spell(matched_path)).read_text(errors='replace')
    out = dict(key=key, source=src_path)
    if kind == 'ov':
        ov, fn = ident
        start, size = _window_overlay(ov, fn, mtext)
        word = overlay_word_fn(ov)
    else:
        start, size = int(ident['address'], 16), ident['size']
        word = main_word
    if not start:
        out['status'] = 'no-window'
        return out
    cw, ct = _compile(kind, ident, mtext)
    if cw is None:
        out['status'] = 'compile-failed'
        return out
    if not ct:
        out['status'] = 'no-table'
        return out
    res = compare(cw, ct, word, start, mtext)
    out['matched'] = res
    out['status'] = 'measured'
    if any(r['status'] == 'disagree' for r in res):
        plan, why = plan_for(mtext, res)
        out['plan'] = plan
        out['plan_error'] = why
    if plan_entry is not None and not is_override:
        try:
            rt = apply_relabel(mtext, plan_entry)
        except ValueError as ex:
            out['relabeled_error'] = str(ex)
        else:
            w2, t2 = _compile(kind, ident, rt)
            if w2 is None:
                out['relabeled_error'] = 'relabeled C does not compile'
            elif w2 != cw:
                out['relabeled_error'] = 'relabeled C compiles to different code'
            else:
                out['relabeled'] = compare(w2, t2, word, start, rt)
    if is_override:
        w2, t2 = _compile(kind, ident, text, header_from=mtext)
        if w2 is not None and _same_code(w2, cw):
            out['override'] = compare(w2, t2, word, start, text)
        else:
            out['override_heuristic'] = heuristic_rows(text, res, word, start,
                                                     ident[0] if kind == 'ov' else None)
    return out


def _same_code(a, b):
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x == y:
            continue
        op = x >> 26
        if op != y >> 26:
            return False
        if op == 2:                            # j: target is a relocation
            continue
        if op not in (0x0F, 0x09, 0x23, 0x21, 0x25, 0x20, 0x24, 0x2B, 0x29, 0x28, 0x0D) or \
                (x & 0xFFFF0000) != (y & 0xFFFF0000):
            return False
    return True


def first_call(word, a, limit=10):
    """(callee address, a0 literal or None) of the first jal from `a`
    before any branch/return, or None."""
    a0 = None
    for k in range(limit):
        w = word(a + 4 * k)
        op = w >> 26
        if op in (0x09, 0x0D) and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 4:
            a0 = (sx(w & 0xFFFF) if op == 0x09 else w & 0xFFFF)
        if op == 3:
            d = word(a + 4 * k + 4)                  # the delay slot
            if (d >> 26) in (0x09, 0x0D) and ((d >> 21) & 31) == 0 and ((d >> 16) & 31) == 4:
                a0 = sx(d & 0xFFFF) if (d >> 26) == 0x09 else d & 0xFFFF
            return 0x80000000 | ((w & 0x3FFFFFF) << 2), a0
        if op == 2 or (op == 0 and (w & 0x3F) == 8) or (op in (4, 5) and k > 0):
            return None
    return None


_NAMES = {}


def name_addr(name, ov=None):
    m = re.fullmatch(r'func_([0-9A-Fa-f]{8})', name)
    if m:
        return int(m.group(1), 16)
    if 'main' not in _NAMES:
        syms = json.loads((DECOMP / 'config/symbol_names.json').read_text())['symbols']
        _NAMES['main'] = {v['name']: int(k, 16) for k, v in syms.items()
                          if v.get('name') and ':' not in k}
    if ov and ov not in _NAMES:
        d = {}
        for line in (DECOMP / 'config/overlay' / ('%s.sym' % ov)).read_text().splitlines():
            q = line.strip().split(':')
            if len(q) >= 3 and q[0].startswith('0x'):
                d.setdefault(q[2].strip(), int(q[0], 16))
        _NAMES[ov] = d
    return (_NAMES.get(ov) or {}).get(name) or _NAMES['main'].get(name)


def heuristic_rows(text, res, word, start, ov=None):
    """For an override that does not compile to the matched code: for every
    slot of each retail table, the first call (and a literal first argument)
    under the override's `case K:` against the first jal (and `li a0`) at
    retail's table[K]; a slot retail sends to a body other than the
    unlabeled target needs a label."""
    rows = []
    t = mask(text)
    tabs = [r for r in res if r.get('r') is not None]
    used = set()
    for sw in switches(text):
        labs = sw['labels']
        vals = [v for v, _ in labs if v is not None]
        if not vals:
            continue
        tab = next((r for r in tabs if id(r) not in used and
                    r['bias'] <= min(vals) and max(vals) < r['bias'] + r['n']), None)
        if tab is None:
            continue
        used.add(id(tab))
        body = {}
        for idx, (v, _) in enumerate(labs):
            if v is None:
                continue
            j = idx
            while j + 1 < len(labs) and t[labs[j][1][1]:labs[j + 1][1][0]].strip() == '':
                j += 1
            end = labs[j + 1][1][0] if j + 1 < len(labs) else sw['body'][1]
            body[v] = t[labs[j][1][1]:end]
        for i in range(tab['n']):
            k = tab['bias'] + i
            rc = first_call(word, start + tab['r'][i])
            if k not in body:
                if tab['r'][i] != tab.get('deflt'):
                    rows.append(dict(case=k, c_callee=None, c_addr=None,
                                     retail_addr=rc[0] if rc else start + tab['r'][i]))
                continue
            m = next((c for c in re.finditer(r'\b([A-Za-z_]\w*)\s*\(([^(),]*)', body[k])
                      if c.group(1) not in ('if', 'while', 'switch', 'for', 'return', 'sizeof')),
                     None)
            if m is not None and re.search(r'[{}]|\b(if|switch|while|for|do|goto)\b',
                                           body[k][:m.start()]):
                continue                    # not a straight-line call: undecidable here
            if m is None or rc is None:
                if (m is None) != (rc is None):
                    rows.append(dict(case=k, c_callee=m and m.group(1), c_addr=None,
                                     retail_addr=rc and rc[0]))
                continue
            ca = name_addr(m.group(1), ov)
            lit = _int(m.group(2)) if m.group(2).strip() else None
            if ca != rc[0] or (lit is not None and rc[1] is not None and lit != rc[1]):
                rows.append(dict(case=k, c_callee='%s(%s)' % (m.group(1), m.group(2).strip()),
                                 c_addr=ca, retail_addr=rc[0], retail_a0=rc[1]))
    return rows


# ---------------------------------------------------------------------------
# enumeration
# ---------------------------------------------------------------------------

def _has_switch(path):
    try:
        return re.search(r'\bswitch\s*\(', mask(path.read_text(errors='replace'))) is not None
    except OSError:
        return False


def main_sources():
    """{name: (entry, matched path, PC path)} for registered main functions
    with matched C that contains a switch."""
    reg = json.loads((DECOMP / 'config/level3_functions.json').read_text())['functions']
    out = {}
    for e in reg:
        s = e.get('source', '')
        if not s.endswith('.c') or not _has_switch(ROOT / spell(s)):
            continue
        pcp = ROOT / 'src/main/pc-port' / Path(s).name
        out[e['name']] = (e, s, os.path.relpath(pcp, ROOT) if pcp.exists() else s)
    return out


def overlay_sources(ovs):
    out = []
    for ov in ovs:
        if exn_path(ov) is None:
            continue
        for f in sorted(glob.glob(str(DECOMP / 'src/overlay' / ov / '*.c'))):
            p = Path(f)
            if not _has_switch(p):
                continue
            fn = p.stem
            o = ROOT / 'pc/src/overlay' / ov / p.name
            out.append((ov, fn, decomp_key(p),
                        os.path.relpath(o, ROOT) if o.exists() else decomp_key(p)))
    return out


_PLAN = {}


def load_plan():
    if PLAN not in _PLAN:
        _PLAN[PLAN] = json.loads(PLAN.read_text()) if PLAN.exists() else {}
    return _PLAN[PLAN]


def tasks_for(ids, matched_only=False):
    plan = load_plan().get('functions', {})
    all_ovs = sorted(p.stem for p in (DECOMP / 'config/overlay').glob('*.sym'))
    want_ov, want_fn, want_main, everything = set(), set(), set(), not ids
    for i in ids:
        if i.startswith('main/'):
            want_main.add(i[5:])
        elif i == 'main':
            want_main.add('*')
        elif '/' in i:
            want_fn.add(i)
        else:
            want_ov.add(i)
    tasks = []
    ovs = all_ovs if everything else sorted(want_ov | {x.split('/')[0] for x in want_fn})
    for ov, fn, mp, pp in overlay_sources(ovs):
        key = '%s/%s' % (ov, fn)
        if not everything and ov not in want_ov and key not in want_fn:
            continue
        src = mp if matched_only else pp
        tasks.append((key, 'ov', (ov, fn), src, mp, None if matched_only else plan.get(key)))
    if everything or want_main:
        for name, (e, mp, pp) in sorted(main_sources().items()):
            if not everything and '*' not in want_main and name not in want_main:
                continue
            key = 'main/%s' % name
            src = mp if matched_only else pp
            tasks.append((key, 'main', e, src, mp, None if matched_only else plan.get(key)))
    return tasks


def _task_hash(t):
    key, kind, ident, src, mp, pe = t
    h = hashlib.sha1()
    h.update(str(TOOL_VERSION).encode())
    h.update(Path(__file__).read_bytes())          # the measurement code itself
    for p in sorted({src, mp}):
        h.update((ROOT / spell(p)).read_bytes())
    h.update(json.dumps(pe, sort_keys=True).encode())
    h.update(json.dumps(ident if kind == 'ov' else [ident['address'], ident['size']]).encode())
    return h.hexdigest()


def run(tasks, jobs):
    cache = {}
    if CACHE.exists():
        try:
            cache = json.load(open(CACHE))
        except ValueError:
            cache = {}
    todo, out = [], {}
    for t in tasks:
        h = _task_hash(t)
        if cache.get(t[0], {}).get('hash') == h:
            out[t[0]] = cache[t[0]]['result']
        else:
            todo.append((t, h))
    if todo:
        if jobs > 1 and len(todo) > 1:
            import multiprocessing as mp
            with mp.get_context('fork').Pool(jobs) as pool:
                res = pool.map(measure, [t for t, _ in todo], chunksize=2)
        else:
            res = [measure(t) for t, _ in todo]
        for (t, h), r in zip(todo, res):
            out[t[0]] = r
            if r.get('status') not in ('compile-failed',):
                cache[t[0]] = dict(hash=h, result=r)
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        json.dump(cache, open(CACHE, 'w'))
    return [out[t[0]] for t in tasks]


# ---------------------------------------------------------------------------
# verdicts
# ---------------------------------------------------------------------------

def verdict(r, plan_entry, matched_only):
    """(state, detail).  state: ok | fixed | override-ok | open | stale."""
    st = r.get('status')
    if st != 'measured':
        if plan_entry is not None:
            return 'stale', 'plan entry but function %s' % st
        return 'ok', st
    bad = [s for s in r['matched'] if s['status'] == 'disagree']
    odd = [s for s in r['matched'] if s['status'] not in ('agree', 'disagree')]
    if matched_only:
        return ('open' if bad else 'ok'), ''
    if 'override' in r:
        ob = [s for s in r['override'] if s['status'] == 'disagree']
        if plan_entry is not None:
            return 'stale', 'plan entry for a function the PC overrides'
        return ('open', 'override disagrees') if ob else ('override-ok', '')
    if 'override_heuristic' in r:
        if plan_entry is not None:
            return 'stale', 'plan entry for a function the PC overrides'
        if r['override_heuristic']:
            return 'open', 'override (heuristic) disagrees'
        return 'override-ok', 'heuristic' + (' (matched C disagrees)' if bad else '')
    if not bad:
        if plan_entry is not None:
            return 'stale', 'plan entry but the matched C agrees'
        return 'ok', ('; '.join(s['status'] for s in odd) if odd else '')
    if plan_entry is None:
        return 'open', r.get('plan_error') or 'no plan entry (--write-plan)'
    if 'relabeled_error' in r:
        return 'stale', r['relabeled_error']
    rb = [s for s in r.get('relabeled', []) if s['status'] != 'agree']
    if rb:
        return 'stale', 'relabeled C still disagrees'
    return 'fixed', ''


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('ids', nargs='*')
    ap.add_argument('--matched', action='store_true',
                    help='audit the matched C with no plan (negative control)')
    ap.add_argument('--strict', action='store_true', help='with --matched: exit 1 on a disagreement')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--write-plan', action='store_true',
                    help='record the computed relabel for every disagreeing function')
    ap.add_argument('--json', help='write the per-function results here')
    ap.add_argument('--relabel', nargs=3, metavar=('SRC', 'OUT', 'KEY'))
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args(argv)

    if a.relabel:
        src, out, key = a.relabel
        entry = load_plan().get('functions', {}).get(key)
        text = Path(src).read_text(errors='replace')
        if entry is not None:
            text = apply_relabel(text, entry)
            text = '/* PC build: case labels relabeled to the retail jump table\n' \
                   ' * (pc/tools/switch_labels.json, %s). */\n#line 1 "%s"\n' % (key, src) + text
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        Path(out).write_text(text)
        return 0

    if not have_inputs():
        print('audit_switch_tables: needs disks/SLPS_000.65 and the overlay EXNs', file=sys.stderr)
        return 1
    tasks = tasks_for(a.ids, a.matched)
    results = run(tasks, a.jobs)
    plan = load_plan()
    pf = plan.get('functions', {})

    if a.write_plan:
        changed = 0
        for t, r in zip(tasks, results):
            if t[0] in pf and r.get('status') == 'measured' and \
                    not any(s['status'] == 'disagree' for s in r['matched']):
                del pf[t[0]]
                changed += 1
            p = r.get('plan')
            if p and p['switches'] and t[3] == t[4]:
                ent = dict(source=t[4], source_sha1=sha((ROOT / spell(t[4])).read_text(errors='replace')),
                           switches=p['switches'])
                if pf.get(t[0]) != ent:
                    pf[t[0]] = ent
                    changed += 1
        plan = dict(description=PLAN_DESC, functions=dict(sorted(pf.items())))
        PLAN.write_text(json.dumps(plan, indent=1, sort_keys=False) + '\n')
        print('switch_labels.json: %d entries (%d changed); re-run to verify' % (len(pf), changed))
        return 0

    states = {}
    rows = []
    for t, r in zip(tasks, results):
        st, why = verdict(r, None if a.matched else pf.get(t[0]), a.matched)
        states.setdefault(st, []).append(t[0])
        if st in ('open', 'stale') or a.verbose and st != 'ok':
            rows.append((t[0], st, why, r))
    # plan entries for functions this run did not see at all
    seen = {t[0] for t in tasks}
    if not a.ids and not a.matched:
        for k in pf:
            if k not in seen:
                states.setdefault('stale', []).append(k)
                rows.append((k, 'stale', 'plan entry for a function with no switch/C', {}))
    for key, st, why, r in rows:
        print('%-6s %-44s %s' % (st.upper(), key, why))
        for s in r.get('matched', []):
            if s['status'] == 'disagree':
                print('        table %s (jr +0x%X): slots %s' % (
                    s.get('retail_table'), 4 * s['jr'],
                    ', '.join('%d: C +0x%X retail +0x%X' % (s['bias'] + i, s['c'][i], s['r'][i])
                              for i in s['slots'][:12]) + (' ...' if len(s['slots']) > 12 else '')))
        for h in r.get('override_heuristic', []) or []:
            print('        case %d: C %s %s, retail %s a0=%s' % (
                h['case'], h['c_callee'], '0x%08X' % h['c_addr'] if h.get('c_addr') else '-',
                '0x%08X' % h['retail_addr'] if h.get('retail_addr') else '-', h.get('retail_a0')))
    nslots = sum(len(s['slots']) for t, r in zip(tasks, results)
                 for s in r.get('matched', []) if s['status'] == 'disagree')
    nfun = sum(1 for r in results if any(s['status'] == 'disagree' for s in r.get('matched', [])))
    print('%d switch functions measured; matched C disagrees with retail in %d slots of %d '
          'functions' % (len(results), nslots, nfun))
    print('  ' + ', '.join('%s %d' % (k, len(v)) for k, v in sorted(states.items())))
    if a.json:
        json.dump(dict(zip([t[0] for t in tasks], results)), open(a.json, 'w'), indent=1)
    if a.matched:
        return 1 if a.strict and states.get('open') else 0
    return 1 if states.get('open') or states.get('stale') else 0


PLAN_DESC = ('Per function: the case-label rewrite that gives the matched C the '
             'retail jump table (written by audit_switch_tables.py --write-plan, '
             'verified by re-compiling the relabeled C with the PS1 compiler). '
             'Applied only to the PC build; the matched C is never edited.')


if __name__ == '__main__':
    sys.exit(main())
