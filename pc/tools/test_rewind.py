# SPDX-License-Identifier: GPL-3.0-or-later
"""Rewind (pc/hal/rewind.c) on the real port, enhanced profile.

The rewind buffer keeps a snapshot of the machine every few game frames (the
bytes a quickload restores; the newest whole, older ones as XOR deltas,
pc/hal/rewind_codec.c) and, while the rewind key is held, restores older
ones.  These tests drive it with the toolkit's `rewind_to F` (the same
restore the key makes, without a key: hotkeys never act in automated runs)
on date_save.txt's opening and Shiori's phone call, resolved to absolute
input, in the deterministic audio mode (PC_PARITY_DIR):

* round trip, mid-music and mid-voice: in ONE process the game runs past a
  snapshot S to S+K (the toolkit saves the machine there), on to a later
  frame, rewinds to S and runs to S+K again (saved again).  The two S+K
  machines must be equal -- PS1 RAM, MMIO, the game stack, VRAM, SPU RAM and
  every device model -- and so must what was presented and heard between S
  and S+K: per frame, FNV hashes of VRAM, the scanout and PS1 RAM
  (PC_FRAME_HASH, PC_FRAME_HASH_RAM) and per mixer call the mixed PCM
  (PC_AUDIO_HASH).  The points: the name entry (the streamed BGM378.XA and a
  sequenced song) and Shiori's call (her voice line streaming from
  CDROM/XA_G/; cd_audio's decoded-sector count rises across the snapshot).
* the live memory card is kept: rewinding from after the album's creation to
  before it leaves the written directory on the card (a quickload's rule).
* negative controls: PC_REWIND_NEGATIVE=spu / =pram leave the SPU RAM / PS1
  RAM out of the restore; then the S+K machines differ (and with spu, the
  audio after the rewind), so the comparisons see a broken restore.
  PC_REWIND_NEGATIVE=card restores the card as well: the album's
  directory entry disappears.
* not in automated runs: without TOKI_REWIND=1, an enhanced harness run
  takes no snapshot, and `rewind` answers that there is nothing to rewind.

Needs the built port and pc/assets; skipped otherwise.  Six power-on runs
to Shiori's call (turbo), a minute or two each.
"""
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402

ROOT = Path(rt.ROOT)
BINARY = ROOT / 'pc/build/tokimeki-pc'
WIN_BINARY = ROOT / 'pc/build-win/tokimeki-pc.exe'   # the Windows build, under Wine
WINE = shutil.which('wine')
ASSETS = ROOT / 'pc/assets'
ENV = {'TOKI_PROFILE': 'enhanced', 'PC_TRACE_PHASE': 'pre_irq'}
K = 90                 # game frames compared after the snapshot
SNAP = re.compile(r'\[REWIND\] snapshot frame (\d+) vblank (\d+) calls (\d+)')
RESTORED = re.compile(r'\[REWIND\] restored frame (\d+) vblank (\d+) calls (\d+)')
PEEK16 = re.compile(r'\[DBG\] cmds line \d+ at frame (\d+): peek memcard\.c:g_card\+0x80 16\n'
                    r'\[DBG\] ok 0x\w+ 16 ([0-9a-f]+)')


def call_script(path):
    """date_save.txt up to the end of Shiori's phone call (the calendar)."""
    lines = []
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        lines.append(line)
        if line.startswith('wait calendar '):
            break
    lines.append('end 60')
    Path(path).write_text('\n'.join(lines) + '\n')


def by_key(lines, key):
    """lines -> {key: [line, ...]} in file order"""
    out = {}
    for line in lines:
        k = key(line)
        if k is not None:
            out.setdefault(k, []).append(line)
    return out


def frame_vblank(line):
    m = re.search(r' vblank=(\d+) ', line)
    return int(m.group(1)) if m else None


def frame_body(line):
    """a frame-hash line without its running index"""
    return line.split(' ', 1)[1]


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class RewindRoundTrip(unittest.TestCase):
    binary = BINARY
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='rewind-test-')
        cls.syms = sst.Symbols(str(cls.binary))
        script = os.path.join(cls.tmp, 'call.txt')
        call_script(script)
        cls.reached, cls.abs_input, _ = rt.resolve(script, cls.tmp, str(cls.binary), 1200, ENV)
        cls.pb = cls.reached['phonebook']
        cls.music_at = cls.reached['surname_entry'] + 60
        cls.voice_at = cls.pb + 560
        cls.card_before = cls.reached['sysfile_prompt'] - 2
        cls.card_after = cls.reached['surname_entry'] - 5
        # discovery: the snapshot schedule of this input (deterministic)
        _, text = cls.run_case('discover', f'at {cls.voice_at + K + 60} quit\n',
                               {'TOKI_REWIND': '1', 'PC_REWIND_LOG': '1'})
        cls.snaps = [(int(f), int(v), int(c)) for f, v, c in SNAP.findall(text)]
        cls.stats = re.findall(r'\[REWIND\] (on: [^\n]*)', text)
        cls.coded = [int(n) for n in re.findall(r'\[REWIND\] coded frame \d+: delta (\d+) bytes', text)]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def run_case(cls, tag, cmds, extra):
        d = os.path.join(cls.tmp, tag)
        os.makedirs(d, exist_ok=True)
        inp = os.path.join(d, 'abs.input')
        with open(inp, 'w') as fh:
            fh.write(cls.abs_input)
        with open(os.path.join(d, 'cmds'), 'w') as fh:
            fh.write(cmds)
        env = dict(ENV, PC_INPUT=inp, PC_DBG_CMDS=os.path.join(d, 'cmds'),
                   PC_PARITY_DIR=os.path.join(d, 'parity'), PC_PARITY_FRAMES='1',
                   PC_FRAME_HASH=os.path.join(d, 'frames.txt'),
                   PC_FRAME_HASH_RAM='cdq' if rt.is_windows(cls.binary) else '1',
                   PC_AUDIO_HASH=os.path.join(d, 'audio.txt'),
                   TOKI_SAVE_DIR=os.path.join(d, 'saves'))
        env.update(extra)
        rc, text = rt.run_port(str(cls.binary), os.path.join(d, 'work'), os.path.join(d, 'log'), env, 1200)
        assert rc == 0, text[-3000:]
        return d, text

    def snapshot_at_or_before(self, frame):
        s = [x for x in self.snaps if x[0] <= frame]
        self.assertTrue(s, f'no snapshot before {frame}: {self.snaps[:5]}')
        return s[-1]

    def rewind_case(self, tag, negative=None):
        """Two points in one process: rewind to the music snapshot, then to
        the voice snapshot.  -> {point: (state at S+K pass 1, pass 2, S)}"""
        points = {}
        cmds = ''
        for name, at in (('music', self.music_at), ('voice', self.voice_at)):
            s = self.snapshot_at_or_before(at)
            first = os.path.join(self.tmp, f'{tag}-{name}-1.tks')
            second = os.path.join(self.tmp, f'{tag}-{name}-2.tks')
            points[name] = (first, second, s)
            cmds += (f'at {s[0]} peek cd_audio.c:g_played 4\n'
                     f'at {s[0] + K} save {first}\n'
                     f'at {s[0] + K + 40} peek cd_audio.c:g_played 4\n'
                     f'at {s[0] + K + 45} rewind_to {s[0]}\n'
                     f'after {K} save {second}\n')
        cmds += 'after 1 quit\n'
        extra = {'TOKI_REWIND': '1', 'PC_REWIND_LOG': '1'}
        if negative:
            extra['PC_REWIND_NEGATIVE'] = negative
        d, text = self.run_case(tag, cmds, extra)
        return d, text, points

    def machine_diff(self, a, b):
        """-> {region: bytes differing, ...}, with the first symbols named"""
        sa = sst.read_state(a)
        rep = sst.diff_states(sa, sst.read_state(b), self.syms)
        out = {r: sum(h - l for l, h in rs) for r, rs in rep.items() if rs}
        if out:
            out['where'] = [n for ns in sst.diff_names(sa, rep, 8, self.syms).values() for n in ns]
        return out

    def test_1_snapshots_are_small_and_cheap(self):
        self.assertTrue(self.stats, 'rewind did not start')
        self.assertGreater(len(self.snaps), 300)
        # the snapshots continue through the scenes at the configured step
        steps = [b[0] - a[0] for a, b in zip(self.snaps, self.snaps[1:])]
        self.assertGreaterEqual(min(steps), 10, steps[:20])
        coded = sorted(self.coded)
        # median delta well under a megabyte (the whole image is ~13 MB)
        self.assertLess(coded[len(coded) // 2], 1 << 20, coded[-10:])

    def test_2_round_trip_mid_music_and_mid_voice(self):
        d, text, points = self.rewind_case('rt')
        restored = RESTORED.findall(text)
        self.assertEqual(len(restored), 2, text[-2000:])
        frames = by_key(Path(d, 'frames.txt').read_text().splitlines(), frame_vblank)
        audio = by_key(Path(d, 'audio.txt').read_text().splitlines(), lambda l: int(l.split()[0]))
        played = [int(v) for v in re.findall(
            r'peek cd_audio\.c:g_played 4\n\[DBG\] ok 0x\w+ \d+ \w+ u=(\d+)', text)]
        for i, (name, (first, second, s)) in enumerate(points.items()):
            with self.subTest(point=name):
                self.assertEqual(int(restored[i][0]), s[0])
                self.assertEqual(int(restored[i][2]), s[2], 'the mixer count is machine state')
                # XA sectors are being decoded across the snapshot
                self.assertGreater(played[2 * i + 1], played[2 * i], name)
                self.assertEqual(self.machine_diff(first, second), {}, name)
                # presented frames S..S+K: the second pass equals the first
                window = [v for v in frames if s[1] < v <= s[1] + K]
                self.assertGreaterEqual(len(window), K - 5)
                for v in window:
                    seen = frames[v]
                    self.assertGreaterEqual(len(seen), 2, (name, v))
                    self.assertEqual(frame_body(seen[-1]), frame_body(seen[-2]), (name, v))
                # mixed audio over the same VBlanks, call by call
                calls = [c for c in range(s[2], s[2] + K)]
                hashes = set()
                for c in calls:
                    self.assertGreaterEqual(len(audio.get(c, [])), 2, (name, c))
                    self.assertEqual(audio[c][-1], audio[c][-2], (name, c))
                    hashes.add(audio[c][-1].split()[2])
                self.assertGreater(len(hashes), K // 2, f'{name}: the window is not silent')

    def test_3_negative_controls_see_a_broken_restore(self):
        for neg, region in (('spu', 'spuram'), ('pram', 'pram')):
            with self.subTest(negative=neg):
                d, text, points = self.rewind_case(f'neg-{neg}', neg)
                first, second, _ = points['music']
                diff = self.machine_diff(first, second)
                self.assertIn(region, diff, diff)
                if neg == 'spu':
                    audio = by_key(Path(d, 'audio.txt').read_text().splitlines(),
                                   lambda l: int(l.split()[0]))
                    s = points['music'][2]
                    differ = [c for c in range(s[2], s[2] + K)
                              if len(audio.get(c, [])) >= 2 and audio[c][-1] != audio[c][-2]]
                    self.assertTrue(differ, 'the audio after a broken restore differs')

    def card_case(self, tag, negative=None):
        s = self.snapshot_at_or_before(self.card_before)
        cmds = (f'at {s[0]} peek memcard.c:g_card+0x80 16\n'
                f'at {self.card_after} peek memcard.c:g_card+0x80 16\n'
                f'at {self.card_after} rewind_to {s[0]}\n'
                f'peek memcard.c:g_card+0x80 16\n'
                'after 1 quit\n')
        extra = {'TOKI_REWIND': '1', 'PC_REWIND_LOG': '1'}
        if negative:
            extra['PC_REWIND_NEGATIVE'] = negative
        _, text = self.run_case(tag, cmds, extra)
        rows = re.findall(r'peek memcard\.c:g_card\+0x80 16\n\[DBG\] ok 0x\w+ 16 ([0-9a-f]+)', text)
        self.assertEqual(len(rows), 3, text[-2000:])
        self.assertIn('rewound to frame', text)
        return rows

    def test_4_the_memory_card_is_kept(self):
        before, after, rewound = self.card_case('card')
        self.assertNotEqual(before, after, 'the album creation writes the directory')
        self.assertEqual(rewound, after, 'a rewind keeps the live card')
        # negative control: restoring the card too brings the old directory back
        before, after, rewound = self.card_case('card-neg', 'card')
        self.assertEqual(rewound, before)

    def test_6_the_held_key_steps_back_and_resumes_there(self):
        # the key held at frame F for 4 shown steps: the newest snapshot, then
        # three older ones; the game resumes at the 4th newest before F
        f = self.voice_at + K
        target = [x for x in self.snaps if x[0] < f][-4]
        first = os.path.join(self.tmp, 'held-1.tks')
        second = os.path.join(self.tmp, 'held-2.tks')
        cmds = (f'at {f - 1} save {first}\n'
                'when rewind.c:g_restores=1 do echo rewound\n'
                f'at {f - 1} save {second}\n'
                'after 1 quit\n')
        _, text = self.run_case('held', cmds, {'TOKI_REWIND': '1', 'PC_REWIND_LOG': '1',
                                               'TOKI_REWIND_HOLD': f'{f}:4'})
        self.assertIn(f'[REWIND] rewinding from frame {f}', text)
        self.assertIn(f'after 4 step(s)', text)
        restored = RESTORED.findall(text)
        self.assertEqual([int(r[0]) for r in restored], [target[0]])
        self.assertEqual(self.machine_diff(first, second), {})

    def test_5_off_in_automated_runs(self):
        _, text = self.run_case('off', f'at {self.music_at} rewind 0\nafter 1 quit\n',
                                {'PC_REWIND_LOG': '1'})
        self.assertNotIn('[REWIND] on', text)
        self.assertNotIn('[REWIND] snapshot', text)
        self.assertIn('err nothing to rewind to', text)



@unittest.skipUnless(WIN_BINARY.is_file() and WINE and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the Windows build, Wine and pc/assets')
class RewindRoundTripWindows(RewindRoundTrip):
    """every test above on the Windows build under Wine (the frame RAM hash
    without the CD queue's uninitialised payloads: PC_FRAME_HASH_RAM=cdq)"""
    binary = WIN_BINARY


if __name__ == '__main__':
    unittest.main()
