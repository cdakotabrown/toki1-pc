#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""translation_pack.py -- the PC English translation pack (TKTP).

The PS1 English patch build
(tools/patch/build_patched.py config/patch/english.json) writes a patched
copy of every disc file it changes.  This tool turns "retail file -> patched
file" into a RECIPE that holds only bytes this project authored:

  SAME len         keep the player's bytes at this position
  COPY src len     take the player's bytes from another offset of the SAME
                   file (a relocated or re-packed retail string)
  MASK len m v     write our nibbles: out = (base & ~m) | v, v & ~m == 0,
                   so an unchanged retail nibble is never stored
  FILL len b       a run of one byte value

The recipe is applied on the player's machine to the player's own extracted
Japanese files (pc/hal/translation.c in the game, `apply` here), each base
and result checked by SHA-1.  Format: pc/hal/translation.h.

Commands:
  build  [--patched DIR] [--retail DIR] [--out FILE]
         DIR defaults: build/patch/iso/staging of the English patch checkout
         (toki1-english, pc/tools/pc_paths.py: the patched disc tree its
         patch build stages) and pc/assets/raw (the player's extraction).
  apply  PACK RETAIL_RAW OUT_RAW         reference applier (tests)
  audit  PACK [--retail DIR] [--json]    the content audit (see audit())
  dump   PACK                            per-file op summary

The pack is a BUILD OUTPUT (pc/build/, never committed): it is generated on
a machine that has the disc and compiled into the executable.
"""
import argparse
import hashlib
import json
import os
import struct
import sys


def _read(path):
    with open(path, 'rb') as fh:
        return fh.read()


def _read_text(path):
    with open(path) as fh:
        return fh.read()

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# The English patch (its build output build/patch, its art src/patch/art)
# is toki1-english's: None when that checkout is absent (pc_paths.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import ENGLISH, english_path  # noqa: E402
EN_ROOT = str(ENGLISH) if ENGLISH is not None else None
SAME, COPY, MASK, FILL = 1, 2, 3, 4
MIN_COPY = 6          # a COPY (9 bytes) beats MASK (2 bytes a byte) from 5 on
MIN_SAME = 3          # inside a changed region, a short equal gap stays MASK (mask 0)
MIN_FILL = 8           # a FILL op (6 bytes) beats MASK (2 a byte) from 4 on; 8 keeps zero pads out of literal runs
GRAM = 4
# CD-DA / XA / STR media are never patched (ENGLISH-PATCH-ARCHITECTURE.md);
# the staged CD-DA track is the mkpsxiso input, not the player's .WAV
# extraction, so it differs in container bytes only.
MEDIA_EXT = ('.WAV', '.STR', '.XA')


# ---------------------------------------------------------------- encoder
def _index(base, gram, skip_trivial=False):
    idx = {}
    for i in range(len(base) - gram + 1):
        g = base[i:i + gram]
        if skip_trivial and len(set(g)) <= 2:
            continue
        lst = idx.get(g)
        if lst is None:
            idx[g] = [i]
        elif len(lst) < 48:
            lst.append(i)
    return idx


def encode(base, out):
    """Ops turning `base` into `out` (same length or not).  Greedy: equal
    bytes -> SAME; otherwise the longest COPY from anywhere in base;
    otherwise our nibbles (MASK)."""
    ops = []
    n, bn = len(out), len(base)
    idx = None
    lit_m, lit_v = bytearray(), bytearray()

    def flush():
        if lit_m:
            ops.append((MASK, bytes(lit_m), bytes(lit_v)))
            lit_m.clear()
            lit_v.clear()

    i = 0
    while i < n:
        # equal run
        j = i
        while j < n and j < bn and out[j] == base[j]:
            j += 1
        if j - i >= MIN_SAME or (j > i and not lit_m) or j == n:
            if j > i:
                flush()
                ops.append((SAME, j - i))
                i = j
                continue
        elif j > i:
            for k in range(i, j):          # short equal gap inside a change
                lit_m.append(0)
                lit_v.append(0)
            i = j
            continue
        # a run of one value
        k = i
        while k < n and out[k] == out[i]:
            k += 1
        if k - i >= MIN_FILL:
            flush()
            ops.append((FILL, k - i, out[i]))
            i = k
            continue
        # copy from elsewhere in base
        best_len, best_src = 0, 0
        if i + GRAM <= n:
            if idx is None:
                # 4-grams find short moves; non-trivial 8-grams keep a run
                # that starts with padding (zeros) from drowning in the cap
                idx = (_index(base, GRAM), _index(base, 8, True))
            cands = list(idx[0].get(bytes(out[i:i + GRAM]), ()))
            if i + 8 <= n:
                cands += idx[1].get(bytes(out[i:i + 8]), ())
            for s in cands:
                L = GRAM
                while i + L < n and s + L < bn and out[i + L] == base[s + L]:
                    L += 1
                if L > best_len:
                    best_len, best_src = L, s
        if best_len >= MIN_COPY:
            flush()
            if best_src == i:
                ops.append((SAME, best_len))
            else:
                ops.append((COPY, best_src, best_len))
            i += best_len
            continue
        # our nibbles
        b = base[i] if i < bn else 0
        m = (0xF0 if (b ^ out[i]) & 0xF0 else 0) | (0x0F if (b ^ out[i]) & 0x0F else 0)
        lit_m.append(m)
        lit_v.append(out[i] & m)
        i += 1
    flush()
    return ops


def apply_ops(ops, base, out_size):
    out = bytearray(out_size)
    pos = 0
    for op in ops:
        t = op[0]
        if t == SAME:
            L = op[1]
            out[pos:pos + L] = base[pos:pos + L]
        elif t == COPY:
            s, L = op[1], op[2]
            out[pos:pos + L] = base[s:s + L]
        elif t == MASK:
            m, v = op[1], op[2]
            L = len(m)
            for k in range(L):
                b = base[pos + k] if pos + k < len(base) else 0
                out[pos + k] = (b & ~m[k] & 0xFF) | (v[k] & m[k])
        elif t == FILL:
            L = op[1]
            out[pos:pos + L] = bytes([op[2]]) * L
        else:
            raise ValueError('bad op %r' % (t,))
        pos += L
    if pos != out_size:
        raise ValueError('ops cover %d of %d bytes' % (pos, out_size))
    return bytes(out)


# ---------------------------------------------------------------- container
def serialize(entries):
    """entries: [(path, base_bytes_or_None, out_bytes, ops)]"""
    parts = [b'TKTP', struct.pack('<II', 1, len(entries))]
    for path, base, out, ops in entries:
        p = path.encode()
        parts.append(struct.pack('<H', len(p)) + p)
        if base is None:
            parts.append(struct.pack('<I', 0) + b'\0' * 20)
        else:
            parts.append(struct.pack('<I', len(base)) + hashlib.sha1(base).digest())
        parts.append(struct.pack('<I', len(out)) + hashlib.sha1(out).digest())
        parts.append(struct.pack('<I', len(ops)))
        for op in ops:
            t = op[0]
            if t == SAME:
                parts.append(struct.pack('<BI', SAME, op[1]))
            elif t == COPY:
                parts.append(struct.pack('<BII', COPY, op[1], op[2]))
            elif t == MASK:
                parts.append(struct.pack('<BI', MASK, len(op[1])) + op[1] + op[2])
            elif t == FILL:
                parts.append(struct.pack('<BIB', FILL, op[1], op[2]))
    return b''.join(parts)


def parse(blob):
    if blob[:4] != b'TKTP':
        raise ValueError('not a TKTP pack')
    ver, n = struct.unpack_from('<II', blob, 4)
    if ver != 1:
        raise ValueError('pack version %d' % ver)
    off = 12
    out = []
    for _ in range(n):
        (pl,) = struct.unpack_from('<H', blob, off); off += 2
        path = blob[off:off + pl].decode(); off += pl
        (bsize,) = struct.unpack_from('<I', blob, off); off += 4
        bsha = blob[off:off + 20]; off += 20
        (osize,) = struct.unpack_from('<I', blob, off); off += 4
        osha = blob[off:off + 20]; off += 20
        (nops,) = struct.unpack_from('<I', blob, off); off += 4
        ops = []
        for _ in range(nops):
            t = blob[off]; off += 1
            if t == SAME:
                (L,) = struct.unpack_from('<I', blob, off); off += 4
                ops.append((SAME, L))
            elif t == COPY:
                s, L = struct.unpack_from('<II', blob, off); off += 8
                ops.append((COPY, s, L))
            elif t == MASK:
                (L,) = struct.unpack_from('<I', blob, off); off += 4
                ops.append((MASK, blob[off:off + L], blob[off + L:off + 2 * L])); off += 2 * L
            elif t == FILL:
                L, v = struct.unpack_from('<IB', blob, off); off += 5
                ops.append((FILL, L, v))
            else:
                raise ValueError('bad op %d in %s' % (t, path))
        out.append({'path': path, 'base_size': bsize, 'base_sha1': bsha.hex(),
                    'out_size': osize, 'out_sha1': osha.hex(), 'ops': ops})
    if off != len(blob):
        raise ValueError('trailing bytes in pack')
    return out


# ---------------------------------------------------------------- build
def changed_files(patched_root, retail_root):
    """(relpath, retail bytes, patched bytes) for every file of the patched
    tree whose content differs from the player's extraction."""
    res = []
    for d, _, fs in os.walk(patched_root, followlinks=True):
        for f in sorted(fs):
            p = os.path.join(d, f)
            rel = os.path.relpath(p, patched_root).replace(os.sep, '/')
            # layout.xml and license_data.dat are mkpsxiso's disc description
            # and system-area sectors, not game files: pc/tools/extract_assets.py
            # never writes them to pc/assets/raw and the port never reads them.
            if rel in ('layout.xml', 'license_data.dat') or rel.upper().endswith(MEDIA_EXT):
                continue
            r = os.path.join(retail_root, rel)
            if not os.path.isfile(r):
                raise SystemExit('translation_pack: %s has no retail counterpart %s' % (rel, r))
            if os.path.islink(p) and os.path.realpath(p) == os.path.realpath(r):
                continue
            a = _read(p)
            b = _read(r)
            if a != b:
                res.append((rel, b, a))
    return sorted(res)


def build(patched_root, retail_root, out_path, quiet=False):
    entries = []
    for rel, base, out in changed_files(patched_root, retail_root):
        ops = encode(base, out)
        if apply_ops(ops, base, len(out)) != out:
            raise SystemExit('translation_pack: round trip failed for ' + rel)
        entries.append((rel, base, out, ops))
        if not quiet:
            st = op_stats(ops)
            print('  %-32s %8d B  same %7d copy %6d ours %6d fill %5d' % (
                rel, len(out), st['same'], st['copy'], st['mask_nibble_bytes'], st['fill']))
    # the patch build's report sits beside the staging it wrote
    # (<English checkout>/build/patch/{english.report.json,iso/staging})
    report = os.path.normpath(os.path.join(patched_root, '..', '..', 'english.report.json'))
    if not os.path.isfile(report):
        report = os.path.join(EN_ROOT or ROOT, 'build/patch/english.report.json')
    syms = module_syms_text(report)
    if syms is not None:
        entries.append(('@module.syms', None, syms, encode(b'', syms)))
    # the audit's knowledge of the build (the compiled-code range) travels
    # INSIDE the pack: a sidecar file next to it got separated from the pack
    # (a copied en.tktp, the battery worktree) and the audit then flagged our
    # own IDO code as retail (2026-11-02, PSX.EXE+0x878A0 / +0x87C0D)
    meta = sidecar(report, require='PSX.EXE' in {rel for rel, _, _, _ in entries})
    outs = {rel: out for rel, _, out, _ in entries}
    for rng in meta['compiled_code']:
        f, lo, hi = rng
        rng.append(hashlib.sha1(outs.get(f, b'')[lo:hi]).hexdigest())
    meta_text = (json.dumps(meta, sort_keys=True) + '\n').encode()
    entries.append((AUDIT_META, None, meta_text, encode(b'', meta_text)))
    blob = serialize(entries)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as fh:
        fh.write(blob)
    with open(out_path + '.json', 'w') as fh:
        json.dump(meta, fh, indent=1)
    print('translation_pack: %d files -> %s (%d bytes)' % (len(entries), out_path, len(blob)))
    return blob


def op_stats(ops):
    st = {'same': 0, 'copy': 0, 'mask_bytes': 0, 'mask_nibble_bytes': 0, 'fill': 0}
    for op in ops:
        if op[0] == SAME:
            st['same'] += op[1]
        elif op[0] == COPY:
            st['copy'] += op[2]
        elif op[0] == MASK:
            st['mask_bytes'] += len(op[1])
            st['mask_nibble_bytes'] += sum(1 for m in op[1] if m)
        elif op[0] == FILL:
            st['fill'] += op[1]
    return st


AUDIT_META = '@audit.json'   # the audit's metadata, carried in the pack (ASCII JSON)


def embedded_meta(entries):
    """the pack's own '@audit.json' (dict), or None for an older pack"""
    for e in entries:
        if e['path'] == AUDIT_META:
            try:
                return json.loads(apply_ops(e['ops'], b'', e['out_size']).decode('ascii'))
            except (ValueError, UnicodeDecodeError):
                return {'compiled_code': []}
    return None


def _base_of(e, retail_roots):
    for root in retail_roots:
        p = os.path.join(root, e['path'])
        if os.path.isfile(p):
            return _read(p)
    return b''


EXE_LOAD, EXE_HDR = 0x80041000, 0x800
# The patch module's data the BUILDER fills after the link (build_patched.py:
# the DTE / module-string pointer tables, the kanji subset, the kanji
# limit).  The native engine (pc/hal/lang.c import_module_data) reads them
# out of the English executable image at these PS1 addresses.
MODULE_DATA = ('patch_dte_table', 'patch_str_table', 'patch_kanji_sub_codes',
               'patch_kanji_sub_glyphs', 'patch_kanji_sub_count', 'patch_kanji_limit')


def module_syms_text(report=os.path.join(EN_ROOT or ROOT, 'build/patch/english.report.json')):
    """'@module.syms': '<name> 0x<addr>' lines (ASCII; addresses only)."""
    try:
        syms = json.loads(_read_text(report))['module']['symbols']
    except (OSError, KeyError, ValueError):
        return None
    lines = ['%s %s' % (n, syms[n]) for n in MODULE_DATA if n in syms]
    return ('\n'.join(lines) + '\n').encode()


def sidecar(report=os.path.join(EN_ROOT or ROOT, 'build/patch/english.report.json'),
            require=False):
    """What the audit needs to know about the patch build: the file range of
    the patch MODULE's compiled code (src/patch/main/*.c, IDO): an
    instruction sequence there can coincide with retail code that touches
    the same globals, which is not retail content.  require: the pack
    carries PSX.EXE, so a missing range is an error (the audit would flag
    the module's own code as retail), never an empty list."""
    meta = {'compiled_code': []}
    try:
        mod = json.loads(_read_text(report))['module']
        lo, hi = int(mod['base'], 16), int(mod['data_start'], 16)
        meta['compiled_code'].append(['PSX.EXE', lo - EXE_LOAD + EXE_HDR, hi - EXE_LOAD + EXE_HDR])
    except (OSError, KeyError, ValueError) as e:
        if require:
            raise SystemExit('translation_pack: the pack carries PSX.EXE but the patch '
                             'module range cannot be read from %s (%s: %s); build the '
                             'English patch (its report sits beside the staging)'
                             % (report, type(e).__name__, e))
    return meta


# ---------------------------------------------------------------- apply
def apply_pack(blob, retail_root, out_root):
    for e in parse(blob):
        if e['path'].startswith('@'):
            continue                    # metadata, not a disc file
        src = os.path.join(retail_root, e['path'])
        base = _read(src) if e['base_size'] else b''
        if e['base_size'] and hashlib.sha1(base).hexdigest() != e['base_sha1']:
            raise SystemExit('translation_pack: %s is not the supported retail file' % src)
        out = apply_ops(e['ops'], base, e['out_size'])
        if hashlib.sha1(out).hexdigest() != e['out_sha1']:
            raise SystemExit('translation_pack: result hash mismatch for ' + e['path'])
        dst = os.path.join(out_root, e['path'])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fh:
            fh.write(out)
    return 0


# ---------------------------------------------------------------- audit
AUDIT_RUN = 12      # an 'ours' run this long that also occurs in a retail file fails
SJIS_LEAD = set(range(0x81, 0xA0)) | set(range(0xE0, 0xF0))


def literal_runs(ops):
    """Maximal runs of fully written bytes (mask 0xFF) inside MASK ops, with
    their output offset: the only places an image/text chunk could hide."""
    pos = 0
    for op in ops:
        if op[0] == MASK:
            m, v = op[1], op[2]
            k = 0
            while k < len(m):
                if m[k] != 0xFF:
                    k += 1
                    continue
                s = k
                while k < len(m) and m[k] == 0xFF:
                    k += 1
                yield pos + s, bytes(v[s:k])
            pos += len(m)
        elif op[0] == SAME:
            pos += op[1]
        elif op[0] == COPY:
            pos += op[2]
        elif op[0] == FILL:
            pos += op[1]


def _trivial(chunk):
    return len(set(chunk)) <= 2


def _english(chunk):
    """Printable ASCII and NUL padding only: our English text (or a short
    coincidence with it), never retail Japanese text or pixels."""
    return all(b == 0 or 0x20 <= b < 0x7F for b in chunk)


def audit(blob, retail_roots, extra_retail_files=(), meta=None):
    """The pack-content audit.  Fails (returns problems) when:
      * a FILE in the pack has base_size 0 (a new file is all literal: it
        must be listed in ALLOWED_NEW, which is empty: the pack never adds
        files today);
      * any literal run of >= AUDIT_RUN bytes (not a 1-2 value pattern) also
        occurs in a retail file of the pack's base set or `extra` files:
        retail text or pixels stored instead of copied;
      * a file's literal bytes exceed half of its size: that is a re-encoded
        whole asset, not an edit.
    Reports per category totals for the record."""
    entries = parse(blob)
    problems = []
    embedded = embedded_meta(entries)
    if meta is not None and embedded is not None and \
            [r[:3] for r in meta.get('compiled_code', [])] != \
            [r[:3] for r in embedded.get('compiled_code', [])]:
        problems.append('%s: the sidecar disagrees with the pack\'s own metadata' % AUDIT_META)
    if embedded is not None:
        meta = embedded
    code = []
    outs = None
    for rng in (meta or {}).get('compiled_code', []):
        f, lo, hi = rng[:3]
        # a code range is honoured only for the executable, at most the
        # module's 64 KiB, and only when the pack's bytes there are the ones
        # the build recorded (the range cannot be moved over other content)
        if f != 'PSX.EXE' or not 0 <= lo < hi or hi - lo > 0x10000:
            problems.append('%s: compiled_code range %r refused' % (AUDIT_META, rng))
            continue
        if len(rng) > 3:
            if outs is None:
                outs = {e['path']: e for e in entries}
            e = outs.get(f)
            got = hashlib.sha1(apply_ops(e['ops'], _base_of(e, retail_roots), e['out_size'])[lo:hi]
                               ).hexdigest() if e else None
            if got != rng[3]:
                problems.append('%s: compiled_code bytes of %s 0x%X-0x%X are not the build\'s'
                                % (AUDIT_META, f, lo, hi))
                continue
        code.append((f, lo, hi))
    bases = {}
    for e in entries:
        for root in retail_roots:
            p = os.path.join(root, e['path'])
            if os.path.isfile(p):
                bases[e['path']] = _read(p)
                break
    corpus = list(bases.values()) + [_read(p) for p in extra_retail_files]
    # index every AUDIT_RUN-gram of the retail corpus (skip trivial ones)
    grams = set()
    for data in corpus:
        for i in range(0, len(data) - AUDIT_RUN + 1):
            g = data[i:i + AUDIT_RUN]
            if not _trivial(g):
                grams.add(g)
    cats = {}
    for e in entries:
        st = op_stats(e['ops'])
        cat = category(e['path'])
        c = cats.setdefault(cat, {'files': 0, 'ours_bytes': 0, 'copied': 0, 'kept': 0})
        c['files'] += 1
        c['ours_bytes'] += st['mask_nibble_bytes'] + (1 if st['fill'] else 0)
        c['copied'] += st['copy']
        c['kept'] += st['same']
        if e['path'].startswith('@'):
            data = apply_ops(e['ops'], b'', e['out_size'])
            if not all(b in (9, 10, 13) or 0x20 <= b < 0x7F for b in data):
                problems.append('%s: metadata entry is not plain ASCII text' % e['path'])
            continue
        if e['base_size'] == 0:
            problems.append('%s: a new file (all literal) in the pack' % e['path'])
        if st['mask_nibble_bytes'] > e['out_size'] // 2:
            problems.append('%s: %d of %d bytes are literal -- a whole asset, not an edit'
                            % (e['path'], st['mask_nibble_bytes'], e['out_size']))
        for off, run in literal_runs(e['ops']):
            for i in range(0, len(run) - AUDIT_RUN + 1):
                g = run[i:i + AUDIT_RUN]
                if not _trivial(g) and not _english(g) and g in grams:
                    if any(e['path'] == f and lo <= off + i < hi for f, lo, hi in code):
                        continue      # our compiled code coinciding with retail code
                    problems.append('%s+0x%X: a %d-byte literal run occurs in a retail file (%s)'
                                    % (e['path'], off + i, AUDIT_RUN, g.hex()))
                    break
    return problems, cats


def category(path):
    if path.startswith('@'):
        return 'metadata (patch-module symbol addresses, ASCII)'
    if path == 'PSX.EXE':
        return 'main executable (strings, pointer words, patch module, kanji subset)'
    if path.endswith('.EXN'):
        return 'overlay strings + pointer words'
    if path.endswith('.NBN') or path.startswith('CDROM/BATTLE/') or path in art_files():
        return 'art (lettering pixels, erase masks, sprite frames)'
    return 'script containers (English text, DTE dictionary, pointers)'


_ART_FILES = None


def art_files():
    """the disc files the art manifests (src/patch/art/*/manifest.json)
    write: OMIKUJI, the BUNKASD stage files, the TAIIKU / EN_GAME panels"""
    global _ART_FILES
    if _ART_FILES is None:
        _ART_FILES = set()
        art = str(english_path('src/patch/art')) if EN_ROOT else os.path.join(ROOT, 'src/patch/art')
        for d in sorted(os.listdir(art)) if os.path.isdir(art) else ():
            p = os.path.join(art, d, 'manifest.json')
            if os.path.isfile(p):
                m = json.loads(_read_text(p))
                _ART_FILES.add(m['file'])
                _ART_FILES.update(c['file'] for c in m.get('copies', []))
    return _ART_FILES


# ---------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--patched', default=os.path.join(EN_ROOT, 'build/patch/iso/staging')
                   if EN_ROOT else None)
    b.add_argument('--retail', default=os.path.join(ROOT, 'pc/assets/raw'))
    b.add_argument('--out', default=os.path.join(ROOT, 'pc/build/translation/en.tktp'))
    b.add_argument('--quiet', action='store_true')
    a = sub.add_parser('apply')
    a.add_argument('pack')
    a.add_argument('retail')
    a.add_argument('out')
    u = sub.add_parser('audit')
    u.add_argument('pack')
    u.add_argument('--retail', action='append', default=None)
    u.add_argument('--json', action='store_true')
    d = sub.add_parser('dump')
    d.add_argument('pack')
    args = ap.parse_args(argv)
    if args.cmd == 'build':
        if args.patched is None:
            sys.exit('translation_pack.py build: toki1-english not found (../toki1-english, '
                     'TOKI_ENGLISH_DIR): the pack is built from the English patch build '
                     '(or pass --patched DIR)')
        build(args.patched, args.retail, args.out, args.quiet)
        return 0
    if args.cmd == 'apply':
        return apply_pack(_read(args.pack), args.retail, args.out)
    if args.cmd == 'audit':
        roots = args.retail or [os.path.join(ROOT, 'pc/assets/raw')]
        blob = _read(args.pack)
        meta = json.loads(_read_text(args.pack + '.json')) if os.path.exists(args.pack + '.json') else None
        problems, cats = audit(blob, roots, meta=meta)
        if args.json:
            print(json.dumps({'size': len(blob), 'categories': cats, 'problems': problems}, indent=1))
        else:
            print('pack %s: %d bytes' % (args.pack, len(blob)))
            for k, v in sorted(cats.items()):
                print('  %-70s %3d files  ours %8d B  copied %8d B  kept %9d B'
                      % (k, v['files'], v['ours_bytes'], v['copied'], v['kept']))
            for p in problems:
                print('  PROBLEM ' + p)
            print('audit: %s' % ('FAIL (%d)' % len(problems) if problems else 'OK'))
        return 1 if problems else 0
    if args.cmd == 'dump':
        for e in parse(_read(args.pack)):
            st = op_stats(e['ops'])
            print('%-32s %8d B %5d ops  %s' % (e['path'], e['out_size'], len(e['ops']), st))
        return 0


if __name__ == '__main__':
    sys.exit(main())
