#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_source_archive.py -- the GPL corresponding-source archive the
player packagers write next to each package (pc/tools/source_archive.py).

    python3 pc/tools/test_source_archive.py

- source_path: the archive's name follows the package's.
- A missing SPU core is refused (SPU_CORE_DIR pointed at an empty folder).
- With the core fetched (pc/3rd/spu-glue/fetch.sh): the archive holds the
  committed tree (pc/LICENSE, the packagers), the six pristine DuckStation
  files and their patched copies byte for byte, the fetch stamp and
  SOURCE.txt naming the commit and the SPU pin; nothing from outside the
  top directory; and the same commit gives the same bytes twice.
"""
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_archive as S  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CORE = S.core_dir()


class Names(unittest.TestCase):
    def test_source_path(self):
        self.assertEqual(S.source_path('build/tokimeki-pc-windows.zip').name,
                         'tokimeki-pc-windows-source.tar.gz')
        self.assertEqual(S.source_path('/x/tokimeki-pc-linux-x86_64.tar.gz'),
                         Path('/x/tokimeki-pc-linux-x86_64-source.tar.gz'))


class MissingCore(unittest.TestCase):
    def test_refused(self):
        with tempfile.TemporaryDirectory() as t:
            old = os.environ.get('SPU_CORE_DIR')
            os.environ['SPU_CORE_DIR'] = t
            try:
                with self.assertRaises(ValueError) as cm:
                    S.spu_files()
                self.assertIn('fetch.sh', str(cm.exception))
            finally:
                if old is None:
                    del os.environ['SPU_CORE_DIR']
                else:
                    os.environ['SPU_CORE_DIR'] = old


@unittest.skipUnless((CORE / '.fetched').is_file(), 'SPU core not fetched (pc/3rd/spu-glue/fetch.sh)')
class Archive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / 'build').mkdir(exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        cls.out, cls.info = S.build(Path(cls.tmp.name) / 'p-source.tar.gz', allow_dirty=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_members(self):
        top = 'p-source'
        # one tree in the monorepo; toki1-pc/, toki1-decomp/ (and toki1-english/)
        # side by side when the port is built from separate checkouts
        parts = S.parts()
        pc = top + '/' + parts[0][2]
        with tarfile.open(self.out) as tf:
            names = set(tf.getnames())
            self.assertTrue(all(n == top or n.startswith(top + '/') for n in names))
            for rel in ('pc/LICENSE', 'LICENSE', 'pc/tools/package_windows.py', 'pc/3rd/SPU-CORE.md',
                        'pc/3rd/spu-core/.fetched'):
                self.assertIn(pc + rel, names)
            self.assertIn(f'{top}/SOURCE.txt', names)
            for _name, _root, sub in parts:   # every tree the binary is built from
                self.assertIn(f'{top}/{sub}tools/repo_paths.py', names)
            self.assertIn(f'{top}/{parts[min(1, len(parts) - 1)][2]}src/main/matched/main.c', names)
            for kind in ('pristine', 'upstream'):
                for f in sorted((CORE / kind).iterdir()):
                    data = tf.extractfile(f'{pc}pc/3rd/spu-core/{kind}/{f.name}').read()
                    self.assertEqual(data, f.read_bytes(), f'{kind}/{f.name}')
            txt = tf.extractfile(f'{top}/SOURCE.txt').read().decode()
        self.assertIn(self.info['commit'], txt)
        self.assertIn(S.spu_pin(), txt)
        self.assertIn('Connor McLaughlin', txt)

    def test_deterministic(self):
        again, _ = S.build(Path(self.tmp.name) / 'q' / 'p-source.tar.gz', allow_dirty=True)
        self.assertEqual(Path(again).read_bytes(), Path(self.out).read_bytes())

    def test_refuses_existing(self):
        with self.assertRaises(FileExistsError):
            S.build(self.out, allow_dirty=True)


if __name__ == '__main__':
    unittest.main()
