#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect PC-port save states (pc/hal/dbg_state.c).

    savestate_tool.py info STATE
    savestate_tool.py diff A B [--bin BIN] [--limit N]
        compare two states region by region (PS1 RAM, MMIO, VRAM, SPU RAM,
        the other native records) and name the differing objects.  Exit 0
        when the machine state is identical.
    savestate_tool.py audit STATE... [--bin BIN]
        every 8-byte word of the saved native records that holds a pointer
        into the saving process's heap, stack, libraries or anonymous
        mappings, grouped by object: candidates for dbg_state.c's KEEP /
        QUIESCE rules (a host pointer restored into another process is a
        dangling pointer).
    savestate_tool.py extract STATE REGION OUT
        write one region (pram, mmio, vram, spuram, natv, stak) as raw bytes.
    savestate_tool.py records STATE
        list the native records (key, address, size).

Format 2 (`TOKISTv2`, Linux and Windows alike) names every native object in
the file, so no binary is needed to read or diff it; --bin is only checked
against the state's build id.  Format 1 (the old Linux process image) is
still read: its EXRW section is split by the binary's ELF symbols.
"""
import argparse
import bisect
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BIN = os.path.join(ROOT, 'pc/build/tokimeki-pc')
PAGE = 4096
SIG_MAGIC = b'TKSIGv01'          # pc/hal/state_sign.h
SIG_LEN = 8 + 32


class StateError(Exception):
    pass


def unpage(body, size, path='state'):
    buf, p = bytearray(), 0
    for off in range(0, size, PAGE):
        n = min(PAGE, size - off)
        if p >= len(body):
            raise StateError(f'{path}: truncated page data')
        t = body[p]
        p += 1
        if t:
            buf += body[p:p + n]
            p += n
        else:
            buf += bytes(n)
    return bytes(buf), p


def read_records(body, path):
    """NATV body -> {key: (addr, bytes)} in file order"""
    (count,) = struct.unpack_from('<I', body, 0)
    pos, out = 4, {}
    for _ in range(count):
        (kl,) = struct.unpack_from('<H', body, pos)
        key = body[pos + 2:pos + 2 + kl].decode('utf-8', 'replace')
        addr, size, stored = struct.unpack_from('<QQQ', body, pos + 2 + kl)
        pos += 2 + kl + 24
        data, used = unpage(body[pos:pos + stored], size, path)
        if used != stored:
            raise StateError(f'{path}: corrupt record {key}')
        out[key] = (addr, data)
        pos += stored
    return out


def read_state(path):
    """-> dict(format=1|2, info={k: v}, maps=str, keep=[(lo, hi)],
    regions={tag: (addr, bytes)}, records={key: (addr, bytes)} (format 2))"""
    with open(path, 'rb') as fh:
        data = fh.read()
    if data[:8] not in (b'TOKISTv1', b'TOKISTv2'):
        raise StateError(f'{path}: not a state file')
    # the installation's signature (pc/hal/state_sign.c): 8-byte magic and
    # an HMAC after the last section; this tool reads, it does not verify
    if len(data) >= 8 + SIG_LEN and data[-SIG_LEN:-SIG_LEN + 8] == SIG_MAGIC:
        data = data[:-SIG_LEN]
    pos = 8
    out = dict(format=int(data[7:8].decode()), info={}, maps='', keep=[], regions={}, records={})
    while pos + 32 <= len(data):
        tag = data[pos:pos + 4].decode('ascii')
        flags, addr, size, stored = struct.unpack_from('<IQQQ', data, pos + 4)
        body = data[pos + 32:pos + 32 + stored]
        if len(body) != stored:
            raise StateError(f'{path}: truncated section {tag}')
        pos += 32 + stored
        if flags & 1:
            body, _ = unpage(body, size, path)
        if tag == 'NATV':
            out['records'] = read_records(body, path)
            continue
        if tag == 'INFO':
            for line in body.decode('utf-8', 'replace').splitlines():
                k, _, v = line.partition('=')
                out['info'][k] = v
        elif tag == 'MAPS':
            out['maps'] = body.decode('utf-8', 'replace')
        elif tag == 'KEEP':
            vals = struct.unpack(f'<{len(body) // 8}Q', body)
            out['keep'] = list(zip(vals[0::2], vals[1::2]))
        else:
            out['regions'][tag] = (addr, body)
    return out


def pe_build_id(path):
    """the CodeView (RSDS) signature `ld --build-id` writes into a PE file's
    debug directory, as dbg_platform reads it at run time; None if absent"""
    with open(path, 'rb') as fh:
        d = fh.read()
    if d[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return None
    nsec, = struct.unpack_from('<H', d, pe + 6)
    opt_size, = struct.unpack_from('<H', d, pe + 20)
    opt = pe + 24
    if struct.unpack_from('<H', d, opt)[0] != 0x20B:          # PE32+
        return None
    dbg_rva, dbg_size = struct.unpack_from('<II', d, opt + 112 + 6 * 8)
    secs = []
    for i in range(nsec):
        h = opt + opt_size + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, h + 8)
        secs.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return raw + rva - va
        return None
    o = off(dbg_rva) if dbg_rva else None
    for i in range(dbg_size // 28 if o is not None else 0):
        typ, size, rva, ptr = struct.unpack_from('<IIII', d, o + 28 * i + 12)
        if typ == 2 and d[ptr:ptr + 4] == b'RSDS':
            return d[ptr + 4:ptr + 20].hex()
    return None


def build_id(binary):
    """the id dbg_state writes into a state's INFO (ELF GNU build-id, PE
    CodeView signature)"""
    bid = pe_build_id(binary)
    if bid:
        return bid
    r = subprocess.run(['readelf', '-n', binary], capture_output=True, text=True)
    m = re.search(r'Build ID: ([0-9a-f]+)', r.stdout)
    return m.group(1) if m else None


class Symbols:
    """the binary's data symbols (readelf -sW: FILE context for statics);
    a format-2 state names its own objects, so a Windows exe gets only its
    build id here"""

    def __init__(self, binary):
        self.rows, self.addrs = [], []
        self.build_id = pe_build_id(binary)
        if self.build_id:
            return
        r = subprocess.run(['readelf', '-sW', binary], capture_output=True, text=True)
        if r.returncode:
            raise StateError(f'readelf failed on {binary}')
        rows, file = [], None
        for line in r.stdout.splitlines():
            p = line.split()
            if len(p) < 8 or not p[0].endswith(':'):
                continue
            _, value, size, typ, bind, _vis, ndx, name = p[:8]
            if typ == 'FILE':
                file = name
                continue
            if typ in ('FUNC', 'SECTION') or ndx == 'UND':
                continue
            try:
                v, s = int(value, 16), int(size, 0)
            except ValueError:
                continue
            if not v:
                continue
            label = f'{file}:{name}' if bind == 'LOCAL' and file else name
            rows.append((v, s, label))
        rows.sort()
        self.addrs = [r[0] for r in rows]
        self.rows = rows
        self.build_id = None
        r = subprocess.run(['readelf', '-n', binary], capture_output=True, text=True)
        m = re.search(r'Build ID: ([0-9a-f]+)', r.stdout)
        if m:
            self.build_id = m.group(1)

    def name(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        while i >= 0:
            v, s, label = self.rows[i]
            if v <= addr < v + max(s, 1):
                return label, addr - v
            if addr - v > 1 << 20:
                break
            i -= 1
        return None, 0

    def lookup(self, label):
        for v, s, l in self.rows:
            if l == label or l.split(':')[-1] == label:
                return v, s
        return None, None


def check_build(state, syms, path):
    bid = state['info'].get('build_id')
    if syms.build_id and bid and bid != syms.build_id[:len(bid)]:
        raise StateError(f'{path} was written by build-id {bid}; --bin is {syms.build_id}')


def in_ranges(ranges, a):
    i = bisect.bisect_right([r[0] for r in ranges], a) - 1
    return i >= 0 and a < ranges[i][1]


def split_exrw(state, syms):
    """EXRW -> {'vram': (addr, bytes), 'spuram': ..., 'exrw': (addr, bytes)}"""
    addr, body = state['regions']['EXRW']
    out = {}
    for label, key in (('g_vram', 'vram'), ('SPU::s_ram', 'spuram'),
                       ('_ZN3SPUL5s_ramE', 'spuram')):
        v, s = syms.lookup(label)
        if v is not None and key not in out:
            out[key] = (v, body[v - addr:v - addr + s])
    return out


def diff_bytes(a, b, base):
    """-> list of (start, end) absolute byte ranges that differ"""
    out = []
    if a == b:
        return out
    n = min(len(a), len(b))
    step = 4096
    for off in range(0, n, step):
        x, y = a[off:off + step], b[off:off + step]
        if x == y:
            continue
        i = 0
        while i < len(x):
            if x[i] != y[i]:
                j = i
                while j < len(x) and x[j] != y[j]:
                    j += 1
                start, end = base + off + i, base + off + j
                if out and start == out[-1][1]:
                    out[-1] = (out[-1][0], end)
                else:
                    out.append((start, end))
                i = j
            else:
                i += 1
    return out


def cmd_info(a):
    st = read_state(a.state)
    for k, v in st['info'].items():
        print(f'{k}: {v}')
    for tag, (addr, body) in st['regions'].items():
        print(f'region {tag}: 0x{addr:x} {len(body)} bytes')
    if st['format'] == 2:
        print(f'native records: {len(st["records"])} '
              f'({sum(len(b) for _, b in st["records"].values())} bytes)')
    else:
        print(f'keep ranges: {len(st["keep"])} ({sum(h - l for l, h in st["keep"])} bytes)')
    return 0


def cmd_records(a):
    st = read_state(a.state)
    for key, (addr, body) in st['records'].items():
        print(f'{key}\t0x{addr:x}\t{len(body)}')
    return 0


# Host noise of the Windows build: cd_read_start (src/main/matched) packs
# a 4-byte tag into a 32-byte local and cdq_command_enqueue copies 16 bytes
# of it into a CD queue record (cdq_que, 48 records of 24 bytes), so
# data[4..14] and the flag of a READ record hold uninitialised host stack
# bytes, and keep them when the ring reuses the record for a command that
# copies no payload.  Nothing reads them (the traces, which read the live
# fields, are identical).  On Linux they are the same in every run (the
# fixed layout); on Windows they hold host wall-clock leftovers
# (milliseconds since start: 0x1db8 in one power-on, 0x1ff2 in the next)
# and differ between any two processes, with or without a save state.
# Comparisons of Windows states leave bytes 12..23 of the 48 records out
# (576 of PS1 RAM's 2 MB) and report them as host_noise.
CDQ_QUE, CDQ_REC, CDQ_N = 0xE8DA8, 24, 48


def cdq_noise(*rams):
    """[(lo, hi)] offsets into PS1 RAM of the CD queue records' payload
    tails (bytes 12..23 of each record)"""
    return [(CDQ_QUE + CDQ_REC * i + 12, CDQ_QUE + CDQ_REC * (i + 1)) for i in range(CDQ_N)]


def mask_ram(ram, noise):
    if not noise:
        return ram
    b = bytearray(ram)
    for lo, hi in noise:
        b[lo:hi] = bytes(hi - lo)
    return bytes(b)


def is_windows(state):
    return state['info'].get('platform', '').startswith('windows')


# the records diff_states reports as regions of their own
RECORD_REGIONS = {'g_vram': 'vram', 'spu.cpp:_ZN3SPUL5s_ramE': 'spuram'}


def name_of(state, addr):
    """`key+0xOFF` of the record (or `REGION+0xOFF`) holding ADDR, or hex"""
    for key, (a, body) in state.get('records', {}).items():
        if a <= addr < a + len(body):
            return f'{key}+0x{addr - a:x}'
    for tag, (a, body) in state['regions'].items():
        if a <= addr < a + len(body):
            return f'{tag}+0x{addr - a:x}'
    return hex(addr)


def diff_states(sa, sb, syms=None, limit=40, quiet=False):
    """-> {region: [(start, end)]} (addresses of A); KEEP ranges are ignored.
    Regions: pram, mmio, stak, and for format 2 vram, spuram and natv (every
    other native record), for format 1 vram, spuram and exrw."""
    if sa['format'] == 2 or sb['format'] == 2:
        if sa['format'] != sb['format']:
            raise StateError('a format-1 state cannot be compared with a format-2 one')
        return diff_states_v2(sa, sb)
    report = {}
    for tag in ('PRAM', 'MMIO', 'STAK'):
        if tag not in sa['regions'] or tag not in sb['regions']:
            continue
        aa, ba = sa['regions'][tag]
        ab, bb = sb['regions'][tag]
        report[tag.lower()] = diff_bytes(ba, bb, aa) if aa == ab else [(aa, aa + len(ba))]
    addr, ea = sa['regions']['EXRW']
    _, eb = sb['regions']['EXRW']
    keep = sorted(set(sa['keep']) | set(sb['keep']))
    ranges = [r for r in diff_bytes(ea, eb, addr)]
    sub = split_exrw(sa, syms)
    named = {k: [] for k in ('vram', 'spuram', 'exrw')}
    for lo, hi in ranges:
        # drop the KEEP parts
        pieces, cur = [], lo
        for kl, kh in keep:
            if kh <= cur or kl >= hi:
                continue
            if kl > cur:
                pieces.append((cur, kl))
            cur = max(cur, kh)
        if cur < hi:
            pieces.append((cur, hi))
        for p in pieces:
            key = 'exrw'
            for k in ('vram', 'spuram'):
                if k in sub and sub[k][0] <= p[0] < sub[k][0] + len(sub[k][1]):
                    key = k
            named[key].append(p)
    report.update(named)
    return report


def diff_names(state, report, n=12, syms=None):
    """{region: [object names]} of a diff_states report (the first N ranges
    of each region), named from STATE's records (format 2) or SYMS"""
    out = {}
    for r, rs in report.items():
        if not rs:
            continue
        if state.get('format') == 2:
            out[r] = [name_of(state, lo) for lo, _ in rs[:n]]
        else:
            out[r] = [(syms.name(lo)[0] if syms else None) or hex(lo) for lo, _ in rs[:n]]
    return out


def diff_states_v2(sa, sb):
    report = {}
    for tag in ('PRAM', 'MMIO', 'STAK'):
        if tag not in sa['regions'] or tag not in sb['regions']:
            continue
        aa, ba = sa['regions'][tag]
        ab, bb = sb['regions'][tag]
        if tag == 'PRAM' and (is_windows(sa) or is_windows(sb)):
            noise = cdq_noise(ba, bb)
            report['host_noise'] = diff_bytes(ba, bb, aa) if aa == ab else []
            ba, bb = mask_ram(ba, noise), mask_ram(bb, noise)
            masked = diff_bytes(ba, bb, aa) if aa == ab else []
            report['host_noise'] = [r for r in report['host_noise'] if r not in masked]
        report[tag.lower()] = (diff_bytes(ba, bb, aa) if aa == ab and len(ba) == len(bb)
                               else [(aa, aa + len(ba))])
    for r in ('vram', 'spuram', 'natv'):
        report[r] = []
    ra, rb = sa['records'], sb['records']
    for key in sorted(set(ra) | set(rb)):
        region = RECORD_REGIONS.get(key, 'natv')
        if key not in ra or key not in rb:
            addr, body = ra.get(key) or rb.get(key)
            report[region].append((addr, addr + max(len(body), 1)))
            continue
        (aa, da), (_, db) = ra[key], rb[key]
        if len(da) != len(db):
            report[region].append((aa, aa + max(len(da), 1)))
        else:
            report[region] += diff_bytes(da, db, aa)
    return report


def print_report(report, syms, limit, state=None):
    clean = True
    for region, ranges in report.items():
        n = sum(h - l for l, h in ranges)
        note = '  (game-stack leftovers: informational)' if region == 'stak' and ranges else ''
        print(f'{region:7s}: {"identical" if not ranges else f"{n} bytes differ in {len(ranges)} ranges"}{note}')
        if not ranges or region == 'stak':
            continue
        clean = False
        by = {}
        for lo, hi in ranges:
            if state is not None and state['format'] == 2:
                label = name_of(state, lo).rsplit('+', 1)[0]
            else:
                label, off = syms.name(lo)
            by.setdefault(label or hex(lo), []).append((lo, hi))
        for label, rs in list(by.items())[:limit]:
            print(f'    {label}: {sum(h - l for l, h in rs)} bytes, first 0x{rs[0][0]:x}')
        if len(by) > limit:
            print(f'    ... {len(by) - limit} more symbols')
    return clean


def cmd_diff(a):
    syms = Symbols(a.bin)
    sa, sb = read_state(a.a), read_state(a.b)
    check_build(sa, syms, a.a)
    check_build(sb, syms, a.b)
    for k in ('frame_counter', 'now_step', 'now_sub_step', 'now_sub_sub_step', 'overlay', 'vblanks'):
        va, vb = sa['info'].get(k), sb['info'].get(k)
        print(f'{k}: {va}' + ('' if va == vb else f' / {vb}  (DIFFERENT)'))
    report = diff_states(sa, sb, syms)
    return 0 if print_report(report, syms, a.limit, sa) else 1


def host_ranges(maps_text, exrw=None):
    """(class, lo, hi) of every mapping that is NOT the executable (its
    file-backed segments and the anonymous tail of its .bss) or the fixed
    PS1 windows: a word pointing there is a host pointer.  The map is
    /proc/self/maps, or the same lines dbg_platform writes on Windows."""
    out, exe = [], None
    xlo, xhi = exrw or (0, 0)
    for line in maps_text.splitlines():
        p = line.split(None, 5)
        if not p:
            continue
        lo, hi = (int(x, 16) for x in p[0].split('-'))
        path = p[5].strip() if len(p) > 5 else ''
        if exe is None and path and not path.startswith('[') and 'x' in p[1]:
            exe = path
        if path and path == exe:
            continue
        if 0x80000000 <= lo < 0x80200000 or lo == 0x1F801000:
            continue
        if lo < xhi and hi > xlo:
            continue
        cls = ('heap' if path == '[heap]' else 'stack' if path.startswith('[stack')
               else 'vdso' if path.startswith('[v') else 'lib' if path else 'anon')
        out.append((cls, lo, hi))
    return out


def cmd_audit(a):
    syms = Symbols(a.bin)
    found = {}
    for path in a.states:
        st = read_state(path)
        check_build(st, syms, path)
        if st['format'] == 2:
            recs = st['records'].values()
            own = ((min(a for a, _ in recs), max(a + len(b) for a, b in recs))
                   if st['records'] else None)
            blocks = [(addr, body, key) for key, (addr, body) in st['records'].items()]
        else:
            ex = st['info'].get('exrw', '0-0').split('-')
            own = (int(ex[0], 16), int(ex[1], 16))
            addr, body = st['regions']['EXRW']
            blocks = [(addr, body, None)]
        ranges = host_ranges(st['maps'], own)
        los = [r[1] for r in ranges]
        keep = st['keep']
        for addr, body, rkey in blocks:
            # the words at 8-byte aligned addresses
            skip = (-addr) % 8
            words = memoryview(body)[skip:skip + (len(body) - skip) // 8 * 8].cast('Q')
            for i, v in enumerate(words):
                if v < 0x10000:
                    continue
                j = bisect.bisect_right(los, v) - 1
                if j < 0 or v >= ranges[j][2]:
                    continue
                a_ = addr + skip + 8 * i
                if in_ranges(keep, a_):
                    continue
                if rkey is not None:
                    key, off = rkey, a_ - addr
                else:
                    label, off = syms.name(a_)
                    key = label or hex(a_)
                found.setdefault(key, set()).add((ranges[j][0], off, os.path.basename(path)))
    if not found:
        print('no host pointers outside the KEEP ranges')
        return 0
    for key in sorted(found):
        classes = sorted({c for c, _, _ in found[key]})
        offs = sorted({o for _, o, _ in found[key]})
        files = sorted({f for _, _, f in found[key]})
        print(f'{key}: {"/".join(classes)} at +{",".join(hex(o) for o in offs[:8])}'
              f'{" ..." if len(offs) > 8 else ""}  ({len(files)} state(s))')
    return 1


def cmd_extract(a):
    st = read_state(a.state)
    key = a.region.lower()
    if st['format'] == 2:
        if key in ('pram', 'mmio', 'stak'):
            data = st['regions'][key.upper()][1]
        elif key == 'natv':
            data = b''.join(b for _, b in st['records'].values())
        else:
            want = {v: k for k, v in RECORD_REGIONS.items()}[key]
            data = st['records'][want][1]
        with open(a.out, 'wb') as fh:
            fh.write(data)
        print(f'{a.out}: {len(data)} bytes')
        return 0
    syms = Symbols(a.bin)
    if key in ('pram', 'mmio', 'exrw'):
        data = st['regions'][key.upper()][1]
    else:
        data = split_exrw(st, syms)[key][1]
    with open(a.out, 'wb') as fh:
        fh.write(data)
    print(f'{a.out}: {len(data)} bytes')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info')
    p.add_argument('state')
    p = sub.add_parser('diff')
    p.add_argument('a')
    p.add_argument('b')
    p.add_argument('--bin', default=DEFAULT_BIN)
    p.add_argument('--limit', type=int, default=40)
    p = sub.add_parser('audit')
    p.add_argument('states', nargs='+')
    p.add_argument('--bin', default=DEFAULT_BIN)
    p = sub.add_parser('records')
    p.add_argument('state')
    p = sub.add_parser('extract')
    p.add_argument('state')
    p.add_argument('region', choices=['pram', 'mmio', 'exrw', 'vram', 'spuram', 'natv', 'stak'])
    p.add_argument('out')
    p.add_argument('--bin', default=DEFAULT_BIN)
    a = ap.parse_args()
    try:
        return dict(info=cmd_info, diff=cmd_diff, audit=cmd_audit, extract=cmd_extract,
                    records=cmd_records)[a.cmd](a)
    except StateError as e:
        print(f'error: {e}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
