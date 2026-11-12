# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows path spellings reach the HAL intact (the Windows build under Wine).

A player's paths on Windows are drive paths with backslashes; the harness
under Wine usually passes "/home/..." (Wine maps it through Z:), which hid
two bugs found on 2026-11-02:

  * PC_PARITY_DIR accepted only a path starting with "/": a real Windows
    harness (C:\\...) could not capture.  Now "X:\\", "X:/" and UNC are
    absolute on Windows.  Control: a relative path is still refused.
  * the music dump created its folders by splitting on "/" only, so a
    [audio] music_dir / TOKI_MUSIC_DIR spelled "D:\\Mods\\new\\music" whose
    folders did not exist yet stayed undumped.  (Same fix in translation.c
    and gpu_texrep.c's fallback: PC_MKDIR_SPLIT.)  Control: the same run
    with the folders made beforehand dumps on any build.

Runs only with PC_TEST_PLATFORM=windows (Wine) and the built exe + assets.
"""
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from safe_process import run
import game_platform

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'pc/assets'


def win(path):
    return 'Z:' + str(path).replace('/', '\\')


@unittest.skipUnless(game_platform.WINDOWS and game_platform.game_available()
                     and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'PC_TEST_PLATFORM=windows: the Windows build under Wine')
class WindowsPaths(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(dir=ROOT / 'build', prefix='winpaths-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def boot(self, name, frames, **extra):
        work = self.tmp / name
        (work / 'pc').mkdir(parents=True)
        (work / 'pc/assets').symlink_to(ASSETS)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_TRACE=str(work / 'trace'), PC_TRACE_FRAMES=str(frames),
                   TOKI_CONFIG_DIR=str(work / 'cfg'))
        env.update(extra)
        r = run([str(game_platform.GAME), str(ASSETS)], cwd=work, env=env, timeout=600,
                max_output=64 * 1024**2)
        return r, work

    def test_parity_dir_drive_path(self):
        out = self.tmp / 'cap'
        r, _ = self.boot('drive', 120, PC_PARITY_DIR=win(out), PC_PARITY_FRAMES='10,60',
                         PC_PARITY_ANCHOR='boot')
        self.assertTrue(r.ok, r.stdout[-2000:])
        self.assertNotIn('must be an absolute path', r.stdout)
        self.assertTrue((out / 'frame_000060.ppm').is_file())
        self.assertGreater((out / 'audio.s16le').stat().st_size, 0)

    def test_control_relative_parity_dir_refused(self):
        r, _ = self.boot('relative', 60, PC_PARITY_DIR='cap', PC_PARITY_FRAMES='10')
        self.assertIn('must be an absolute path', r.stdout)

    def music_dump(self, name, music, premade):
        if premade:
            (music / 'dump').mkdir(parents=True)
        r, _ = self.boot(name, 500, TOKI_PROFILE='enhanced', TOKI_MUSIC_DUMP='1',
                         TOKI_MUSIC_DIR=win(music), SCENE_NAV='18')
        self.assertTrue(r.ok, r.stdout[-2000:])
        return sorted(p.name for p in (music / 'dump').glob('*.wav')) if (music / 'dump').exists() else []

    def test_music_dump_into_new_backslash_folders(self):
        wavs = self.music_dump('dump', self.tmp / 'Mods' / 'new folder' / 'music', premade=False)
        self.assertTrue(wavs, 'the title song was not dumped into the new folders')

    def test_control_music_dump_into_existing_folders(self):
        wavs = self.music_dump('premade', self.tmp / 'Mods2' / 'music', premade=True)
        self.assertTrue(wavs)


if __name__ == '__main__':
    unittest.main()
