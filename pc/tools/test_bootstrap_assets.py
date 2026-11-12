# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic fixtures only: never distribute retail data in tests."""
from dataclasses import replace
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
import zipfile
from unittest.mock import patch

import bootstrap_assets as assets
from disc_image import DiscImage, Profile, SECTOR, SYNC, resolve_track, safe_relative
from safe_process import run


def record(name, lba, size, directory=False):
    name = name.encode('ascii')
    data = bytearray(33 + len(name) + (len(name) % 2 == 0))
    data[0] = len(data)
    struct.pack_into('<I', data, 2, lba)
    struct.pack_into('>I', data, 6, lba)
    struct.pack_into('<I', data, 10, size)
    struct.pack_into('>I', data, 14, size)
    data[25] = 2 if directory else 0
    data[28:32] = b'\x01\x00\x00\x01'
    data[32] = len(name)
    data[33:33 + len(name)] = name
    return bytes(data)


def fixture(path, extra_name='RAW.BIN', extra_lba=35, extra_size=3):
    image = bytearray(SECTOR * 64)
    for number in range(64):
        start = number * SECTOR
        image[start:start + 12] = SYNC
        image[start + 15] = 2
        image[start + 16:start + 24] = bytes([1, 0, 8, 0] * 2)

    def put(lba, data):
        image[lba * SECTOR + 24:lba * SECTOR + 24 + len(data)] = data

    def directory(lba, children):
        put(lba, record('\x00', lba, 2048, True) + record('\x01', 22, 2048, True) + b''.join(children))

    exe = bytearray(0x800 + 32)
    exe[:8] = b'PS-X EXE'
    struct.pack_into('<II', exe, 0x18, 0x80041000, 32)
    exe[0x800:] = bytes(range(32))
    put(24, exe[:2048])
    put(25, exe[2048:])
    put(32, b'overlay data')
    put(33, b'nbn bytes')
    put(35, b'bin')
    for lba, submode in ((28, 0x42), (29, 0x64), (34, 0x64)):
        start = lba * SECTOR
        image[start + 16:start + 24] = bytes([1, 2, submode, 1] * 2)
        image[start + 24:start + SECTOR] = bytes([lba]) * (SECTOR - 24)
    directory(22, [record('PSX.EXE;1', 24, len(exe)), record('KONAMIC3.STR;1', 28, 4096),
                   record('VOICES.XA;1', 34, 2048), record('CDROM', 26, 2048, True)])
    directory(26, [record('EXEDIR', 27, 2048, True), record('FUKUTEST', 31, 2048, True),
                   record(extra_name + ';1', extra_lba, extra_size)])
    directory(27, [record('OPTION.EXN;1', 32, len(b'overlay data'))])
    directory(31, [record('TITLE.NBN;1', 33, len(b'nbn bytes'))])
    pvd = bytearray(2048)
    pvd[:7] = b'\x01CD001\x01'
    struct.pack_into('<H', pvd, 128, 2048)
    pvd[156:190] = record('\x00', 22, 2048, True)
    put(16, pvd)
    path.write_bytes(image)
    return Profile(len(image), hashlib.sha1(image).hexdigest(), 24, len(exe),
                   hashlib.sha1(exe).hexdigest(), 6)


def load_launcher():
    path = Path(__file__).resolve().parents[1] / 'start_game.py'
    spec = importlib.util.spec_from_file_location('start_game_test', path)
    launcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(launcher)
    return launcher


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.image = self.root / 'owned disc 日本.bin'
        self.profile = fixture(self.image)
        self.destination = self.root / 'game with spaces' / 'pc' / 'assets'

    def prepare(self, image=True):
        return assets.prepare_assets(self.image if image else None, self.destination, self.profile)

    def test_complete_raw_files_aliases_and_media_framing(self):
        report = self.prepare()
        self.assertEqual(len(report['files']), 11)
        self.assertEqual((self.destination / 'raw/CDROM/RAW.BIN').read_bytes(), b'bin')
        raw = self.image.read_bytes()
        video = b''.join(raw[lba * SECTOR + 16:(lba + 1) * SECTOR] for lba in (28, 29))
        self.assertEqual((self.destination / 'video/KONAMIC3.STR').read_bytes(), video)
        self.assertEqual(len(video), 2336 * 2)
        self.assertEqual((self.destination / 'audio/VOICES.XA').read_bytes(), raw[34 * SECTOR + 16:35 * SECTOR])
        self.assertEqual((self.destination / 'nbn/TITLE.NBN').read_bytes(), b'nbn bytes')
        self.assertEqual((self.destination / 'overlays/OPTION.EXN').read_bytes(), b'overlay data')
        self.assertEqual(assets.verify_cache(self.destination, self.profile), report)

    def test_cache_reuse_needs_no_disc_and_does_not_rewrite(self):
        first = self.prepare()
        before = {p: p.stat().st_mtime_ns for p in self.destination.rglob('*')}
        self.image.unlink()
        self.assertEqual(self.prepare(False), first)
        self.assertEqual(before, {p: p.stat().st_mtime_ns for p in self.destination.rglob('*')})

    def test_portable_copy_fallback(self):
        with patch.object(assets.os, 'link', side_effect=OSError('no hard links')):
            self.prepare()
        self.assertEqual((self.destination / 'raw/PSX.EXE').read_bytes(),
                         (self.destination / 'data/PSX.EXE').read_bytes())

    def test_wrong_image_and_executable_fail_before_publication(self):
        for profile in (replace(self.profile, track_sha1='0' * 40),
                        replace(self.profile, exe_sha1='0' * 40),
                        replace(self.profile, exe_lba=23), replace(self.profile, file_count=7)):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                assets.prepare_assets(self.image, self.destination, profile)
            self.assertFalse(self.destination.exists())

    def test_source_size_and_cooked_iso_rejected(self):
        self.image.write_bytes(b'not a raw track')
        with self.assertRaisesRegex(ValueError, 'MODE2/2352'):
            self.prepare()
        self.assertFalse(self.destination.exists())

    def test_unsafe_names_and_out_of_range_extents(self):
        for name, lba in (('../BAD', 35), ('RAW.BIN', 100000)):
            with self.subTest(name=name, lba=lba):
                self.profile = fixture(self.image, name, lba)
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.destination.exists())

    def test_safe_paths_reject_cross_platform_aliases(self):
        for name in ('/etc/passwd', '../x', 'x/../y', 'x//y', 'C:/x', 'x\\y', 'A./B', 'CON.XA', 'x\x00y', 'é.XA', 'x\x7fy'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_relative(name)

    def test_missing_or_corrupt_cache_is_not_replaced(self):
        self.prepare()
        target = self.destination / 'raw/CDROM/RAW.BIN'
        target.write_bytes(b'mod')  # Same-size corruption must be noticed.
        with self.assertRaisesRegex(ValueError, 'NOT changed'):
            self.prepare()
        self.assertEqual(target.read_bytes(), b'mod')
        (self.destination / assets.MANIFEST).unlink()
        with self.assertRaisesRegex(ValueError, 'manifest'):
            self.prepare()
        self.assertEqual(target.read_bytes(), b'mod')

    def test_missing_alias_and_forged_schema_fail(self):
        self.prepare()
        (self.destination / 'nbn/TITLE.NBN').unlink()
        with self.assertRaises(ValueError):
            self.prepare(False)
        manifest = self.destination / assets.MANIFEST
        data = json.loads(manifest.read_text())
        data['schema'] = 999
        manifest.write_text(json.dumps(data))
        with self.assertRaises(ValueError):
            self.prepare(False)

    def test_unmanaged_directory_and_symlink_are_preserved(self):
        self.destination.mkdir(parents=True)
        (self.destination / 'mine').write_text('keep')
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual((self.destination / 'mine').read_text(), 'keep')
        outside = self.root / 'outside'
        outside.mkdir()
        (self.destination / 'shortcut').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            assets.checked_path(self.destination, 'shortcut/file')

    def test_failure_cleans_only_private_staging_and_owned_lock(self):
        for method in ('extract_to_stage', 'link_or_copy'):
            with self.subTest(method=method), patch.object(assets, method, side_effect=OSError('failure')):
                with self.assertRaises(OSError):
                    self.prepare()
            self.assertFalse(self.destination.exists())
            self.assertEqual(list(self.destination.parent.iterdir()), [])

    def test_publication_failure_keeps_destination_absent(self):
        with patch.object(Path, 'rename', side_effect=OSError('rename failed')):
            with self.assertRaises(OSError):
                self.prepare()
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.destination.parent.iterdir()), [])

    def test_source_change_during_setup_rejected(self):
        with patch.object(DiscImage, 'unchanged', side_effect=ValueError('source changed')):
            with self.assertRaisesRegex(ValueError, 'source changed'):
                self.prepare()
        self.assertFalse(self.destination.exists())

    def test_existing_lock_is_never_stolen(self):
        self.destination.parent.mkdir(parents=True)
        lock = self.destination.parent / '.assets.prepare.lock'
        lock.mkdir()
        (lock / 'owner.json').write_text('someone else')
        with self.assertRaisesRegex(ValueError, 'another setup'):
            self.prepare()
        self.assertEqual((lock / 'owner.json').read_text(), 'someone else')

    def test_explicit_wrong_disc_is_not_ignored_on_cache_hit(self):
        self.prepare()
        self.image.write_bytes(b'wrong')
        with self.assertRaises(ValueError):
            self.prepare()
        assets.verify_cache(self.destination, self.profile)

    def test_first_run_requires_disc_and_cli_reports_failure(self):
        with self.assertRaisesRegex(ValueError, '--disc'):
            self.prepare(False)
        with patch('sys.stderr', new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
            assets.main(['--out', str(self.destination)])
        self.assertEqual(error.exception.code, 1)

    def test_cue_uses_its_track_not_current_working_directory(self):
        cue = self.root / 'disc.cue'
        cue.write_text(f'FILE "{self.image.name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                       'FILE "missing audio track.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n')
        self.assertEqual(resolve_track(cue), self.image)
        assets.prepare_assets(cue, self.destination, self.profile)
        cue.write_text(f'FILE "{self.image.name}" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:02:00\n')
        with self.assertRaisesRegex(ValueError, '00:00:00'):
            resolve_track(cue)

    def test_cdda_is_required_and_preserves_audio_without_pregap(self):
        base = fixture(self.image, 'RAW.DA', 65, 2048)
        audio = self.root / 'track two.bin'
        audio.write_bytes(b'P' * SECTOR + b'A' * SECTOR)
        self.profile = replace(base, audio_size=SECTOR * 2,
                               audio_sha1=hashlib.sha1(audio.read_bytes()).hexdigest(),
                               audio_skip=1, cdda_lba=65, cdda_size=2048)
        with self.assertRaisesRegex(ValueError, 'CD-DA Track 2'):
            self.prepare()
        self.assertFalse(self.destination.exists())
        cue = self.root / 'both tracks.cue'
        cue.write_text(f'FILE "{self.image.name}" BINARY\n\tTRACK\t01 MODE2/2352\nINDEX 01 00:00:00\n'
                       f'FILE "{audio.name}" BINARY\nTRACK 02 AUDIO\nINDEX 00 00:00:00\nINDEX 01 00:00:01\n')
        result = assets.prepare_assets(cue, self.destination, self.profile)
        self.assertEqual(len(result['files']), 12)
        self.assertEqual((self.destination / 'raw/CDROM/RAW.DA').read_bytes(), b'A' * SECTOR)
        self.assertEqual((self.destination / 'audio/RAW.DA').read_bytes(), b'A' * SECTOR)
        audio.write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'Track 2 size/SHA1'):
            assets.prepare_assets(self.image, self.destination, self.profile, audio_track=audio)
        assets.verify_cache(self.destination, self.profile)

    def test_sector_framing_and_unexpected_form2_rejected(self):
        for offset in (0, 35 * SECTOR + 18):
            with self.subTest(offset=offset):
                self.profile = fixture(self.image)
                raw = bytearray(self.image.read_bytes())
                raw[offset] ^= 0x20
                if offset:
                    raw[offset + 4] = raw[offset]  # Both duplicated subheaders agree.
                self.image.write_bytes(raw)
                self.profile = replace(self.profile, track_sha1=hashlib.sha1(raw).hexdigest())
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.destination.exists())

    def test_known_source_anomaly_is_reported_not_repaired(self):
        raw = bytearray(self.image.read_bytes())
        raw[29 * SECTOR + 20] ^= 1
        self.image.write_bytes(raw)
        self.profile = replace(self.profile, track_sha1=hashlib.sha1(raw).hexdigest(),
                               preserved_subheader_lba=29)
        report = self.prepare()
        self.assertEqual(report['preserved_source_anomalies'][0]['lba'], 29)
        video = (self.destination / 'video/KONAMIC3.STR').read_bytes()
        self.assertEqual(video[2336:], raw[29 * SECTOR + 16:30 * SECTOR])
        self.assertEqual(video[2336 + 4], raw[29 * SECTOR + 20])
        report.pop('preserved_source_anomalies')
        (self.destination / assets.MANIFEST).write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, 'source anomaly'):
            self.prepare(False)

    def test_disk_budget_precedes_extraction(self):
        with patch.object(assets.shutil, 'disk_usage', return_value=SimpleNamespace(free=0)), \
                patch.object(assets, 'extract_to_stage') as extract:
            with self.assertRaisesRegex(ValueError, 'free disk'):
                self.prepare()
            extract.assert_not_called()
        self.assertFalse(self.destination.exists())

    def test_manifest_wrong_json_shape_fails_clearly(self):
        self.prepare()
        (self.destination / assets.MANIFEST).write_text('[]')
        with self.assertRaisesRegex(ValueError, 'schema'):
            self.prepare(False)

    def test_launcher_is_executable_relative_and_fail_closed(self):
        launcher = load_launcher()
        game = self.destination.parents[1] / 'tokimeki-pc'
        game.parent.mkdir(parents=True)
        game.write_text('test double')
        cwd = Path.cwd()
        self.addCleanup(os.chdir, cwd)
        # main() exports TOKI_SAVE_DIR (and TOKI_SET) for the game: keep them,
        # and the save folder, inside this test
        isolated = {k: v for k, v in os.environ.items() if not k.startswith('TOKI_')}
        isolated['TOKI_CONFIG_DIR'] = str(self.root / 'config')
        env = patch.dict(os.environ, isolated, clear=True)
        env.start()
        self.addCleanup(env.stop)
        with patch.object(assets, 'SUPPORTED', self.profile), patch.object(launcher.os, 'execv') as execute:
            launcher.main(['--game', str(game), '--disc', str(self.image)])
            self.assertEqual(Path.cwd(), game.parent)
            execute.assert_called_once_with(str(game), [str(game), str(self.destination)])
        with patch.object(launcher.os, 'execv') as execute, patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                launcher.main(['--game', str(game), '--disc', str(self.image)])
            execute.assert_not_called()

    def test_legacy_cli_delegates_to_the_same_verified_engine(self):
        from extract_assets import main
        self.assertIs(main, assets.main)
        with patch.object(assets, 'SUPPORTED', self.profile), patch('sys.stdout', new_callable=io.StringIO):
            self.assertEqual(main([str(self.image), str(self.destination)]), 0)
        assets.verify_cache(self.destination, self.profile)

    def test_headless_uses_deadline_and_propagates_timeout(self):
        launcher = load_launcher()
        game = self.destination.parents[1] / 'tokimeki-pc'
        game.parent.mkdir(parents=True)
        game.write_text('test double')
        self.addCleanup(os.chdir, Path.cwd())
        for ok, reason, code in ((True, None, 0), (False, 'TIMEOUT', 124)):
            result = SimpleNamespace(ok=ok, reason=reason, returncode=0, stdout='bounded output')
            with patch.object(assets, 'SUPPORTED', self.profile), \
                    patch.object(launcher, 'run', return_value=result) as bounded, \
                    patch.dict(os.environ, {}, clear=True), \
                    patch('sys.stdout', new_callable=io.StringIO), patch('sys.stderr', new_callable=io.StringIO):
                self.assertEqual(launcher.main(['--game', str(game), '--disc', str(self.image),
                                               '--headless', '--frames', '2', '--timeout', '3']), code)
                self.assertEqual(os.environ['PC_TRACE_FRAMES'], '2')
                self.assertEqual(os.environ['SDL_VIDEODRIVER'], 'dummy')
                self.assertEqual(bounded.call_args.kwargs['timeout'], 3)
                self.assertEqual(bounded.call_args.args[0], [str(game), str(self.destination)])

    def test_default_location_distinguishes_source_and_portable_folders(self):
        launcher = load_launcher()
        with patch.object(launcher, 'HERE', self.root):
            self.assertEqual(launcher.default_game(), self.root / 'tokimeki-pc')
            (self.root / 'Makefile').write_text('source checkout')
            self.assertEqual(launcher.default_game(), self.root / 'build/tokimeki-pc')
            (self.root / 'tokimeki-pc').write_text('packaged binary')
            self.assertEqual(launcher.default_game(), self.root / 'tokimeki-pc')

    def test_overlong_native_asset_path_is_not_silently_truncated(self):
        launcher = load_launcher()
        game = self.root / 'tokimeki-pc'
        game.write_text('test double')
        result = {'files': [{'path': 'raw/' + 'x' * 512}]}
        with patch.object(launcher, 'prepare_assets', return_value=result), \
                patch.object(launcher.os, 'execv') as execute, \
                patch('sys.stdout', new_callable=io.StringIO), patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit) as error:
                launcher.main(['--game', str(game)])
            self.assertEqual(error.exception.code, 1)
            execute.assert_not_called()

    def test_single_file_launcher_is_deterministic_and_code_only(self):
        from package_launcher import build_launcher, SOURCES
        first, second = self.root / 'start_game.pyz', self.root / 'same.pyz'
        manifest = build_launcher(first)
        build_launcher(second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertFalse(manifest['game_assets_included'])
        self.assertFalse(manifest['native_executable_included'])
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(set(archive.namelist()), set(SOURCES) | {'launcher.manifest.json'})
            self.assertEqual(json.loads(archive.read('launcher.manifest.json')), manifest)
        environment = dict(os.environ)
        environment.pop('PYTHONPATH', None)
        result = run(['/usr/bin/python3', str(first), '--help'], cwd=self.root, env=environment, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('--disc', result.stdout)
        with self.assertRaises(FileExistsError):
            build_launcher(first)

    def test_launcher_packaging_never_overwrites_existing_data(self):
        from package_launcher import build_launcher
        before = self.image.read_bytes()
        with self.assertRaises(FileExistsError):
            build_launcher(self.image)
        self.assertEqual(self.image.read_bytes(), before)

    def test_shell_wrapper_help_from_unrelated_directory(self):
        root = Path(__file__).resolve().parents[2]
        result = run([str(root / 'run_game.sh'), '--help'], cwd=self.root, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('--disc', result.stdout)
        self.assertIn('--audio-track', result.stdout)
        self.assertIn('--prepare-only', result.stdout)


if __name__ == '__main__':
    unittest.main()
