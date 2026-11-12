# SPDX-License-Identifier: GPL-3.0-or-later
"""The first-run plumbing inside the game binary (pc/build/tokimeki-pc), fast
and without game data: the embedded licences, the data-root decision
(portable vs per-user, pc/include/pc_data_root.h), the log location, and the
extractor's command line wired into the game executable.  The full flows
(setup screen, extraction from the real disc, boot, playthrough, every
error screen) are pc/tools/fresh_install.py.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import bootstrap_assets as assets
from test_asset_setup_c import differences
from test_bootstrap_assets import fixture
import game_platform  # noqa: E402
from pc_paths import DECOMP  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
EXE = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine


# Windows keeps per-user data in %APPDATA%, here inside the run's private
# Wine prefix, which is gone when the run ends: fresh_install.py's readonly
# and audit cases check those paths on Windows instead.
PER_USER_ON_WINE = 'per-user data dir: covered on Windows by fresh_install.py readonly/audit'


def clean_env(home, **extra):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'TOKI_', 'SDL_', 'XDG_', 'SCENE', 'AUTOPLAY'))}
    env.update(HOME=str(home), SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    env.update(extra)
    return env


def files_under(path):
    return sorted(str(p.relative_to(path)) for p in Path(path).rglob('*'))


@unittest.skipUnless(EXE.is_file(), 'build the port first (make -f pc/Makefile all)')
class FirstRunTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.game = self.root / 'game'
        self.home = self.root / 'home'
        self.cwd = self.root / 'cwd'
        for d in (self.game, self.home, self.cwd):
            d.mkdir()
        shutil.copy(EXE, self.game / game_platform.GAME_NAME)

    def run_exe(self, *args, **env):
        # a Windows exe runs under Wine in a private prefix (game_platform.wrap)
        cmd, environ, cleanup = game_platform.wrap(
            [str(self.game / game_platform.GAME_NAME), *args], clean_env(self.home, **env))
        try:
            return subprocess.run(cmd, cwd=self.cwd, env=environ, capture_output=True,
                                  text=True, errors='replace', timeout=120)
        finally:
            cleanup()

    def test_licences_embedded(self):
        # the download has no licence files: every text is in the program,
        # the project's own MIT LICENSE too (toki1-decomp's: pc/hal/licenses.c)
        out = self.run_exe('--licenses').stdout
        for f in sorted((ROOT / 'pc/licenses').glob('*.txt')) + [DECOMP / 'LICENSE']:
            with self.subTest(f=f.name):
                self.assertIn(f.read_text(encoding='utf-8').strip(), out)
        # control: the player README is not a licence and is not in there
        self.assertNotIn('WHAT THE FOLDER NEEDS', out)

    def test_portable_check_logs_beside_the_exe(self):
        r = self.run_exe('--check', TOKI_PORTABLE='1')
        self.assertEqual(r.returncode, 1)              # no game data yet
        log = (self.game / 'data/tokimeki-pc.log').read_text()
        self.assertIn(f'data folder: {game_platform.host_path(self.game)} '
                      '(portable; TOKI_PORTABLE=1)', log)
        self.assertIn('no game data yet', log)
        self.assertEqual(files_under(self.home), [])
        self.assertEqual(files_under(self.cwd), [])

    @unittest.skipUnless(game_platform.WINDOWS, 'the MAX_PATH warning is Windows-only')
    def test_long_data_folder_is_warned_about(self):
        deep = self.root / ('long-folder-name-' * 9) / 'game'          # > 150 characters
        deep.mkdir(parents=True)
        shutil.copy(EXE, deep / game_platform.GAME_NAME)
        cmd, environ, cleanup = game_platform.wrap(
            [str(deep / game_platform.GAME_NAME), '--check'], clean_env(self.home, TOKI_PORTABLE='1'))
        try:
            subprocess.run(cmd, cwd=self.cwd, env=environ, capture_output=True, timeout=120)
        finally:
            cleanup()
        log = (deep / 'data/tokimeki-pc.log').read_text()
        self.assertIn('WARNING: the data folder path is', log)
        # control: the short folder of the other tests is not warned about
        self.run_exe('--check', TOKI_PORTABLE='1')
        self.assertNotIn('WARNING: the data folder path', (self.game / 'data/tokimeki-pc.log').read_text())

    @unittest.skipIf(game_platform.WINDOWS, PER_USER_ON_WINE)
    def test_per_user_check_logs_in_the_data_dir(self):
        """Negative control of the above: not portable -> ~/.local/share."""
        r = self.run_exe('--check', TOKI_PORTABLE='0')
        self.assertEqual(r.returncode, 1)
        self.assertTrue((self.home / '.local/share/tokimeki-pc/data/tokimeki-pc.log').is_file())
        self.assertEqual(files_under(self.game), [game_platform.GAME_NAME])

    @unittest.skipIf(game_platform.WINDOWS, PER_USER_ON_WINE)
    def test_markers(self):
        (self.game / 'portable.txt').write_text('')
        self.run_exe('--check')
        self.assertIn('(portable; portable.txt)', (self.game / 'data/tokimeki-pc.log').read_text())
        (self.game / 'data/tokimeki-pc.log').unlink()
        (self.game / 'user-data.txt').write_text('')     # wins over portable.txt
        self.run_exe('--check')
        self.assertFalse((self.game / 'data/tokimeki-pc.log').exists())
        self.assertIn('user-data.txt', (self.home / '.local/share/tokimeki-pc/data/tokimeki-pc.log').read_text())

    @unittest.skipIf(game_platform.WINDOWS, PER_USER_ON_WINE)
    def test_read_only_folder_falls_back(self):
        os.chmod(self.game, 0o555)
        self.addCleanup(os.chmod, self.game, 0o755)
        self.run_exe('--check', TOKI_PORTABLE='1')
        log = (self.home / '.local/share/tokimeki-pc/data/tokimeki-pc.log').read_text()
        self.assertIn('not writable', log)

    def test_old_layout_moves_to_data(self):
        """An install of the old layout (assets/, assets-en/ with a left-over
        .prepare.lock, window.ini, an empty hints.ini, the log in the
        folder, a settings.ini with the player's own lines) becomes data/,
        data/en/ and [state] of settings.ini on the next start, without
        extracting anything; the player's lines stay byte for byte."""
        g = self.game
        (g / 'assets/raw').mkdir(parents=True)
        (g / 'assets/assets.manifest.json').write_text('{"schema": 1}')    # fails the check: no boot
        (g / 'assets/raw/F.BIN').write_text('ja')
        (g / 'assets-en/raw').mkdir(parents=True)
        (g / 'assets-en/raw/F.BIN').write_text('en')
        (g / 'assets-en/.prepare.lock').write_text('')
        (g / 'saves').mkdir()
        (g / 'saves/card1.mcr').write_text('card')
        (g / 'hints.ini').write_text('')
        (g / 'window.ini').write_text('[window]\nx = 100\ny = 80\nwidth = 1280\nheight = 960\nmaximized = 0\n')
        mine = '# MY OWN COMMENT: keep me\nprofile = "enhanced"\n\n[display]\nscale = integer   # mine too\n'
        (g / 'settings.ini').write_text(mine)
        (g / 'tokimeki-pc.log').write_text('the old log\n')
        # a player's run (no automation variable) that stops on the setup
        # screen: nobody to ask without a window
        self.run_exe(TOKI_PORTABLE='1', TOKI_TEST_AS_PLAYER='1')
        self.assertEqual(sorted(p.name for p in g.iterdir()),
                         sorted([game_platform.GAME_NAME, 'data', 'saves', 'settings.ini']))
        self.assertEqual((g / 'data/raw/F.BIN').read_text(), 'ja')
        self.assertEqual((g / 'data/en/raw/F.BIN').read_text(), 'en')
        self.assertFalse((g / 'data/en/.prepare.lock').exists())
        self.assertEqual((g / 'saves/card1.mcr').read_text(), 'card')
        self.assertEqual((g / 'data/tokimeki-pc.log.old').read_text(), 'the old log\n')
        log = (g / 'data/tokimeki-pc.log').read_text()
        self.assertIn('[DATA] moved', log)
        self.assertIn('old state file(s) folded', log)
        ini = (g / 'settings.ini').read_text()
        self.assertTrue(ini.startswith(mine), ini)
        self.assertIn('[state]\nwindow_x = 100\nwindow_y = 80\nwindow_width = 1280\n', ini)
        self.assertNotIn('controls_hint', ini)        # the empty hints.ini said nothing
        # a second start changes nothing more
        self.run_exe(TOKI_PORTABLE='1', TOKI_TEST_AS_PLAYER='1')
        self.assertEqual((g / 'settings.ini').read_text(), ini)
        self.assertNotIn('[DATA] moved', (g / 'data/tokimeki-pc.log').read_text())

    @unittest.skipIf(game_platform.WINDOWS, 'chmod: the POSIX fallback path')
    def test_old_layout_that_cannot_move_stays_in_use(self):
        """Negative control: when assets/ cannot be renamed (here: the
        folder is read-only for a moment) the old folders stay the game
        data and nothing is lost."""
        cfg = self.root / 'cfg'
        (cfg / 'assets').mkdir(parents=True)
        (cfg / 'assets/assets.manifest.json').write_text('{"schema": 1}')
        os.chmod(cfg, 0o555)
        try:
            r = self.run_exe('--check', TOKI_CONFIG_DIR=str(cfg), TOKI_LOG_OFF='1')
        finally:
            os.chmod(cfg, 0o755)
        self.assertIn('cannot move', r.stdout)
        self.assertTrue((cfg / 'assets/assets.manifest.json').exists())
        self.assertIn('game data in ' + game_platform.host_path(cfg / 'assets'), r.stdout)

    def test_game_binary_prepare_assets_matches_python(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        prof = self.root / 'profile.txt'
        prof.write_text(''.join(f'{k}={v}\n' for k, v in profile.__dict__.items()))
        c = self.root / 'c' / 'assets'
        r = self.run_exe('--prepare-assets', str(image), '--out', str(c), '--test-profile', str(prof))
        self.assertEqual(r.returncode, 0, r.stdout[-2000:])
        py = self.root / 'py' / 'assets'
        assets.prepare_assets(image, py, profile)
        self.assertEqual(differences(py, c), [])


if __name__ == '__main__':
    unittest.main()
