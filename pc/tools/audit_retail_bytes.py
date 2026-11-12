#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_retail_bytes.py -- the port's binaries must carry no retail bytes.

    python3 pc/tools/audit_retail_bytes.py BINARY [BINARY ...]
        [--assets pc/assets] [--min-run 64] [--json OUT]

A player download is the executable alone; the game's data comes from the
player's own disc at first run.  This gate looks for the disc's bytes inside
the built executables (Linux ELF, Windows PE):

  - every file of the extracted disc except the streamed media (.XA, .STR,
    .DA: ADPCM / MDEC / CD audio) is cut into 32-byte chunks at 32-byte
    alignment; chunks with fewer than 8 distinct byte values (zero fill,
    0xFF fill, short repeats) are left out, as every program has those;
  - every offset of the binary is hashed over 32 bytes and looked up (numpy);
  - each hit is verified byte for byte and extended both ways to the full
    identical run.  Any run of at least --min-run bytes (64 by default:
    always contains an aligned 32-byte chunk) is a finding.
Exit 1 with a report when a finding exists, 0 when clean, 2 when the asset
tree or a binary is missing.  The disc is the player's own; this tool only
compares and never writes any of its bytes out (the report names files and
offsets).

Negative control: a build that still links pc/src/game_data.c (the
405,504-byte copy of PSX.EXE's 0x80086000-0x800E9000 data the port used to
carry) must fail -- pc/tools/test_audit_retail_bytes.py checks that on a
synthetic image, and the first run of this gate found exactly that blob.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
CHUNK = 32
MEDIA = ('.XA', '.STR', '.DA')
M1, M2, M3, M4 = (np.uint64(0x9E3779B97F4A7C15), np.uint64(0xC2B2AE3D27D4EB4F),
                  np.uint64(0x165667B19E3779F9), np.uint64(0x27D4EB2F165667C5))


def mix(w0, w1, w2, w3):
    h = w0 * M1
    h ^= (w1 * M2) + (h >> np.uint64(29))
    h ^= (w2 * M3) + (h >> np.uint64(31))
    h ^= (w3 * M4) + (h >> np.uint64(27))
    return h


def source_files(assets):
    raw = Path(assets) / 'raw'
    out = []
    for p in sorted(raw.rglob('*')):
        if p.is_file() and p.suffix.upper() not in MEDIA and p.stat().st_size >= CHUNK:
            out.append(p)
    return out


def index_sources(files):
    """-> (sorted hashes, file id per hash, offset per hash) of the aligned,
    non-trivial 32-byte chunks."""
    hashes, fids, offs = [], [], []
    for fid, p in enumerate(files):
        data = np.frombuffer(p.read_bytes(), dtype=np.uint8)
        n = len(data) // CHUNK
        if not n:
            continue
        chunks = data[:n * CHUNK].reshape(n, CHUNK)
        srt = np.sort(chunks, axis=1)
        distinct = 1 + (np.diff(srt, axis=1) != 0).sum(axis=1)
        keep = np.nonzero(distinct >= 8)[0]
        if not len(keep):
            continue
        w = chunks[keep].copy().view(np.uint64)         # (k, 4)
        hashes.append(mix(w[:, 0], w[:, 1], w[:, 2], w[:, 3]))
        fids.append(np.full(len(keep), fid, dtype=np.int32))
        offs.append((keep * CHUNK).astype(np.int64))
    h = np.concatenate(hashes)
    order = np.argsort(h, kind='stable')
    return h[order], np.concatenate(fids)[order], np.concatenate(offs)[order]


def binary_hashes(data):
    """hash of data[i:i+32] for every i (the last 31 offsets excepted)."""
    n = len(data) - CHUNK + 1
    out = np.empty(max(n, 0), dtype=np.uint64)
    for phase in range(8):
        words = np.frombuffer(data[phase:phase + (len(data) - phase) // 8 * 8], dtype=np.uint64)
        k = len(words) - 3
        if k <= 0:
            continue
        h = mix(words[:k], words[1:k + 1], words[2:k + 2], words[3:k + 3])
        idx = phase + 8 * np.arange(k)
        valid = idx < n
        out[idx[valid]] = h[valid]
    return out


def tools_for(binary):
    with open(binary, 'rb') as fh:
        pe = fh.read(2) == b'MZ'
    pre = ROOT / 'build/mingw-root/usr/bin/x86_64-w64-mingw32-'
    if pe and Path(f'{pre}objdump').is_file():
        return f'{pre}objdump', f'{pre}nm', pe
    return 'objdump', 'nm', pe


def sections(binary):
    """[(file_offset, size, name, vma)] from objdump -h (ELF or PE)."""
    objdump, _, pe = tools_for(binary)
    try:
        out = subprocess.run([objdump, '-h', str(binary)], capture_output=True, text=True).stdout
        if pe:   # objdump -h prints PE VMAs without the image base
            hdr = subprocess.run([objdump, '-p', str(binary)], capture_output=True, text=True).stdout
            base = next((int(l.split()[1], 16) for l in hdr.splitlines() if l.startswith('ImageBase')), 0)
        else:
            base = 0
    except OSError:
        return []
    rows = []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 7 and f[0].isdigit():
            vma = int(f[3], 16)
            rows.append((int(f[5], 16), int(f[2], 16), f[1], vma if not pe or vma >= base else vma + base))
    return rows


def symbols(binary):
    """sorted [(address, name)] of the data/bss symbols (nm)."""
    _, nm, _ = tools_for(binary)
    try:
        out = subprocess.run([nm, '-n', str(binary)], capture_output=True, text=True).stdout
    except OSError:
        return []
    rows = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'dDbBrRgGsS':
            rows.append((int(f[0], 16), f[2]))
    return rows


def place(secs, syms, off):
    """(section, symbol+offset) of a file offset."""
    import bisect
    for start, size, name, vma in secs:
        if start <= off < start + size:
            addr = vma + (off - start)
            i = bisect.bisect_right([a for a, _ in syms], addr) - 1 if syms else -1
            sym = f'{syms[i][1]}+0x{addr - syms[i][0]:x}' if i >= 0 else '?'
            return name, sym
    return '?', '?'


def audit(binary, files, index, min_run):
    hs, fids, offs = index
    data = Path(binary).read_bytes()
    bh = binary_hashes(data)
    pos = np.searchsorted(hs, bh)
    pos[pos >= len(hs)] = len(hs) - 1
    cand = np.nonzero(hs[pos] == bh)[0]
    cache = {}
    runs = []
    covered_until = -1
    for i in cand.tolist():
        if i < covered_until:
            continue
        j = int(pos[i])
        # every source chunk with this hash (collisions of equal chunks)
        best = None
        while j < len(hs) and hs[j] == bh[i]:
            fid, so = int(fids[j]), int(offs[j])
            src = cache.get(fid)
            if src is None:
                src = cache[fid] = files[fid].read_bytes()
            if data[i:i + CHUNK] == src[so:so + CHUNK]:
                a, b, sa = i, i + CHUNK, so
                while a > 0 and sa > 0 and data[a - 1] == src[sa - 1]:
                    a, sa = a - 1, sa - 1
                se = so + CHUNK
                while b < len(data) and se < len(src) and data[b] == src[se]:
                    b, se = b + 1, se + 1
                if best is None or b - a > best[1] - best[0]:
                    best = (a, b, fid, sa)
            j += 1
        if best and best[1] - best[0] >= min_run:
            runs.append(best)
            covered_until = best[1]
    secs = sections(binary)
    syms = symbols(binary)
    findings = []
    for a, b, fid, sa in runs:
        name = str(files[fid]).split('/raw/', 1)[-1]
        sec, sym = place(secs, syms, a)
        findings.append(dict(binary_offset=a, length=b - a, section=sec, symbol=sym,
                             source=name, source_offset=sa))
    return findings


def load_allow(path):
    """[(disc file, start, end, reason)] from retail_bytes_allow.txt"""
    rows = []
    if not path or not Path(path).is_file():
        return rows
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        f = line.split(None, 3)
        start, length = int(f[1], 16), int(f[2], 0)
        rows.append((f[0], start, start + length, f[3] if len(f) > 3 else ''))
    return rows


def allowed(finding, allow):
    a, b = finding['source_offset'], finding['source_offset'] + finding['length']
    for src, s, e, why in allow:
        if src == finding['source'] and s <= a and b <= e:
            return why
    return None


IDENT = set(b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_\0')
_DATA = {}


def data_of(binary):
    if binary not in _DATA:
        _DATA.clear()
        _DATA[binary] = Path(binary).read_bytes()
    return _DATA[binary]


def symbol_names(finding, data):
    """Outside every section (a PE's COFF symbol / string table, kept for
    dladdr) and nothing but identifier characters and NULs: function names
    the decompilation shares with the disc's debug build (O.BIN).  Names,
    not data."""
    if finding['section'] != '?':
        return None
    run = data[finding['binary_offset']:finding['binary_offset'] + finding['length']]
    return 'symbol names (identifiers) in the symbol table' if set(run) <= IDENT else None


def run_audit(binaries, assets, min_run=64, allow_path=None, show=20, json_out=None):
    """-> (exit status, {binary: findings}).  Prints the report."""
    if min_run < CHUNK * 2:
        raise ValueError(f'min_run must be at least {CHUNK * 2} (a shorter run need not contain an aligned chunk)')
    if not (Path(assets) / 'raw/PSX.EXE').is_file():
        print(f'audit_retail_bytes: no extracted disc at {assets} (raw/PSX.EXE)')
        return 2, {}
    allow = load_allow(allow_path)
    files = source_files(assets)
    index = index_sources(files)
    print(f'audit_retail_bytes: {len(files)} disc files, {len(index[0])} distinct 32-byte chunks indexed; '
          f'{len(allow)} allowed source literal(s)')
    report, bad = {}, 0
    for b in binaries:
        if not Path(b).is_file():
            print(f'audit_retail_bytes: missing binary {b}')
            return 2, report
        found = audit(b, files, index, min_run)
        for x in found:
            x['allowed'] = allowed(x, allow) or symbol_names(x, data_of(b))
        report[str(b)] = found
        live = [x for x in found if not x['allowed']]
        print(f'{b}: {len(live)} run(s) of >= {min_run} disc bytes ({sum(x["length"] for x in live)} bytes); '
              f'{len(found) - len(live)} allowed source literal(s)')
        for x in sorted(live, key=lambda x: -x['length'])[:show]:
            print(f'  {x["length"]:8d} bytes at 0x{x["binary_offset"]:x} ({x["section"]} {x["symbol"]}) == '
                  f'{x["source"]} +0x{x["source_offset"]:x}')
        bad += len(live)
    if json_out:
        Path(json_out).write_text(json.dumps(report, indent=2) + '\n')
    print('audit_retail_bytes: ' + ('FAIL' if bad else 'clean'))
    return (1 if bad else 0), report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('binaries', nargs='+')
    ap.add_argument('--assets', default=str(ROOT / 'pc/assets'))
    ap.add_argument('--min-run', type=int, default=64)
    ap.add_argument('--allow', default=str(ROOT / 'pc/tools/retail_bytes_allow.txt'),
                    help='allowed source literals (default %(default)s; "" for none)')
    ap.add_argument('--json', help='write the findings here')
    ap.add_argument('--show', type=int, default=20, help='findings listed per binary')
    a = ap.parse_args(argv)
    try:
        status, _ = run_audit(a.binaries, a.assets, a.min_run, a.allow, a.show, a.json)
    except ValueError as e:
        ap.error(str(e))
    return status


if __name__ == '__main__':
    sys.exit(main())
