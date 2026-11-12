# SPDX-License-Identifier: GPL-3.0-or-later
"""Save-state round trip inside TACO, the 3D sub-game (pc/hal/dbg_state.c),
on Linux and, under Wine, Windows.

pc/tools/scenarios/taco.txt reaches TACO's game proper (step 144, sub-step
1: GTE-transformed TMD models drawn through the HAL's sorters) from an
album card.  The scenario is prepared on the Linux build (its card, input
and command script; its own run ends early at an input wait after the
target, with or without this change, so its state is not used); each build
replays them with a save 120 frames after TACO is reached.  From there:

  reference  load the TACO state, run 60 frames, `save S`, `anchor`, run K
             frames, `save END`
  loaded     a fresh process loads S, `anchor`, runs K frames, `save END'`

END and END' must be the same machine (PS1 RAM, MMIO, every native record
-- VRAM, the GTE, the GPU, SPU RAM ...; on Windows outside the CD queue's
host noise) and the parity artifacts after the anchor (mixed PCM, the
displayed image, VRAM and PS1 RAM at checkpoints) identical.

Needs the built port(s) and pc/assets; about five minutes per build.
"""
import os
from pathlib import Path
import re
import shutil
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402
import scenario  # noqa: E402

ROOT = Path(rt.ROOT)
LINUX = ROOT / 'pc/build/tokimeki-pc'
WINDOWS = ROOT / 'pc/build-win/tokimeki-pc.exe'
ASSETS = ROOT / 'pc/assets'
WINE = shutil.which('wine')
OUT = ROOT / 'build/state-taco'
K = 240
LOADED = re.compile(r'PC_LOADSTATE \S+: frame (\d+)')


def have(binary):
    if not binary.is_file() or not (ASSETS / 'raw/PSX.EXE').is_file():
        return False
    return not str(binary).endswith('.exe') or bool(WINE)


def taco_scenario():
    """prepare taco.txt on the Linux build once -> its directory (card,
    input, commands; reaching the target is required)"""
    d = OUT / 'scenario'
    if not (d / 'taco' / 'scenario.cmds').exists():
        sc = scenario.parse(str(ROOT / 'pc/tools/scenarios/taco.txt'))
        res = scenario.run(sc, str(d), str(LINUX), 900, False)
        assert res.get('reached'), res
    return d / 'taco'


class Taco:
    binary = None

    @classmethod
    def setUpClass(cls):
        cls.d = OUT / ('windows' if rt.is_windows(cls.binary) else 'linux')
        shutil.rmtree(cls.d, ignore_errors=True)
        cls.d.mkdir(parents=True)
        src = taco_scenario()
        # the same card, input and commands; this build's own TACO state
        work = cls.d / 'scenario-work'
        saves = cls.d / 'scenario-saves'          # (run_port clears work/saves)
        saves.mkdir(parents=True)
        shutil.copy(src / 'work' / 'saves' / 'card1.mcr', saves / 'card1.mcr')
        lines = (src / 'scenario.cmds').read_text().splitlines()
        lines = lines[:[i for i, l in enumerate(lines) if 'target_reached' in l][0] + 1]
        lines += [f'after 120 save {cls.d / "taco.tks"}', 'after 1 quit']
        (cls.d / 'scenario.cmds').write_text('\n'.join(lines) + '\n')
        env = dict(PC_INPUT=str(src / 'scenario.input'), PC_DBG_CMDS=str(cls.d / 'scenario.cmds'),
                   TOKI_SAVE_DIR=str(saves))
        rc, text = cls.port('scenario', env, work=work)
        assert rc == 0 and 'ok saved' in text, text[-3000:]
        cls.taco = cls.d / 'taco.tks'
        info = sst.read_state(str(cls.taco))['info']
        assert info['now_step'] == '144' and info['now_sub_step'] != '0', info

    @classmethod
    def port(cls, tag, env, work=None):
        return rt.run_port(str(cls.binary), str(work or cls.d / f'{tag}-work'), str(cls.d / f'{tag}.log'),
                           env, 900)

    def run_case(self, tag, load, cmds):
        Path(self.d, f'{tag}.cmds').write_text(cmds)
        env = dict(PC_LOADSTATE=str(load), PC_DBG_CMDS=str(self.d / f'{tag}.cmds'),
                   PC_PARITY_DIR=str(self.d / f'{tag}-parity'), PC_PARITY_FRAMES=f'1,30,120,{K - 10}',
                   PC_PARITY_RAM='1')
        rc, text = self.port(tag, env)
        self.assertEqual(rc, 0, text[-3000:])
        return int(LOADED.search(text).group(1))

    def test_round_trip_in_the_3d_game(self):
        s, ref_end, end = self.d / 's.tks', self.d / 'ref_end.tks', self.d / 'load_end.tks'
        t = int(sst.read_state(str(self.taco))['info']['frame_counter'])
        f0 = self.run_case('ref', self.taco, f'at {t + 60} save {s}\nanchor\nafter {K} save {ref_end}\n'
                                             'after 1 quit\n')
        self.assertEqual(f0, t)
        info = sst.read_state(str(s))['info']
        self.assertEqual((info['now_step'], info['overlay']), ('144', 'TACO'))
        self.assertEqual(int(info['frame_counter']), f0 + 60)
        self.run_case('load', s, f'anchor\nafter {K} save {end}\nafter 1 quit\n')
        sa = sst.read_state(str(ref_end))
        self.assertEqual(sa['info']['now_step'], '144')
        rep = sst.diff_states(sa, sst.read_state(str(end)))
        state = {r: sum(h - l for l, h in rs) for r, rs in rep.items()}
        self.assertTrue(rt.machine_same(state), sst.diff_names(sa, rep))
        self.assertEqual(rt.compare_dirs(str(self.d / 'ref-parity'), str(self.d / 'load-parity'),
                                         rt.is_windows(self.binary)), [])


@unittest.skipUnless(have(LINUX), 'needs the Linux build and pc/assets')
class TacoLinux(Taco, unittest.TestCase):
    binary = LINUX


@unittest.skipUnless(have(LINUX) and have(WINDOWS), 'needs both builds (the scenario runs on Linux), '
                                                    'Wine and pc/assets')
class TacoWindows(Taco, unittest.TestCase):
    binary = WINDOWS


if __name__ == '__main__':
    unittest.main()
