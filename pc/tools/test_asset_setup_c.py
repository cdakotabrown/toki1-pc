# SPDX-License-Identifier: GPL-3.0-or-later
"""The C first-run extractor (pc/hal/asset_setup.c) against the Python one
(bootstrap_assets.py / disc_image.py), on synthetic discs only: never retail
data in tests.

The C side is the standalone command line (pc/tests/asset_cli_main.c),
compiled here with the host cc, so the test needs no SDL and no game build.
Both tools must write the same tree and the same assets.manifest.json, byte
for byte, and refuse the same bad inputs.  The real disc is compared by
pc/tools/fresh_install.py (case python-equal).
"""
from dataclasses import asdict, replace
import filecmp
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import bootstrap_assets as assets
from disc_image import SECTOR
from test_bootstrap_assets import fixture

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['pc/tests/asset_cli_main.c', 'pc/hal/asset_setup.c', 'pc/hal/pc_fs.c', 'pc/hal/pc_hash.c']


def build_cli(out):
    cc = shutil.which('cc') or shutil.which('gcc')
    if not cc:
        raise unittest.SkipTest('no C compiler')
    exe = Path(out) / 'toki-assets'
    subprocess.run([cc, '-std=gnu17', '-O2', '-D_GNU_SOURCE', '-I', str(ROOT / 'pc/hal')] +
                   [str(ROOT / s) for s in SOURCES] + ['-o', str(exe)], check=True)
    return exe


def tree(root):
    root = Path(root)
    return sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file())


def differences(a, b):
    """[] when the two trees hold the same files with the same bytes."""
    ta, tb = tree(a), tree(b)
    if ta != tb:
        return [f'file lists differ: {sorted(set(ta) ^ set(tb))}']
    return [n for n in ta if not filecmp.cmp(Path(a) / n, Path(b) / n, shallow=False)]


class CExtractorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.cli = build_cli(cls.build.name)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def profile_file(self, profile):
        path = self.root / 'profile.txt'
        path.write_text(''.join(f'{k}={v}\n' for k, v in asdict(profile).items()))
        return path

    def c(self, image, out, profile, *extra, audio=None):
        cmd = [str(self.cli), '--prepare-assets', str(image), '--out', str(out),
               '--test-profile', str(self.profile_file(profile)), *extra]
        if audio:
            cmd += ['--audio-track', str(audio)]
        return subprocess.run(cmd, capture_output=True, text=True)

    def c_verify(self, dest, profile, fast=False):
        cmd = [str(self.cli), '--verify-assets', str(dest), '--test-profile', str(self.profile_file(profile))]
        return subprocess.run(cmd + (['--fast'] if fast else []), capture_output=True, text=True)

    def python(self, image, out, profile, audio=None):
        return assets.prepare_assets(image, out, profile, audio_track=audio)

    def both(self, image, profile, audio=None):
        py, c = self.root / 'py' / 'assets', self.root / 'c' / 'assets'
        self.python(image, py, profile, audio)
        r = self.c(image, c, profile, audio=audio)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return py, c

    # ---- equal output

    def test_bin_tree_and_manifest_byte_identical(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        py, c = self.both(image, profile)
        self.assertEqual(differences(py, c), [])
        self.assertEqual((py / 'assets.manifest.json').read_bytes(), (c / 'assets.manifest.json').read_bytes())
        self.assertEqual(len(tree(c)), 12)        # 11 managed files + the manifest

    def test_negative_control_one_byte(self):
        """The comparison is not blind: one flipped byte is found, and the C
        full verification refuses the tree while the fast one (sizes only)
        still accepts it -- the documented trade-off of every launch."""
        image = self.root / 'disc.bin'
        profile = fixture(image)
        py, c = self.both(image, profile)
        victim = c / 'raw/CDROM/RAW.BIN'
        data = bytearray(victim.read_bytes())
        data[0] ^= 1
        os.unlink(victim)          # a hard link: replace it, leave the alias as it was
        victim.write_bytes(bytes(data))
        self.assertEqual(differences(py, c), ['raw/CDROM/RAW.BIN'])
        self.assertNotEqual(self.c_verify(c, profile).returncode, 0)
        self.assertEqual(self.c_verify(c, profile, fast=True).returncode, 0)
        with self.assertRaises(ValueError):
            assets.verify_cache(c, profile)

    def test_cue_input(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        cue = self.root / 'disc.cue'
        cue.write_text('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        py, c = self.both(cue, profile)
        self.assertEqual(differences(py, c), [])

    def cdda_fixture(self):
        """Track 1 with an external CD-DA extent, and a Track 2 BIN."""
        image = self.root / 'disc (Track 1).bin'
        profile = fixture(image, extra_name='DADIR/TR02.DA', extra_lba=64, extra_size=4096)
        audio = self.root / 'disc (Track 2).bin'
        payload = bytes((i * 7) & 0xFF for i in range((2 + 2) * SECTOR))
        audio.write_bytes(payload)
        profile = replace(profile, audio_size=len(payload), audio_sha1=hashlib.sha1(payload).hexdigest(),
                          audio_skip=2, cdda_lba=64, cdda_size=4096)
        return image, audio, profile

    def test_cdda_track2_identical(self):
        image, audio, profile = self.cdda_fixture()
        py, c = self.both(image, profile, audio=audio)
        self.assertEqual(differences(py, c), [])
        self.assertEqual((c / 'audio/TR02.DA').read_bytes(), audio.read_bytes()[2 * SECTOR:])

    def test_partner_track2_found_by_name(self):
        image, audio, profile = self.cdda_fixture()
        py, c = self.root / 'py' / 'assets', self.root / 'c' / 'assets'
        self.python(image, py, profile)          # "(Track 1)" -> "(Track 2)" sibling
        r = self.c(image, c, profile)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(differences(py, c), [])

    def test_single_file_bin_matches_split(self):
        """C extension: one BIN holding both tracks (CUE INDEX 00/01) gives
        the same tree as the split image; the Python tool refuses it."""
        image, audio, profile = self.cdda_fixture()
        py, c = self.both(image, profile, audio=audio)
        single = self.root / 'single.bin'
        single.write_bytes(image.read_bytes() + audio.read_bytes())
        sectors = image.stat().st_size // SECTOR
        msf = lambda n: f'{n // 4500:02d}:{n // 75 % 60:02d}:{n % 75:02d}'  # noqa: E731
        cue = self.root / 'single.cue'
        cue.write_text(f'FILE "single.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                       f'  TRACK 02 AUDIO\n    INDEX 00 {msf(sectors)}\n    INDEX 01 {msf(sectors + 2)}\n')
        c2 = self.root / 'c2' / 'assets'
        r = self.c(cue, c2, profile)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(differences(py, c2), [])
        with self.assertRaises(ValueError):
            self.python(cue, self.root / 'py2' / 'assets', profile)

    def test_missing_track2_refused_then_omitted(self):
        image, audio, profile = self.cdda_fixture()
        audio.unlink()
        r = self.c(image, self.root / 'c' / 'assets', profile)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Track 2', r.stdout)
        with self.assertRaises(ValueError):
            self.python(image, self.root / 'py' / 'assets', profile)
        r = self.c(image, self.root / 'c' / 'assets', profile, '--allow-no-audio')
        self.assertEqual(r.returncode, 0, r.stdout)
        c = self.root / 'c' / 'assets'
        self.assertFalse((c / 'audio/TR02.DA').exists())
        self.assertIn('"omitted_files"', (c / 'assets.manifest.json').read_text())
        self.assertEqual(self.c_verify(c, profile).returncode, 0)
        with self.assertRaises(ValueError):           # the Python tool does not accept it
            assets.verify_cache(c, profile)

    def test_c_reads_python_cache_and_reuses_it(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        py = self.root / 'py' / 'assets'
        self.python(image, py, profile)
        self.assertEqual(self.c_verify(py, profile).returncode, 0)
        before = {p: p.stat().st_mtime_ns for p in py.rglob('*')}
        r = self.c(image, py, profile)               # an existing valid cache: verified, untouched
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(before, {p: p.stat().st_mtime_ns for p in py.rglob('*')})

    # ---- the same refusals

    def assert_both_refuse(self, image, profile, needle=None, audio=None):
        with self.assertRaises((ValueError, OSError)):
            self.python(image, self.root / 'py' / 'assets', profile, audio)
        r = self.c(image, self.root / 'c' / 'assets', profile, audio=audio)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        if needle:
            self.assertIn(needle, r.stdout)
        self.assertFalse((self.root / 'c' / 'assets').exists())
        self.assertEqual([p.name for p in (self.root / 'c').iterdir()] if (self.root / 'c').exists() else [], [])

    def test_refuses_modified_image(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        data = bytearray(image.read_bytes())
        data[40 * SECTOR + 100] ^= 0xFF
        image.write_bytes(bytes(data))
        self.assert_both_refuse(image, profile, 'SHA1')

    def test_refuses_wrong_size(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        with open(image, 'ab') as fh:
            fh.write(b'\0' * SECTOR)
        self.assert_both_refuse(image, profile, 'image size')

    def test_refuses_bad_cues(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        bad = ['FILE "disc.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n',
               'FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:02:00\n',
               'FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n',
               'TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n',
               'FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
               '  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n',
               'FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:61:00\n',
               'FILE "missing.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n']
        for text in bad:
            with self.subTest(cue=text):
                cue = self.root / 'disc.cue'
                cue.write_text(text)
                self.assert_both_refuse(cue, profile)

    def test_c_refuses_cue_files_outside_its_folder(self):
        # an absolute path, a network share (Windows would send NTLM
        # credentials to stat it) or ".." in a FILE record: the program
        # refuses the .cue before it touches the named file
        image = self.root / 'disc.bin'
        profile = fixture(image)
        (self.root / 'sub').mkdir()
        for name in (str(image), '\\\\host\\share\\disc.bin', '//host/share/disc.bin',
                     'C:disc.bin', 'sub/../disc.bin', '../disc.bin'):
            with self.subTest(name=name):
                cue = self.root / 'sub' / 'disc.cue'
                cue.write_text(f'FILE "{name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
                r = self.c(cue, self.root / 'c' / 'assets', profile)
                self.assertNotEqual(r.returncode, 0, r.stdout)
                self.assertIn('outside its own folder', r.stdout)
                self.assertFalse((self.root / 'c' / 'assets').exists())

    def test_refuses_unsafe_and_colliding_names(self):
        # OPTION.EXN: a second one in CDROM/ collides with EXEDIR's in overlays/
        for name in ('CON.TXT', 'LPT1', 'A?B', 'TRAIL.', 'OPTION.EXN'):
            with self.subTest(name=name):
                image = self.root / 'disc.bin'
                profile = fixture(image, extra_name=name)
                self.assert_both_refuse(image, profile)

    def test_refuses_out_of_range_extent(self):
        image = self.root / 'disc.bin'
        profile = fixture(image, extra_lba=63, extra_size=4096)
        self.assert_both_refuse(image, profile, 'out-of-range')

    def test_refuses_existing_unmanaged_destination(self):
        image = self.root / 'disc.bin'
        profile = fixture(image)
        dest = self.root / 'c' / 'assets'
        dest.mkdir(parents=True)
        (dest / 'mine.txt').write_text('keep me')
        r = self.c(image, dest, profile)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual((dest / 'mine.txt').read_text(), 'keep me')
        self.assertEqual(tree(dest), ['mine.txt'])

    # ---- the data folder (2026-11-07): the log and en/ live in it

    def test_extracts_into_folder_holding_only_the_programs_files(self):
        """data/ already holds the open log and en/ (the setup runs after
        the log is opened there): the disc is extracted into it, those
        stay, and no lock or staging folder is left beside it."""
        image = self.root / 'disc.bin'
        profile = fixture(image)
        py = self.root / 'py' / 'assets'
        self.python(image, py, profile)
        dest = self.root / 'game' / 'data'
        (dest / 'en/raw').mkdir(parents=True)
        (dest / 'en/raw/X.BIN').write_text('english')
        (dest / 'tokimeki-pc.log').write_text('log line\n')
        r = self.c(image, dest, profile)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((dest / 'tokimeki-pc.log').read_text(), 'log line\n')
        self.assertEqual((dest / 'en/raw/X.BIN').read_text(), 'english')
        ours = [n for n in tree(dest) if not n.startswith('en/') and n != 'tokimeki-pc.log']
        self.assertEqual(ours, tree(py))
        self.assertEqual(sorted(p.name for p in dest.parent.iterdir()), ['data'])   # no lock, no stage
        self.assertEqual(self.c_verify(dest, profile).returncode, 0)

    def test_stale_lock_and_stage_do_not_block(self):
        """What a crashed setup leaves (its lock file, a staging folder)
        neither blocks the next one nor stays behind."""
        image = self.root / 'disc.bin'
        profile = fixture(image)
        dest = self.root / 'game' / 'data'
        dest.parent.mkdir(parents=True)
        (dest.parent / '.data.prepare.lock').write_text('')
        (dest.parent / '.data.stage-99999-0/assets').mkdir(parents=True)
        r = self.c(image, dest, profile)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(sorted(p.name for p in dest.parent.iterdir()), ['data'])

    def test_held_lock_blocks(self):
        """Negative control: a lock another process holds refuses the setup."""
        import fcntl
        image = self.root / 'disc.bin'
        profile = fixture(image)
        dest = self.root / 'game' / 'data'
        dest.parent.mkdir(parents=True)
        with open(dest.parent / '.data.prepare.lock', 'w') as fh:
            fcntl.flock(fh, fcntl.LOCK_EX)
            r = self.c(image, dest, profile)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('another setup', r.stdout)
        self.assertFalse(dest.exists())


if __name__ == '__main__':
    unittest.main()
