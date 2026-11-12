#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A tiny SoundFont 2 for tests of SoundFont mode (pc/hal/soundfont.c).

Nothing here comes from a real instrument: the samples are computed waves
(a sine, a square, white noise from a fixed-seed generator), released into
the public domain by construction, so the file can be generated anywhere and
never needs to be committed.  The port ships no SoundFont.

  python3 pc/tools/make_test_sf2.py OUT.sf2 [--wave sine|square|silent]
                                            [--release SECONDS] [--gain-db DB]

Layout: bank 0 presets 0..127 all play one looped instrument (the --wave,
441 Hz at key 69, so a key k sounds 441 * 2**((k - 69) / 12) Hz); bank 128
preset 0 (the drum kit) plays a 0.2 s noise burst on every key.  --silent
makes every sample zero (the song must vanish if the SoundFont really
replaces it).  Envelope: instant attack, full sustain, --release seconds.
"""
import argparse
import math
import random
import struct
import sys

RATE = 44100
PERIOD = 100               # 441 Hz


def chunk(tag, data):
    if len(data) % 2:
        data += b'\0'
    return tag + struct.pack('<I', len(data)) + data


def lst(kind, *parts):
    return chunk(b'LIST', kind + b''.join(parts))


def name(s, n=20):
    return s.encode('ascii')[:n - 1].ljust(n, b'\0')


def timecents(seconds):
    return -32768 if seconds <= 0.001 else int(round(1200 * math.log2(seconds)))


def build(wave='sine', release=0.1, gain_db=0.0):
    tone = []
    for i in range(PERIOD * 4):
        if wave == 'silent':
            v = 0
        elif wave == 'square':
            v = 12000 if (i % PERIOD) < PERIOD // 2 else -12000
        else:
            v = int(round(16000 * math.sin(2 * math.pi * i / PERIOD)))
        tone.append(v)
    rng = random.Random(1995)
    noise = []
    for i in range(int(RATE * 0.2)):
        env = 1.0 - i / (RATE * 0.2)
        noise.append(0 if wave == 'silent' else int(rng.uniform(-12000, 12000) * env))
    pad = [0] * 46
    smpl = tone + pad + noise + pad
    t0, t1 = 0, len(tone)
    n0 = t1 + 46
    n1 = n0 + len(noise)
    sdta = lst(b'sdta', chunk(b'smpl', struct.pack(f'<{len(smpl)}h', *smpl)))

    shdr = (name('tone') + struct.pack('<5IBbHH', t0, t1, t0, t1, RATE, 69, 4, 0, 1)
            + name('noise') + struct.pack('<5IBbHH', n0, n1, n0, n1, RATE, 60, 0, 0, 1)
            + name('EOS') + struct.pack('<5IBbHH', 0, 0, 0, 0, 0, 0, 0, 0, 0))

    def gen(op, amount):
        return struct.pack('<Hh', op, amount)

    att = int(round(-gain_db * 10)) if gain_db < 0 else 0
    common = (gen(34, timecents(0)) + gen(36, timecents(0)) + gen(37, 0) + gen(38, timecents(release))
              + gen(48, att))
    igen = (common + gen(54, 1) + gen(53, 0)                       # looped tone
            + common + gen(54, 0) + gen(53, 1))                    # one-shot noise
    per_zone = len(common) // 4 + 2
    ibag = struct.pack('<HH', 0, 0) + struct.pack('<HH', per_zone, 0) + struct.pack('<HH', 2 * per_zone, 0)
    inst = name('tone') + struct.pack('<H', 0) + name('noise') + struct.pack('<H', 1) + name('EOI') + struct.pack('<H', 2)
    imod = b'\0' * 10

    phdr = b''
    pbag = b''
    pgen = b''
    zones = 0
    presets = [(p, 0, 0) for p in range(128)] + [(0, 128, 1)]
    for preset, bank, instrument in presets:
        phdr += name(f'{"drums" if bank else "wave"} {preset}') + struct.pack('<HHHIII', preset, bank, zones, 0, 0, 0)
        pbag += struct.pack('<HH', len(pgen) // 4, 0)
        pgen += gen(41, instrument)
        zones += 1
    phdr += name('EOP') + struct.pack('<HHHIII', 0, 0, zones, 0, 0, 0)
    pbag += struct.pack('<HH', len(pgen) // 4, 0)
    pgen += b'\0' * 4
    pmod = b'\0' * 10
    igen += b'\0' * 4
    pdta = lst(b'pdta', chunk(b'phdr', phdr), chunk(b'pbag', pbag), chunk(b'pmod', pmod),
               chunk(b'pgen', pgen), chunk(b'inst', inst), chunk(b'ibag', ibag),
               chunk(b'imod', imod), chunk(b'igen', igen), chunk(b'shdr', shdr))
    info = lst(b'INFO', chunk(b'ifil', struct.pack('<HH', 2, 1)), chunk(b'isng', b'EMU8000\0'),
               chunk(b'INAM', b'tokimeki test waves\0'))
    body = b'sfbk' + info + sdta + pdta
    return b'RIFF' + struct.pack('<I', len(body)) + body


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--wave', choices=('sine', 'square', 'silent'), default='sine')
    ap.add_argument('--release', type=float, default=0.1)
    ap.add_argument('--gain-db', type=float, default=0.0)
    a = ap.parse_args(argv)
    with open(a.out, 'wb') as f:
        f.write(build(a.wave, a.release, a.gain_db))
    return 0


if __name__ == '__main__':
    sys.exit(main())
