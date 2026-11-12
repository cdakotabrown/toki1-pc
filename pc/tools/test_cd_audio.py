# SPDX-License-Identifier: GPL-3.0-or-later
"""The CD-ROM controller's XA audio path (pc/hal/cd_audio.c), asset-free.

Decode and resampling are checked against an independent Python model of
psx-spx's XA-ADPCM decoder and the 37.8/18.9 kHz -> 44.1 kHz interpolators,
then the controller behaviour the drive model relies on: the file/channel
filter, the current-file lock, the FIFO on the system clock (silence queued
when the SPU ran dry, a sector dropped when >10 frames are still queued),
the decoder reset discarding what the SPU has not taken, Mute and the ATV
matrix, and the real-time mixer's jitter margin (TOKI_XA_MARGIN_MS; the
other tests pop the FIFO directly, so they run with the margin off).  On retail (scene 20, the surname voice line) the PCM is bit-exact
with the reference emulator once aligned."""
import ctypes
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

from safe_process import run
from pc_paths import NO_HISTORY, have_commit

ROOT = Path(__file__).resolve().parents[2]
TICKS = 768

STUB = r'''
#include "types.h"
int (*test_source)(s16 *, s16 *);
void spu_psx_set_cd_audio_source(int (*source)(s16 *, s16 *)) { test_source = source; }
int test_pop(s16 *l, s16 *r) { return test_source(l, r); }
'''

POS = (0, 60, 115, 98)
NEG = (0, 0, -52, -55)


def clamp(v):
    return max(-32768, min(32767, v))


def model_decode(sector, hist):
    """psx-spx XA-ADPCM: returns a list of frames (l, r)."""
    coding = sector[3]
    stereo, eight = coding & 1, coding & 0x10
    nblk = 4 if eight else 8
    out = []
    for g in range(18):
        grp = sector[8 + g * 128: 8 + (g + 1) * 128]
        blocks = []
        for b in range(nblk):
            hdr = grp[4 + b]
            shift = hdr & 15
            shift = 9 if shift > 12 else shift
            f = (hdr >> 4) & 3
            ch = (b & 1) if stereo else 0
            samples = []
            for w in range(28):
                word = grp[16 + w * 4: 20 + w * 4]
                code = word[b] if eight else (word[b >> 1] >> ((b & 1) * 4)) & 15
                raw = (code << (8 if eight else 12)) & 0xFFFF
                raw = raw - 0x10000 if raw & 0x8000 else raw
                s = raw >> shift
                p0, p1 = hist[ch]
                v = clamp(s + ((p0 * POS[f]) >> 6) + ((p1 * NEG[f]) >> 6))
                hist[ch] = (v, p0)
                samples.append(v)
            blocks.append(samples)
        if stereo:
            for b in range(0, nblk, 2):
                out.extend(zip(blocks[b], blocks[b + 1]))
        else:
            for b in range(nblk):
                out.extend((s, s) for s in blocks[b])
    return out


ZIGZAG = [
    [0, 0, 0, 0, 0, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054, 0x0034, 0x0009, -0x010A, 0x0400,
     -0x0A78, 0x234C, 0x6794, -0x1780, 0x0BCD, -0x0623, 0x0350, -0x016D, 0x006B, 0x000A, -0x0010,
     0x0011, -0x0008, 0x0003, -0x0001],
    [0, 0, 0, -0x0002, 0, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2, -0x00E3, 0x0132, -0x0043,
     -0x0267, 0x0C9D, 0x74BB, -0x11B4, 0x09B8, -0x05BF, 0x0372, -0x01A8, 0x00A6, -0x001B, 0x0005,
     0x0006, -0x0008, 0x0003, -0x0001, 0],
    [0, 0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3, -0x0192, 0x02B1, -0x039E,
     0x04F8, -0x05A6, 0x7939, -0x05A6, 0x04F8, -0x039E, 0x02B1, -0x0192, 0x00B3, -0x004A, 0x001F,
     -0x0005, -0x0002, 0x0003, -0x0001, 0, 0],
    [0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8, 0x0372, -0x05BF,
     0x09B8, -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2, -0x004B, 0x003C,
     -0x0013, 0x0003, 0, -0x0002, 0, 0, 0],
    [-0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D, 0x0350, -0x0623, 0x0BCD,
     -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400, -0x010A, 0x0009, 0x0034, -0x0054, 0x0041, -0x0022,
     0x000A, -0x0001, 0, 0x0001, 0, 0, 0],
    [0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B, -0x0548, 0x0AFA, -0x16FA,
     0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347, 0x015B, -0x0044, -0x0017, 0x0046, -0x0023, 0x0011,
     -0x0005, 0, 0, 0, 0, 0, 0],
    [-0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347, 0x080E, -0x1249, 0x3C07,
     0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B, -0x00EB, 0x001A, 0x002B, -0x0023, 0x0010, -0x0008,
     0x0002, 0, 0, 0, 0, 0, 0],
]
# 18.9 kHz phase k: 25 taps, applied oldest-first over ring[p-25 .. p-1].
# They are the zigzag rows realigned, except one tap of phase 5 (-2, the
# row's -1): the weights Mednafen and the reference emulator use.
HALF = [[0] + ZIGZAG[6][:24], ZIGZAG[0][4:29], ZIGZAG[1][3:28], ZIGZAG[2][2:27],
        ZIGZAG[3][1:26], ZIGZAG[4][0:25], [0] + ZIGZAG[5][0:24]]
HALF[5][23] = -0x0002


class Resampler:
    def __init__(self):
        self.ring = [[0] * 32, [0] * 32]
        self.p, self.six = 0, 6

    def full(self, frames):
        out = []
        for l, r in frames:
            self.ring[0][self.p], self.ring[1][self.p] = l, r
            self.p = (self.p + 1) & 31
            self.six -= 1
            if self.six == 0:
                self.six = 6
                for t in ZIGZAG:
                    out.append(tuple(clamp(sum((self.ring[c][(self.p - i) & 31] * t[i]) >> 15
                                               for i in range(29))) for c in (0, 1)))
        return out

    def half(self, frames):
        out, n = [], 0
        while n < len(frames):
            if self.six >= 7:
                self.six -= 7
                self.p = (self.p + 1) & 31
                self.ring[0][self.p], self.ring[1][self.p] = frames[n]
                n += 1
            t = HALF[self.six]
            out.append(tuple(clamp(sum(self.ring[c][(self.p - 25 + i) & 31] * t[i]
                                       for i in range(25)) >> 15) for c in (0, 1)))
            self.six += 3
        return out


def sector(file=1, channel=0, coding=0x01, submode=0x64, seed=1):
    rnd = random.Random(seed)
    s = bytearray(rnd.getrandbits(8) for _ in range(2336))
    s[:8] = bytes((file, channel, submode, coding)) * 2
    for g in range(18):
        for b in range(16):   # sound parameters: filter 0..3, shift 0..12
            s[8 + g * 128 + b] = (rnd.randrange(4) << 4) | rnd.randrange(13)
    return bytes(s)


class LibMixin:
    def load(self, margin_ms, source='pc/hal/cd_audio.c'):
        # read once per library instance (each load is a fresh copy)
        os.environ['TOKI_XA_MARGIN_MS'] = margin_ms
        self.addCleanup(os.environ.pop, 'TOKI_XA_MARGIN_MS', None)
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        path = Path(self.dir.name)
        (path / 'stub.c').write_text(STUB)
        lib = path / 'cd_audio.so'
        run(['gcc', '-std=gnu99', '-O2', '-shared', '-fPIC', '-Ipc/include', '-Ipc/include/game',
             '-Ipc/hal', str(source), str(path / 'stub.c'), '-o', str(lib)],
            cwd=ROOT, timeout=60).require_ok()
        self.lib = ctypes.CDLL(str(lib))
        self.lib.cd_audio_reset_decoder.argtypes = [ctypes.c_uint64]
        self.lib.cd_audio_xa_sector.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint64]
        self.lib.cd_audio_mix_window.argtypes = [ctypes.c_uint64, ctypes.c_uint32]
        self.lib.cd_audio_set_filter.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib.cd_audio_set_volume.argtypes = [ctypes.c_char_p]
        self.lib.cd_audio_init()

    def pop(self, n):
        l, r = ctypes.c_int16(), ctypes.c_int16()
        out = []
        for _ in range(n):
            self.lib.test_pop(ctypes.byref(l), ctypes.byref(r))
            out.append((l.value, r.value))
        return out


class CdAudioTests(LibMixin, unittest.TestCase):
    def setUp(self):
        self.load('0')

    def test_decode_and_resample_match_the_model(self):
        for coding in (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14):
            with self.subTest(coding=hex(coding)):
                self.setUp()
                hist, rs, expected = [(0, 0), (0, 0)], Resampler(), []
                for k in range(3):
                    s = sector(coding=coding, seed=k + coding)
                    frames = model_decode(s, hist)
                    expected += rs.half(frames) if coding & 4 else rs.full(frames)
                    self.lib.cd_audio_xa_sector(s, 0, 0)
                got = self.pop(len(expected) + 4)
                self.assertEqual(got[:len(expected)], expected)
                self.assertEqual(got[len(expected):], [(0, 0)] * 4)   # FIFO ran dry: silence

    def queued(self):
        l, r = ctypes.c_int16(), ctypes.c_int16()
        n = 0
        while self.lib.test_pop(ctypes.byref(l), ctypes.byref(r)):
            n += 1
        return n

    def test_frames_per_sector(self):
        # 37.8 kHz: 7 frames per 6 samples; 18.9 kHz: 7 per 3, the last
        # sample's final phase emitted only when the next sector arrives
        # (retail: the first voice sector leaves the FIFO one frame short)
        for coding, frames in ((0x01, 3 * 2352), (0x00, 3 * 4704), (0x04, 3 * 9408 - 1)):
            with self.subTest(coding=hex(coding)):
                self.setUp()
                for k in range(3):
                    self.lib.cd_audio_xa_sector(sector(coding=coding, seed=k), 0, 0)
                self.assertEqual(self.queued(), frames)

    def test_filter_and_current_file(self):
        self.lib.cd_audio_set_filter(1, 3)
        self.lib.cd_audio_xa_sector(sector(channel=2), 1, 0)       # other channel: skipped
        self.assertEqual(self.pop(4), [(0, 0)] * 4)
        self.lib.cd_audio_xa_sector(sector(channel=3), 1, 0)
        self.assertNotEqual(self.pop(2352), [(0, 0)] * 2352)
        # without the filter the first file/channel read locks until EOF
        self.setUp()
        self.lib.cd_audio_xa_sector(sector(channel=255), 0, 0)     # junk channel: skipped
        self.lib.cd_audio_xa_sector(sector(channel=5, submode=0xE4), 0, 0)   # locks 5, then EOF
        self.lib.cd_audio_xa_sector(sector(channel=6), 0, 0)       # EOF released the lock
        self.lib.cd_audio_xa_sector(sector(channel=5), 0, 0)       # now 6 holds it
        played = ctypes.c_uint32(); dropped = ctypes.c_uint32(); skipped = ctypes.c_uint32()
        self.lib.cd_audio_stats(ctypes.byref(played), ctypes.byref(dropped), ctypes.byref(skipped))
        self.assertEqual((played.value, skipped.value), (2, 2))

    def test_fifo_on_the_system_clock(self):
        self.lib.cd_audio_mix_window(TICKS * 1000 + 100, 1000)    # grid at 1000 samples
        grid = TICKS * 1000
        # arrives 300.5 samples after the batch ended: the SPU played 300 of silence
        self.lib.cd_audio_xa_sector(sector(seed=7), 0, grid + 300 * TICKS + 384)
        got = self.pop(302)
        self.assertEqual(got[:300], [(0, 0)] * 300)
        self.assertNotEqual(got[300:], [(0, 0)] * 2)
        # a sector landing while >10 frames are still queued is dropped
        self.lib.cd_audio_mix_window(TICKS * 2000, 1000)          # grid 2000: 1000 popped by then
        self.lib.cd_audio_xa_sector(sector(seed=8), 0, (2000 + 10) * TICKS)
        stats = [ctypes.c_uint32() for _ in range(3)]
        self.lib.cd_audio_stats(*[ctypes.byref(s) for s in stats])
        self.assertEqual(stats[1].value, 1)
        # a reset (Pause) keeps only what the SPU took before it
        self.lib.cd_audio_reset_decoder((2000 + 40) * TICKS)
        self.pop(40)
        self.assertEqual(self.pop(4), [(0, 0)] * 4)

    def test_mute_and_volume_matrix(self):
        self.lib.cd_audio_set_mute(1)
        self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=3), 0, 0)
        self.assertEqual(self.pop(2352), [(0, 0)] * 2352)
        self.setUp()
        s = sector(coding=0x01, seed=4)
        ref = Resampler().full(model_decode(s, [(0, 0), (0, 0)]))
        self.lib.cd_audio_set_volume(bytes((79, 79, 79, 79)))   # SD_StereoModeSet mono
        self.lib.cd_audio_xa_sector(s, 0, 0)
        got = self.pop(len(ref))
        self.assertEqual(got, [(clamp(((l * 79) >> 7) + ((r * 79) >> 7)),) * 2 for l, r in ref])


class RealTimeMarginTests(LibMixin, unittest.TestCase):
    """The real-time mixer (no cd_audio_mix_window) starts a stream only once
    its first data has waited the margin, and again after running dry."""
    def setUp(self):
        self.load('10')             # 441 frames

    def test_stream_waits_the_margin_then_plays_every_frame(self):
        self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=1), 0, 0)    # 2352 frames
        got = self.pop(441 + 2352 + 4)
        self.assertEqual(got[:441], [(0, 0)] * 441)                       # the margin
        self.assertNotEqual(got[441:451], [(0, 0)] * 10)
        self.assertEqual(got[-4:], [(0, 0)] * 4)                          # ran dry
        # a dry FIFO re-primes: the next sector waits the margin again
        self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=2), 0, 0)
        again = self.pop(441 + 1)
        self.assertEqual(again[:441], [(0, 0)] * 441)
        self.assertNotEqual(again[441], (0, 0))

    def test_negative_control_without_the_margin_the_first_frame_plays(self):
        self.load('0')
        self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=1), 0, 0)
        self.assertNotEqual(self.pop(10), [(0, 0)] * 10)

    def tail_after_pause(self):
        self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=1), 0, 0)    # 2352 frames
        self.pop(441 + 100)                                               # margin + 100 played
        self.lib.cd_audio_reset_decoder(0)                                # Pause
        return self.pop(451 + 4)

    def test_a_pause_keeps_the_margin_tail(self):
        """STATE-MACHINES.md C8: the real-time FIFO plays a margin late; at
        a Pause the frames the console had already played (the margin, and
        the low watermark) stay.  Negative control: cd_audio.c before the
        fix drops them (the line's tail was clipped)."""
        got = self.tail_after_pause()
        self.assertNotEqual(got[440:451], [(0, 0)] * 11)
        self.assertEqual(got[451:], [(0, 0)] * 4)
        if not have_commit('84553342'):
            self.skipTest(NO_HISTORY % '84553342')
        old = Path(self.dir.name) / 'cd_audio_before.c'
        old.write_text(subprocess.run(['git', 'show', '84553342:pc/hal/cd_audio.c'], cwd=ROOT,
                                      capture_output=True, text=True, check=True).stdout)
        self.load('10', source=old)
        self.assertEqual(self.tail_after_pause()[:451], [(0, 0)] * 451)

    def test_trim_keeps_the_newest_frames(self):
        """C6: cd_audio_trim (fast-forward released) keeps the newest frames"""
        for k in range(4):
            self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=10 + k), 0, 0)
        full = self.pop(441 + 4 * 2352)[441:]
        self.load('10')
        for k in range(4):
            self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=10 + k), 0, 0)
        self.lib.cd_audio_trim(500)
        got = self.pop(441 + 504)[441:]
        self.assertEqual(got[:500], full[-500:])
        self.assertEqual(got[500:], [(0, 0)] * 4)

    def test_the_deterministic_mixer_has_no_margin(self):
        self.lib.cd_audio_mix_window(0, 0)                  # the per-VBlank mixer runs
        self.lib.cd_audio_xa_sector(sector(coding=0x01, seed=1), 0, 0)
        self.assertNotEqual(self.pop(10), [(0, 0)] * 10)


if __name__ == '__main__':
    unittest.main()
