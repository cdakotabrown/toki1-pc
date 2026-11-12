# SPDX-License-Identifier: GPL-3.0-or-later
"""Player saves live outside the build directory (pc/hal/save_paths.c).

* The launcher (pc/start_game.py) resolves the save folder -- TOKI_SAVE_DIR,
  else [system] save_dir (--set, then settings.ini), else <data dir>/saves --
  passes it to the game as TOKI_SAVE_DIR, and copies an older
  <game dir>/saves/card1.mcr there once (never moving or deleting it).
* The HAL puts the card where TOKI_SAVE_DIR says and, with nothing set,
  exactly where it always did (cwd-relative saves/card1.mcr), which is what
  every harness run, playthrough and the battery see.

The binary half needs the built port and pc/assets; it is skipped otherwise.
"""
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from safe_process import run
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'
CARD = bytes([0x4D, 0x43]) + bytes(128 * 1024 - 2)   # "MC" header, a stand-in image


def load_launcher():
    spec = importlib.util.spec_from_file_location('start_game_saves', ROOT / 'pc/start_game.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LauncherSaveDir(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cfg = self.root / 'cfg'
        self.game_dir = self.root / 'game'
        self.game_dir.mkdir()
        self.game = self.game_dir / 'tokimeki-pc'
        self.game.write_text('test double')
        env = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'XDG_'))}
        env['TOKI_CONFIG_DIR'] = str(self.cfg)   # data dir == config dir
        patcher = patch.dict(os.environ, env, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(os.chdir, Path.cwd())
        self.launcher = load_launcher()

    def test_resolution_order(self):
        L = self.launcher
        self.assertEqual(L.save_dir(self.game_dir), self.cfg / 'saves')
        ini = self.root / 'settings.ini'
        ini.write_text('[display]\nsave_dir = /wrong\n[system]\n'
                       'save_dir = "/abs/saves"   # comment\n')
        self.assertEqual(L.save_dir(self.game_dir, ini), Path('/abs/saves'))
        ini.write_text('[system]\nsave_dir = rel/saves ; comment\n')
        self.assertEqual(L.save_dir(self.game_dir, ini), self.game_dir / 'rel/saves')
        ini.write_text('[system]\nsave_dir = ~/toki\n')
        self.assertEqual(L.save_dir(self.game_dir, ini), Path.home() / 'toki')
        ini.write_text('[system]\nsave_dir = ""\n')
        self.assertEqual(L.save_dir(self.game_dir, ini), self.cfg / 'saves')
        self.assertEqual(L.save_dir(self.game_dir, ini, ['system.save_dir=/from/set']),
                         Path('/from/set'))
        os.environ['TOKI_SAVE_DIR'] = str(self.root / 'env')
        self.assertEqual(L.save_dir(self.game_dir, ini, ['system.save_dir=/from/set']),
                         self.root / 'env')

    def test_migration_copies_once_and_never_overwrites(self):
        L = self.launcher
        old, new = self.game_dir / 'saves', self.root / 'deep/new/saves'
        self.assertIsNone(L.migrate_saves(old, new))          # nothing to copy
        self.assertFalse(new.exists())
        old.mkdir()
        (old / 'card1.mcr').write_bytes(CARD)
        self.assertEqual(L.migrate_saves(old, new), new / 'card1.mcr')
        self.assertEqual((new / 'card1.mcr').read_bytes(), CARD)
        self.assertEqual((old / 'card1.mcr').read_bytes(), CARD)   # left in place
        self.assertEqual(sorted(p.name for p in new.iterdir()), ['card1.mcr'])
        (new / 'card1.mcr').write_bytes(b'newer')
        self.assertIsNone(L.migrate_saves(old, new))          # the new card wins
        self.assertEqual((new / 'card1.mcr').read_bytes(), b'newer')
        self.assertIsNone(L.migrate_saves(old, old))          # same folder

    def launch(self, *extra):
        out = io.StringIO()
        with patch.object(self.launcher, 'prepare_assets', return_value={'files': []}), \
                patch.object(self.launcher.os, 'execv') as execv, \
                patch('sys.stdout', out), patch('sys.stderr', new_callable=io.StringIO):
            self.launcher.main(['--game', str(self.game), *extra])
        return out.getvalue(), execv

    def test_launch_passes_the_folder_and_migrates(self):
        old = self.game_dir / 'saves'
        old.mkdir()
        (old / 'card1.mcr').write_bytes(CARD)
        out, execv = self.launch()
        execv.assert_called_once()
        new = self.cfg / 'saves'
        self.assertEqual(os.environ['TOKI_SAVE_DIR'], str(new))
        self.assertEqual((new / 'card1.mcr').read_bytes(), CARD)
        self.assertTrue((old / 'card1.mcr').is_file())
        self.assertIn(f'Saves: {new}', out)
        self.assertIn('Copied your memory card', out)
        self.assertNotIn('deletes them too', out)
        # a second launch copies nothing
        (new / 'card1.mcr').write_bytes(b'played since')
        out, _ = self.launch()
        self.assertNotIn('Copied', out)
        self.assertEqual((new / 'card1.mcr').read_bytes(), b'played since')

    def test_where_reports_without_copying(self):
        old = self.game_dir / 'saves'
        old.mkdir()
        (old / 'card1.mcr').write_bytes(CARD)
        out, execv = self.launch('--where')
        execv.assert_not_called()
        self.assertIn(f'Saves:       {self.cfg / "saves"}', out)
        self.assertIn(f'Old saves:   {old}', out)
        self.assertIn('copied there on the next launch', out)
        self.assertFalse((self.cfg / 'saves').exists())
        self.assertNotIn('TOKI_SAVE_DIR', os.environ)

    def test_headless_keeps_the_historical_folder(self):
        with patch.object(self.launcher, 'run') as bounded:
            bounded.return_value = type('R', (), dict(ok=True, reason=None, returncode=0,
                                                       stdout=''))()
            self.launch('--headless', '--frames', '2')
        self.assertNotIn('TOKI_SAVE_DIR', os.environ)


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class HalSaveDir(unittest.TestCase):
    def boot(self, extra_env):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(__import__('shutil').rmtree, tmp, True)
        work = tmp / 'work'
        (work / 'pc').mkdir(parents=True)
        (work / 'pc/assets').symlink_to(ASSETS)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_TRACE_FRAMES='60', TOKI_CONFIG_DIR=str(tmp / 'cfg'))
        env.update({k: v.format(tmp=tmp) for k, v in extra_env.items()})
        result = run([str(BINARY), str(work / 'pc/assets')], cwd=work, env=env,
                     timeout=120, max_output=64 * 1024**2)
        return tmp, work, result.stdout

    def test_unset_is_the_historical_cwd_saves(self):
        tmp, work, out = self.boot({})
        self.assertTrue((work / 'saves/card1.mcr').is_file(), out[-2000:])
        self.assertEqual((work / 'saves/card1.mcr').stat().st_size, 128 * 1024)
        self.assertTrue((work / 'saves/seed').is_dir())
        self.assertNotIn('[SAVES]', out)

    def test_toki_save_dir_moves_the_card(self):
        tmp, work, out = self.boot({'TOKI_SAVE_DIR': '{tmp}/elsewhere/saves'})
        card = tmp / 'elsewhere/saves/card1.mcr'
        self.assertTrue(card.is_file(), out[-2000:])
        self.assertEqual(card.stat().st_size, 128 * 1024)
        self.assertTrue((tmp / 'elsewhere/saves/seed').is_dir())
        self.assertFalse((work / 'saves').exists())
        self.assertIn(f'[SAVES] save directory {tmp}/elsewhere/saves (TOKI_SAVE_DIR)', out)

    def test_settings_are_ignored_in_harness_runs(self):
        # [system] save_dir is a player setting: an automated run never reads it
        cfg_dir = Path(tempfile.mkdtemp())
        self.addCleanup(__import__('shutil').rmtree, cfg_dir, True)
        (cfg_dir / 'settings.ini').write_text(f'[system]\nsave_dir = {cfg_dir}/hostile\n')
        tmp, work, out = self.boot({'TOKI_SETTINGS': str(cfg_dir / 'settings.ini')})
        self.assertTrue((work / 'saves/card1.mcr').is_file(), out[-2000:])
        self.assertFalse((cfg_dir / 'hostile').exists())


@unittest.skipUnless(game_platform.WINDOWS and game_platform.game_available()
                     and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'PC_TEST_PLATFORM=windows: the Windows build under Wine')
class WindowsHomeSaveDir(unittest.TestCase):
    """[system] save_dir = "~\\TokiSaves" on Windows: there is no HOME, so
    "~" is %USERPROFILE% (pc/hal/save_paths.c).  The unfixed build made a
    folder literally named "~" in the working folder.  Run as a player (the
    setting is ignored in harness runs) with HOME removed from the
    environment, in a private prefix kept until the files are checked.
    Control: "~TokiSaves" (no separator after the tilde) is a plain relative
    folder name and is not expanded.  (The pre-fix exe fails the first test:
    its card lands in work\\~\\TokiSaves.)"""

    def boot(self, save_dir):
        tmp = Path(tempfile.mkdtemp(dir=ROOT / 'build'))
        self.addCleanup(__import__('shutil').rmtree, tmp, True)
        work = tmp / 'work'
        (work / 'pc').mkdir(parents=True)
        (work / 'pc/assets').symlink_to(ASSETS)
        (tmp / 'settings.ini').write_text(f'[system]\nsave_dir = "{save_dir}"\n')
        prefix = game_platform.new_prefix()
        self.addCleanup(game_platform.stop_prefix, prefix)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'TOKI_', 'SCENE', 'AUTOPLAY', 'HOME'))}
        # no PC_TURBO / PC_TRACE: either makes it a harness run, which ignores
        # [system] save_dir (pc_settings_harness_reason)
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   TOKI_TEST_AS_PLAYER='1', TOKI_TEST_QUIT_FRAMES='120',
                   TOKI_CONFIG_DIR=str(tmp / 'cfg'), TOKI_SETTINGS=str(tmp / 'settings.ini'),
                   WINEPREFIX=str(prefix), WINEDEBUG='-all',
                   WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree=d;mshtml=d')
        result = run(['wine', str(BINARY), str(work / 'pc/assets')], cwd=work, env=env,
                     timeout=180, max_output=64 * 1024**2)
        game_platform.stop_prefix(prefix, remove=False)
        return tmp, work, prefix, result.stdout

    def test_tilde_is_the_user_profile(self):
        tmp, work, prefix, out = self.boot('~\\TokiSaves')
        cards = list((prefix / 'drive_c/users').glob('*/TokiSaves/card1.mcr'))
        self.assertEqual(len(cards), 1, out[-3000:])
        self.assertRegex(out, r'\[SAVES\] save directory C:\\users\\[^\\]+\\TokiSaves '
                              r'\(settings system\.save_dir\)')
        self.assertFalse((work / '~').exists())

    def test_control_tilde_without_separator_is_literal(self):
        tmp, work, prefix, out = self.boot('~TokiSaves')
        self.assertTrue((work / '~TokiSaves/card1.mcr').is_file(), out[-3000:])
        self.assertFalse(list((prefix / 'drive_c/users').glob('*/TokiSaves')))


if __name__ == '__main__':
    unittest.main()
