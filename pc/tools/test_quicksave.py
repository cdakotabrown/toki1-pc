# SPDX-License-Identifier: GPL-3.0-or-later
"""Quicksave / quickload slots (pc/hal/quicksave.c) on the real port.

The quicksave is a debug-toolkit save state written into a slot of the
player's saves folder (quicksave/slotN.tks) with a 160x120 thumbnail
(slotN.png) and an index file (slotN.ini).  These tests drive
it through the toolkit commands `qsave N` / `qload N` (the same code path as
the F5 / F9 hotkeys, which never fire in an automated run) on new_game.txt's
absolute input, in the deterministic audio mode (PC_PARITY_DIR):

* round trip, at two points: the name entry (surname_entry+60: the streamed
  XA track is playing -- cd_audio's decoded-sector count rises -- and the
  sequencer has a song, gSDInfo bit 0x100) and the first lesson (lesson+50:
  a song, no XA).  A reference run quicksaves at N and runs to N+K; a second
  process quickloads that slot at game frame 200 -- as a player pressing F9
  in a new session -- and runs to N+K.  The two N+K machines must be equal
  (PS1 RAM, MMIO, VRAM, SPU RAM, every restored device model) and so must
  the parity artifacts recorded after N: the mixed PCM (SPU voices and the CD
  audio input), the displayed image, VRAM and RAM at checkpoints.  The slot's
  files (state, PNG, index with the scene, build, profile and -- in a game --
  the date and stats) and the automatic undo slot 0 are checked too.
* slot selection (`qslot N`, what [ / ] do), the refusal of slot 0 as a
  save target and of another build's slot, and the slot list text.
* session state is not restored: a parity-profile slot loaded into an
  enhanced session leaves the session enhanced (profile.c is a KEEP file).
* the memory card is not rolled back: a slot saved over an empty card,
  loaded while the card holds a save, leaves the save on the card (else
  the game's next card write would flush the old image over card1.mcr).

Needs the built port and pc/assets; skipped otherwise.  Each round trip is
two power-on runs to ~N+K game frames (turbo), about a minute each.
"""
import configparser
import os
from pathlib import Path
import re
import shutil
import struct
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
SCRIPT = ROOT / 'pc/tools/playthrough/new_game.txt'
K = 180
PEEK = re.compile(r'\[DBG\] cmds line \d+ at frame (\d+): peek (\S+)[^\n]*\n\[DBG\] ok 0x\w+ \d+ \w+ u=(\d+)')


def peeks(text):
    out = {}
    for frame, name, value in PEEK.findall(text):
        out.setdefault(name, []).append((int(frame), int(value)))
    return out


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class QuicksaveRoundTrip(unittest.TestCase):
    binary = BINARY
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='quicksave-test-')
        cls.syms = sst.Symbols(str(cls.binary))
        cls.reached, cls.abs_input, _ = rt.resolve(str(SCRIPT), cls.tmp, str(cls.binary), 900)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_case(self, tag, cmds, extra=None, saves=None):
        d = os.path.join(self.tmp, tag)
        os.makedirs(d, exist_ok=True)
        inp = os.path.join(d, 'abs.input')
        with open(inp, 'w') as fh:
            fh.write(self.abs_input)
        with open(os.path.join(d, 'cmds'), 'w') as fh:
            fh.write(cmds)
        env = dict(PC_INPUT=inp, PC_DBG_CMDS=os.path.join(d, 'cmds'),
                   PC_PARITY_DIR=os.path.join(d, 'parity'), PC_PARITY_FRAMES=f'1,30,90,{K - 10}',
                   PC_PARITY_RAM='1', TOKI_SAVE_DIR=saves or os.path.join(self.tmp, 'saves'))
        env.update(extra or {})
        rc, text = rt.run_port(str(self.binary), os.path.join(d, 'work'), os.path.join(d, 'log'), env, 900)
        self.assertEqual(rc, 0, text[-3000:])
        return d, text

    def round_trip(self, name, label, offset, want_xa):
        n = self.reached[label] + offset
        saves = os.path.join(self.tmp, f'saves-{name}')
        ref_end = os.path.join(self.tmp, f'{name}-ref_end.tks')
        d_ref, text = self.run_case(
            f'{name}-ref',
            f'at {n} peek cd_audio.c:g_played 4\nat {n} peek gSDInfo 4\nat {n} qsave 4\nanchor\n'
            f'at {n + 30} peek cd_audio.c:g_played 4\nat {n + K} save {ref_end}\nafter 1 quit\n',
            saves=saves)
        self.assertIn('[QUICKSAVE] slot 4 saved', text)
        self.assertNotIn('deferred', text)
        p = peeks(text)
        played = [v for _, v in p['cd_audio.c:g_played']]
        self.assertTrue(p['gSDInfo'][0][1] & 0x100, 'a song is playing at the save')
        if want_xa:
            self.assertGreater(played[1], played[0], 'XA sectors are being decoded at the save')

        # the slot's files
        qs = Path(saves) / 'quicksave'
        info = sst.read_state(str(qs / 'slot4.tks'))['info']
        self.assertEqual(int(info['frame_counter']), n)
        png = (qs / 'slot4.png').read_bytes()
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>II', png[16:24]), (160, 120))
        ini = configparser.ConfigParser()
        ini.read(qs / 'slot4.ini')
        self.assertEqual(ini['slot']['format'], '1')
        self.assertEqual(ini['slot']['state'], 'slot4.tks')
        self.assertEqual(ini['slot']['thumbnail'], 'slot4.png')
        self.assertEqual(ini['slot']['build_id'], self.syms.build_id)
        self.assertEqual(ini['slot']['profile'], 'parity')
        self.assertEqual(int(ini['slot']['frame_counter']), n)
        self.assertEqual(ini['slot']['scene'],
                         f"{info['now_step']}/{info['now_sub_step']}/{info['now_sub_sub_step']}")

        # a new session quickloads the slot at frame 200 and runs to N+K
        load_end = os.path.join(self.tmp, f'{name}-load_end.tks')
        d_load, text = self.run_case(
            f'{name}-load', f'at 200 qload 4\nanchor\nat {n + K} save {load_end}\nafter 1 quit\n',
            saves=saves)
        self.assertIn('[QUICKSAVE] slot 4 loaded', text)
        self.assertIn('[QUICKSAVE] slot 0 saved', text)       # the undo slot
        undo = configparser.ConfigParser()
        undo.read(qs / 'slot0.ini')
        self.assertEqual(int(undo['slot']['frame_counter']), 200)
        sa = sst.read_state(ref_end)
        rep = sst.diff_states(sa, sst.read_state(load_end), self.syms)
        state = {r: sum(h - l for l, h in rs) for r, rs in rep.items()}
        names = sst.diff_names(sa, rep, syms=self.syms)
        self.assertTrue(rt.machine_same(state), names)
        self.assertEqual(rt.compare_dirs(os.path.join(d_ref, 'parity'), os.path.join(d_load, 'parity'),
                                         rt.is_windows(self.binary)), [])
        return ini, saves

    def test_mid_xa_and_music(self):
        ini, _ = self.round_trip('nameentry', 'surname_entry', 60, want_xa=True)
        self.assertEqual(ini['game']['in_progress'], '0')    # the name entry: no game yet

    def test_mid_music_in_game(self):
        ini, saves = self.round_trip('lesson', 'lesson', 50, want_xa=False)
        g = ini['game']
        self.assertEqual(g['in_progress'], '1')
        self.assertEqual((g['year'], g['month']), ('1995', '4'))
        self.assertIn(g['weekday'], ('Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'))
        self.assertEqual(int(g['week']), int(g['pass_days']) // 7 + 1)
        for stat in ('para_tai', 'para_bun', 'para_ri', 'para_gei', 'para_un', 'para_zat',
                     'para_you', 'para_kon', 'para_str'):
            int(g[stat])

        # a parity slot loaded into an enhanced session: the session stays enhanced
        d, text = self.run_case(
            'profile', 'at 199 peek profile.c:g_profile 4\nat 200 qload 4\nafter 1 peek profile.c:g_profile 4\n'
            'after 1 quit\n', extra={'TOKI_PROFILE': 'enhanced'}, saves=saves)
        self.assertIn('[QUICKSAVE] slot 4 loaded', text)
        before, after = [v for _, v in peeks(text)['profile.c:g_profile']]
        self.assertEqual(before, after, 'the loaded slot changed the session profile')

    def test_quickload_keeps_the_card(self):
        saves = os.path.join(self.tmp, 'saves-card')
        d, text = self.run_case('card-save', 'at 300 qsave 2\nafter 1 peek memcard.c:g_card+128 1\n'
                                'after 1 quit\n', saves=saves)
        self.assertIn('[QUICKSAVE] slot 2 saved', text)
        self.assertEqual(peeks(text)['memcard.c:g_card+128'][0][1], 0xA0)   # a free entry
        # the card now holds a save (directory entry 1 in use)
        card = bytearray(128 * 1024)
        card[0:2] = b'MC'
        card[127] = 0x4D ^ 0x43
        entry = bytearray(128)
        entry[0:4] = (0x51).to_bytes(4, 'little')
        entry[4:8] = (8192).to_bytes(4, 'little')
        entry[8:10] = b'\xff\xff'
        entry[10:30] = b'BISLPS-00065TEST'.ljust(20, b'\0')
        x = 0
        for b in entry[:127]:
            x ^= b
        entry[127] = x
        card[128:256] = entry
        card[8192:8192 + 8] = b'saveDATA'
        (Path(saves) / 'card1.mcr').write_bytes(bytes(card))
        d, text = self.run_case('card-load', 'at 200 peek memcard.c:g_card+128 1\nafter 0 qload 2\n'
                                'after 1 peek memcard.c:g_card+128 1\nafter 1 peek memcard.c:g_card+8192 8\n'
                                'after 1 quit\n', saves=saves)
        self.assertIn('[QUICKSAVE] slot 2 loaded', text)
        p = peeks(text)
        self.assertEqual([v for _, v in p['memcard.c:g_card+128']], [0x51, 0x51])
        self.assertEqual(p['memcard.c:g_card+8192'][0][1], int.from_bytes(b'saveDATA', 'little'))
        on_disk = (Path(saves) / 'card1.mcr').read_bytes()
        self.assertEqual(on_disk[128:256], bytes(entry))
        self.assertEqual(on_disk[8192:8200], b'saveDATA')

    def test_empty_and_bad_slot(self):
        saves = os.path.join(self.tmp, 'saves-misc')
        qs = Path(saves) / 'quicksave'
        qs.mkdir(parents=True)
        (qs / 'slot7.tks').write_bytes(b'TOKISTv1')
        (qs / 'slot8.tks').write_bytes(b'TOKISTv1')
        (qs / 'slot8.ini').write_text('[slot]\nbuild_id = 0123\nsaved = 2026-01-02 03:04:05\n'
                                      'scene = 18/1/0\n[game]\nin_progress = 0\n')
        d, text = self.run_case('misc', 'at 200 qload 6\nafter 1 qload 8\nafter 1 qload 7\n'
                                'after 1 qsave 0\nafter 1 qslot 3\nafter 1 qsave\nafter 1 qlist\n'
                                'after 1 quit\n', saves=saves)
        self.assertIn('[QUICKSAVE] Slot 6 is empty', text)
        self.assertIn('[QUICKSAVE] Slot 8: saved by another build', text)
        self.assertIn('[QUICKSAVE] Load slot 7 failed', text)
        self.assertIn('[QUICKSAVE] Slot 0 is the automatic undo slot', text)
        self.assertIn('[QUICKSAVE] slot 3 saved', text)      # qsave without N: the selected slot
        self.assertTrue((qs / 'slot3.png').is_file() and (qs / 'slot3.ini').is_file())
        listing = re.search(r'ok (Quicksave slot 3\|.*)', text).group(1).split('|')
        self.assertEqual(listing[0], 'Quicksave slot 3')
        self.assertEqual([line[:2] for line in listing[1:11]],
                         [' 1', ' 2', '>3', ' 4', ' 5', ' 6', ' 7', ' 8', ' 9', ' 0'])
        self.assertEqual(listing[1], ' 1  -')
        self.assertRegex(listing[3], r'^>3  Scene \d+/\d+/\d+  \d\d-\d\d \d\d:\d\d$')
        self.assertEqual(listing[8], ' 8  Scene 18/1/0  01-02 03:04')


    def test_only_this_installations_states_load(self):
        # a state is restored byte for byte over native memory (function
        # pointers, the game stack), so a load accepts only a state signed
        # with this installation's key (pc/hal/state_sign.c): a changed
        # byte, a missing signature, or a valid-looking signature made with
        # another key are all refused before the file is parsed
        import hashlib
        import hmac
        saves = os.path.join(self.tmp, 'saves-sign')
        qs = Path(saves) / 'quicksave'
        d, text = self.run_case('sign-save', 'at 200 qsave 2\nafter 1 quit\n', saves=saves)
        self.assertIn('[QUICKSAVE] slot 2 saved', text)
        good = (qs / 'slot2.tks').read_bytes()
        self.assertEqual(good[-40:-32], b'TKSIGv01')
        # the key itself is never in a state (state_sign.c is a KEEP file)
        key = (ASSETS / '.state.key').read_bytes()
        self.assertEqual(len(key), 32)
        self.assertNotIn(key, good)
        self.assertNotIn(key[:16], good)
        tampered = bytearray(good)
        tampered[len(good) // 2] ^= 0x01
        foreign = good[:-40] + b'TKSIGv01' + hmac.new(os.urandom(32), good[:-40], hashlib.sha256).digest()
        for slot, data in ((3, bytes(tampered)), (4, good[:-40]), (5, foreign)):
            (qs / f'slot{slot}.tks').write_bytes(data)
            shutil.copy(qs / 'slot2.ini', qs / f'slot{slot}.ini')
        d, text = self.run_case('sign-load', 'at 200 qload 3\nafter 1 qload 4\nafter 1 qload 5\n'
                                'after 1 qload 2\nafter 1 quit\n', saves=saves)
        for slot in (3, 4, 5):
            self.assertIn(f'[QUICKSAVE] Load slot {slot} failed', text)
        self.assertEqual(text.count('was not saved by this installation'), 3, text[-3000:])
        self.assertIn('[QUICKSAVE] slot 2 loaded', text)


@unittest.skipUnless(WIN_BINARY.is_file() and WINE and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the Windows build, Wine and pc/assets')
class QuicksaveRoundTripWindows(QuicksaveRoundTrip):
    """every test above on the Windows build under Wine (the portable state
    format, pc/hal/dbg_state.c; the PS1 RAM comparison leaves out the CD
    queue's uninitialised payloads, savestate_tool.cdq_noise)"""
    binary = WIN_BINARY


if __name__ == '__main__':
    unittest.main()
