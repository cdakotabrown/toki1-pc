#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""audio_realtime.py -- capture and check what the port's REAL-TIME audio
device plays.

The playthroughs and the AV-parity tools hear the deterministic game-thread
mix (pc/hal/parity.c, one VBlank's samples at each VBlank).  A player hears
the SDL device's callback instead, on its own thread and clock.  This tool
runs a playthrough script the way a player runs the game -- no PC_TURBO,
the device mixing (--audio-mix host) -- with SDL's disk audio driver, which
writes every buffer the callback produced to a file at the device's pace
(SDL_AUDIODRIVER=disk, SDL_DISKAUDIOFILE), and analyses it:

  health    the player log's "[AUDIO] health" line (pc/hal/spu.c): late
            callbacks, key-on / key-off writes the SPU core never applied,
            the voice-register queue, XA sectors dropped and FIFO gaps
  gaps      runs of digital silence inside sound (dropouts)
  repeats   a non-silent block repeated verbatim (a stuck buffer)
  stereo    seconds where one channel is silent or the two cancel
  envelope  the 20 ms loudness envelope against a reference capture (the
            deterministic mix of the same script): missing notes lower it

    python3 pc/tools/audio_realtime.py capture SCRIPT --out DIR [--profile enhanced]
            [--deterministic] [--set NAME=VALUE ...]
    python3 pc/tools/audio_realtime.py analyze FILE.raw [--ref REF.raw]

The disk driver paces itself with SDL_Delay(23 ms) per 1,024-frame buffer
plus the mixing time, so its clock runs a few percent slow; that is the
point of a real-time test (the game's clock and the device's differ), not a
defect of the capture.  PC_TEST_PLATFORM=windows runs the Windows exe under
Wine (playthrough.py), with the same driver.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
RATE = 44100

HEALTH = re.compile(
    r'\[AUDIO\] health(?P<final> \(run total\))?: callbacks (?P<callbacks>\d+) \(frames (?P<min_frames>\d+)-(?P<max_frames>\d+), late (?P<late>\d+), '
    r'longest gap (?P<gap>[\d.]+) ms, period (?P<period>[\d.]+) ms, longest mix (?P<mix>[\d.]+) ms\), '
    r'clipped (?P<clipped>\d+); key-on writes (?P<kon_writes>\d+), key-on lost (?P<kon_lost>\d+), '
    r'key-off lost (?P<koff_lost>\d+), key-off over a pending key-on (?P<kon_koff>\d+); '
    r'voice writes queued (?P<queued>\d+) \(latency (?P<latency>\d+) ms \+ adaptive (?P<extra>[\d.]+) ms '
    r'\(at most (?P<max_extra>[\d.]+)\), late (?P<rt_late>\d+), '
    r'latest (?P<rt_latest>[\d.]+) ms, resyncs (?P<resyncs>\d+), flushed (?P<flushed>\d+)'
    r'(?: \(key-ons cancelled (?P<flush_cancel>\d+)\))?, '
    r'overflow (?P<overflow>\d+)(?:, dropped by a restore (?P<dropped_restore>\d+))?\); xa sectors (?P<xa>\d+), dropped (?P<xa_dropped>\d+), '
    r'gaps (?P<xa_gaps>\d+) \((?P<xa_gap_frames>\d+) frames\)')
DEVICE = re.compile(r'\[SPU\] audio device: mixer (\d+) Hz (\d+) ch (\d+) frames .*-> obtained (\d+) Hz '
                    r'(\d+) ch (\d+) frames')
DRIVER = re.compile(r'\[SPU\] audio driver (\S+)')


# ------------------------------------------------------------------ parsing
def health(log_text):
    """the run's totals (the last health line) as a dict of numbers, or None"""
    found = None
    for m in HEALTH.finditer(log_text):
        found = {k: (v if k == 'final' else float(v) if '.' in v else int(v))
                 for k, v in m.groupdict().items() if v is not None}
        found['final'] = bool(m.group('final'))
    return found


def device(log_text):
    m = DEVICE.search(log_text)
    d = DRIVER.search(log_text)
    if not m:
        return None
    w = tuple(int(x) for x in m.groups())
    return dict(driver=d.group(1) if d else None, want=w[:3], have=w[3:])


def load(path):
    """s16le stereo -> (frames, 2) int16"""
    a = np.fromfile(path, dtype='<i2')
    return a[: len(a) // 2 * 2].reshape(-1, 2)


# ------------------------------------------------------------------ analysis
def _rms(x):
    return float(np.sqrt(np.mean(np.square(x.astype(np.float64))))) if len(x) else 0.0


def gaps(pcm, min_ms=3, max_ms=400, around_ms=60, loud=150.0):
    """[(start_s, ms)]: runs of exact digital silence (both channels 0) of
    min_ms..max_ms with sound (RMS > `loud`) on both sides -- a dropout,
    not the silence between two songs."""
    silent = np.all(pcm == 0, axis=1)
    out = []
    if not len(silent):
        return out
    edges = np.flatnonzero(np.diff(np.concatenate(([0], silent.view(np.int8), [0]))))
    around = around_ms * RATE // 1000
    for s, e in zip(edges[::2], edges[1::2]):
        n = e - s
        if n < min_ms * RATE // 1000 or n > max_ms * RATE // 1000:
            continue
        before, after = pcm[max(0, s - around):s], pcm[e:e + around]
        if _rms(before) > loud and _rms(after) > loud:
            out.append((s / RATE, n * 1000.0 / RATE))
    return out


def repeats(pcm, block=512, loud=150.0):
    """[start_s]: a non-silent block followed by an identical one"""
    n = len(pcm) // block
    out = []
    for i in range(1, n):
        a, b = pcm[(i - 1) * block:i * block], pcm[i * block:(i + 1) * block]
        if _rms(a) > loud and np.array_equal(a, b):
            out.append(i * block / RATE)
    return out


def stereo(pcm, loud=300.0):
    """per-second problems: ('mono-silent', s, ch) where one channel is
    silent while the other is loud; ('cancel', s) where L+R carries less
    than 5% of L-R's energy (a channel inverted)"""
    out = []
    for s in range(len(pcm) // RATE):
        seg = pcm[s * RATE:(s + 1) * RATE].astype(np.float64)
        l, r = _rms(seg[:, 0]), _rms(seg[:, 1])
        if max(l, r) < loud:
            continue
        if min(l, r) < max(l, r) * 0.01:
            out.append(('mono-silent', s, 'L' if l < r else 'R'))
            continue
        if _rms(seg[:, 0] + seg[:, 1]) < 0.05 * _rms(seg[:, 0] - seg[:, 1]):
            out.append(('cancel', s))
    return out


def envelope(pcm, ms=20):
    hop = RATE * ms // 1000
    n = len(pcm) // hop
    if not n:
        return np.zeros(0)
    seg = pcm[: n * hop].astype(np.float64).reshape(n, hop, 2)
    return np.sqrt(np.mean(np.square(seg), axis=(1, 2)))


def envelope_match(pcm, ref, ms=20, max_lag_s=3.0):
    """(best correlation, lag in s) of the loudness envelopes (log scale),
    over the shorter capture, with `pcm` shifted against `ref`"""
    a, b = np.log1p(envelope(pcm, ms)), np.log1p(envelope(ref, ms))
    n = min(len(a), len(b))
    if n < 50:
        return 0.0, 0.0
    a, b = a[:n], b[:n]
    best = (-1.0, 0)
    max_lag = int(max_lag_s * 1000 / ms)
    for lag in range(-max_lag, max_lag + 1):
        x = a[max(0, lag):n + min(0, lag)]
        y = b[max(0, -lag):n - max(0, lag)]
        if len(x) < 50 or x.std() == 0 or y.std() == 0:
            continue
        c = float(np.corrcoef(x, y)[0, 1])
        if c > best[0]:
            best = (c, lag)
    return best[0], best[1] * ms / 1000.0


def analyze(path, ref=None):
    pcm = load(path)
    res = dict(seconds=len(pcm) / RATE, gaps=gaps(pcm), repeats=repeats(pcm), stereo=stereo(pcm),
               peak=int(np.abs(pcm.astype(np.int32)).max()) if len(pcm) else 0)
    if ref is not None:
        res['envelope'] = envelope_match(pcm, load(ref))
    return res


# ------------------------------------------------------------------ capture
def capture(script, out, profile='enhanced', deterministic=False, settings=(), max_frames=12000,
            timeout=1500):
    """Run `script` (a playthrough script file) and return dict(rc, log, raw,
    dir).  Real time (default): device mixing, SDL disk driver, no turbo.
    deterministic: the playthrough's game-thread mix, its output dumped
    with PC_AUDIO_DUMP."""
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    raw = out / 'audio.raw'
    if raw.exists():
        raw.unlink()
    cmd = [sys.executable, str(ROOT / 'pc/tools/playthrough.py'), str(script), '--out', str(out / 'run'),
           '--profile', profile, '--max-frames', str(max_frames)]
    if deterministic:
        cmd += ['--set', f'PC_AUDIO_DUMP={raw}']
    else:
        cmd += ['--audio-mix', 'host', '--set', 'PC_TURBO=0', '--set', 'SDL_AUDIODRIVER=disk',
                '--set', f'SDL_DISKAUDIOFILE={raw}']
    for kv in settings:
        cmd += ['--set', kv]
    with open(out / 'playthrough.log', 'w') as fh:
        r = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=ROOT, timeout=timeout)
    logs = sorted((out / 'run').glob('*.pc.log'))
    text = ''.join(p.read_text(errors='replace') for p in logs)
    return dict(rc=r.returncode, log=text, raw=raw, dir=out,
                playthrough=(out / 'playthrough.log').read_text(errors='replace'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture')
    c.add_argument('script')
    c.add_argument('--out', required=True)
    c.add_argument('--profile', default='enhanced', choices=('parity', 'enhanced'))
    c.add_argument('--deterministic', action='store_true')
    c.add_argument('--max-frames', type=int, default=12000)
    c.add_argument('--set', action='append', default=[])
    a = sub.add_parser('analyze')
    a.add_argument('raw')
    a.add_argument('--ref')
    args = ap.parse_args()
    if args.cmd == 'capture':
        r = capture(args.script, args.out, args.profile, args.deterministic, args.set, args.max_frames)
        print(f'rc {r["rc"]}; device {device(r["log"])}')
        print(f'health {health(r["log"])}')
        if r['raw'].exists():
            print(f'audio {analyze(r["raw"])}')
        return r['rc']
    print(analyze(args.raw, args.ref))
    return 0


if __name__ == '__main__':
    sys.exit(main())
