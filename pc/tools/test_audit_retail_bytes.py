# SPDX-License-Identifier: GPL-3.0-or-later
"""pc/tools/audit_retail_bytes.py on synthetic "discs" and "binaries" (random
bytes; no retail data): a copied run is found and measured, short runs and
fill bytes are not, the allow-list only covers what it names."""
import os
from pathlib import Path
import random
import tempfile
import unittest

import audit_retail_bytes as arb


class RetailBytesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        rnd = random.Random(7)
        raw = self.root / 'assets/raw'
        (raw / 'CDROM/EXEDIR').mkdir(parents=True)
        self.exe = bytes(rnd.getrandbits(8) for _ in range(65536))
        (raw / 'PSX.EXE').write_bytes(self.exe)
        self.exn = bytes(rnd.getrandbits(8) for _ in range(20000))
        (raw / 'CDROM/EXEDIR/X.EXN').write_bytes(self.exn)
        (raw / 'CDROM/V.XA').write_bytes(self.exe[:4096])          # media: not indexed
        self.filler = bytes(rnd.getrandbits(8) for _ in range(30000))

    def binary(self, name, *parts):
        p = self.root / name
        p.write_bytes(b''.join(parts))
        return p

    def run_audit(self, *bins, allow=None):
        return arb.run_audit([str(b) for b in bins], str(self.root / 'assets'), 64, allow, show=0)

    def test_copied_run_found_with_exact_extent(self):
        b = self.binary('a', self.filler[:1001], self.exe[5000:5300], self.filler[2000:3000])
        status, rep = self.run_audit(b)
        self.assertEqual(status, 1)
        f = rep[str(b)]
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0]['binary_offset'], f[0]['length'], f[0]['source'], f[0]['source_offset']),
                         (1001, 300, 'PSX.EXE', 5000))

    def test_overlay_file_and_unaligned(self):
        b = self.binary('b', self.filler[:7], self.exn[1233:1333])
        status, rep = self.run_audit(b)
        self.assertEqual(status, 1)
        self.assertEqual(rep[str(b)][0]['source'], 'CDROM/EXEDIR/X.EXN')

    def test_clean_binary_short_runs_and_fill(self):
        """controls: unrelated bytes, a 40-byte copy (below 64), zero and
        0xFF fill, and a copy of a media file (not indexed) are not findings"""
        b = self.binary('c', self.filler, self.exe[100:140], bytes(4096), b'\xff' * 4096,
                        self.filler[:50], self.exe[:4096][::-1])
        status, rep = self.run_audit(b)
        self.assertEqual(status, 0)
        self.assertEqual(rep[str(b)], [])

    def test_allow_list_covers_only_its_range(self):
        allow = self.root / 'allow.txt'
        allow.write_text('# test\nPSX.EXE 0x1388 300 a source literal\n')
        ok = self.binary('d', self.filler[:500], self.exe[5000:5300])
        status, _ = self.run_audit(ok, allow=str(allow))
        self.assertEqual(status, 0)
        wider = self.binary('e', self.filler[:500], self.exe[5000:5400])      # 100 bytes beyond the entry
        status, _ = self.run_audit(wider, allow=str(allow))
        self.assertEqual(status, 1)

    def test_symbol_names_outside_sections(self):
        """identifiers outside every section (a symbol table) are names, not
        data"""
        names = b'\0'.join(b'name_%04d_of_a_function' % i for i in range(8)) + b'\0'
        (self.root / 'assets/raw/CDROM/EXEDIR/O.BIN').write_bytes(self.filler[20000:20100] + names)
        b = self.binary('f', self.filler[:300], names)
        status, rep = self.run_audit(b)
        self.assertEqual(status, 0)
        self.assertIn('symbol names', rep[str(b)][0]['allowed'])

    def test_missing_assets(self):
        status, _ = arb.run_audit([os.devnull], str(self.root / 'nowhere'), 64, None, show=0)
        self.assertEqual(status, 2)


if __name__ == '__main__':
    unittest.main()
