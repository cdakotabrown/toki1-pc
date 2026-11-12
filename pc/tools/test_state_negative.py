# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls of the save-state round trip (pc/hal/dbg_state.c), on
Linux and, under Wine, Windows.

A reference run saves the machine at the name entry (surname_entry+40: the
streamed XA track and a sequenced song are playing) and again K frames
later.  A fresh process loads the first state (PC_LOADSTATE) and runs to the
same frame; the two end machines and the parity artifacts recorded after
the load (mixed PCM, the displayed image, VRAM and PS1 RAM at checkpoints)
must be equal.  Then the load leaves one component out on purpose
(PC_STATE_NEGATIVE=...): the comparison must see it.

  natv                      every native record (the HAL's device models)
  pram                      PS1 RAM
  g_vram                    one record: VRAM
  spu.cpp:_ZN3SPUL5s_ramE   one record: SPU RAM (samples, CD capture)
  cd_audio.c:g_fifo         one record: the decoded XA audio queue
  stak                      the game stack's leftovers -- the one component
                            that is NOT seen: run on to the first lesson,
                            across scene loads and CD reads, the machine is
                            the same without it, on both builds.  The slots
                            game code over-reads (cd_read_start's local) are
                            rewritten by the same frame's host code before
                            they are read, so the dead window below the
                            boundary is regenerated every frame.  The test
                            asserts that (the evidence that the native stack
                            need not be saved at this boundary; STAK stays
                            in the format as a cheap safeguard).

A load that refuses or fails (natv: the overlay identity is native state,
so the load stops with "the overlay is -") counts as seen.

Needs the built port(s) and pc/assets.  Seven power-on runs per build.
"""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402

ROOT = Path(rt.ROOT)
LINUX = ROOT / 'pc/build/tokimeki-pc'
WINDOWS = ROOT / 'pc/build-win/tokimeki-pc.exe'
ASSETS = ROOT / 'pc/assets'
SCRIPT = ROOT / 'pc/tools/playthrough/new_game.txt'
WINE = shutil.which('wine')
K = 150
NEGATIVES = ('natv', 'pram', 'g_vram', 'spu.cpp:_ZN3SPUL5s_ramE', 'cd_audio.c:g_fifo', 'stak')


class Negative:
    binary = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='state-neg-', dir=ROOT / 'build')
        b = str(cls.binary)
        cls.reached, abs_input, _ = rt.resolve(str(SCRIPT), cls.tmp, b, 900)
        cls.n = cls.reached['surname_entry'] + 40
        cls.inp = os.path.join(cls.tmp, 'abs.input')
        Path(cls.inp).write_text(abs_input)
        cls.ref = os.path.join(cls.tmp, 'ref.tks')
        cls.ref_end = os.path.join(cls.tmp, 'ref_end.tks')
        cls.ref_far = os.path.join(cls.tmp, 'ref_far.tks')
        cls.far = cls.reached['lesson']
        cls.run_case('ref', f'at {cls.n} save {cls.ref}\nanchor\nat {cls.n + K} save {cls.ref_end}\n'
                            f'at {cls.far} save {cls.ref_far}\nafter 1 quit\n', {})

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def run_case(cls, tag, cmds, extra):
        d = os.path.join(cls.tmp, tag)
        os.makedirs(d, exist_ok=True)
        Path(d, 'cmds').write_text(cmds)
        env = dict(PC_INPUT=cls.inp, PC_DBG_CMDS=os.path.join(d, 'cmds'),
                   PC_PARITY_DIR=os.path.join(d, 'parity'), PC_PARITY_FRAMES=f'1,30,90,{K - 10}',
                   PC_PARITY_RAM='1')
        env.update(extra)
        rc, text = rt.run_port(str(cls.binary), os.path.join(d, 'work'), os.path.join(d, 'log'), env, 900)
        return d, rc, text

    def loaded(self, negative=None, far=False):
        """-> (machine differences, parity files that differ) of a load;
        ({'load': message}, []) when the load itself failed"""
        tag = 'load-' + (negative or 'none').replace(':', '_') + ('-far' if far else '')
        end = os.path.join(self.tmp, f'{tag}.tks')
        extra = {'PC_LOADSTATE': self.ref}
        if negative:
            extra['PC_STATE_NEGATIVE'] = negative
        at = self.far if far else self.n + K
        d, rc, text = self.run_case(tag, f'anchor\nat {at} save {end}\nafter 1 quit\n', extra)
        if rc != 0:
            self.assertIsNotNone(negative, text[-3000:])
            return {'load': [l for l in text.splitlines() if 'PC_LOADSTATE' in l][-1:]}, []
        sa = sst.read_state(self.ref_far if far else self.ref_end)
        rep = sst.diff_states(sa, sst.read_state(end))
        state = {r: sum(h - l for l, h in rs) for r, rs in rep.items() if rs}
        parity = [] if far else rt.compare_dirs(os.path.join(self.tmp, 'ref', 'parity'),
                                                os.path.join(d, 'parity'), rt.is_windows(self.binary))
        return state, parity

    def test_1_the_load_is_exact(self):
        state, parity = self.loaded()
        self.assertTrue(rt.machine_same(state), state)
        self.assertEqual(parity, [])
        # and across the scene loads to the first lesson (the stack control's span)
        state, _ = self.loaded(far=True)
        self.assertTrue(rt.machine_same(state), state)

    def test_2_each_omission_is_seen(self):
        for neg in NEGATIVES:
            with self.subTest(negative=neg):
                state, parity = self.loaded(neg, far=neg == 'stak')
                broken = not rt.machine_same(state) or parity
                if neg == 'stak':
                    # unobservable: see the module docstring
                    self.assertFalse(broken, (state, parity))
                    continue
                self.assertTrue(broken, f'{neg} left out, and nothing differs: {state} {parity}')


def runnable(binary):
    if not (ASSETS / 'raw/PSX.EXE').is_file() or not binary.is_file():
        return False
    return not str(binary).endswith('.exe') or bool(WINE)


@unittest.skipUnless(runnable(LINUX), 'needs the Linux build and pc/assets')
class NegativeLinux(Negative, unittest.TestCase):
    binary = LINUX


@unittest.skipUnless(runnable(WINDOWS), 'needs the Windows build, Wine and pc/assets')
class NegativeWindows(Negative, unittest.TestCase):
    binary = WINDOWS


if __name__ == '__main__':
    unittest.main()
