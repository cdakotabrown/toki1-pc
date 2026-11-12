# SPDX-License-Identifier: GPL-3.0-or-later
"""The PS1 register page can no longer land inside the host heap
(pc/src/ps1_regions.c, pc/tests/ps1_regions_harness.c).

main.c used to map 0x1F801000 with MAP_FIXED after the brk heap had
grown; a heap that had grown over the page lost a page of malloc'd data
to the register mirror (they aliased).  Each case runs the harness under
`setarch -R`, so the brk heap starts below the page as in the bad runs:

  fix:              pc_map_fixed_regions() refuses an occupied range
                    (MAP_FIXED_NOREPLACE) and leaves the heap data alone;
                    mapped first, the kernel refuses to grow the heap over
                    the page;
  negative control: the old MAP_FIXED mapping over the same heap succeeds
                    and the heap page's data is gone.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SETARCH = shutil.which('setarch')


@unittest.skipUnless(SETARCH, 'needs setarch')
class Ps1Regions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.tmp.name, 'regions')
        subprocess.run(['gcc', '-O1', '-no-pie', '-fno-pie', 'pc/src/ps1_regions.c',
                        'pc/tests/ps1_regions_harness.c', '-o', cls.exe],
                       cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_mode(self, mode):
        r = subprocess.run([SETARCH, os.uname().machine, '-R', self.exe, mode],
                           capture_output=True, text=True, timeout=60)
        if r.returncode == 77:
            self.skipTest(r.stdout.strip())
        return r

    def test_occupied_range_is_refused(self):
        r = self.run_mode('heap-first-new')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('map refused; heap data intact', r.stdout)
        self.assertIn('FATAL: cannot map PSX MMIO', r.stderr)
        self.assertIn('[heap]', r.stderr)          # names what is in the way

    def test_old_mapping_aliases_the_heap(self):
        # negative control: what main.c did
        r = self.run_mode('heap-first-old')
        self.assertIn('old mapping succeeded; heap data LOST', r.stdout)

    def test_mapped_first_the_heap_stops_below(self):
        r = self.run_mode('map-first')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('heap over the page refused', r.stdout)


if __name__ == '__main__':
    unittest.main()
