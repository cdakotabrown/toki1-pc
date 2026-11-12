# SPDX-License-Identifier: GPL-3.0-or-later
"""Generated overlay views take a PS1 global's address through PS1_VIEW
(pc/include/ps1_view.h), so GCC cannot assume a view stays inside its
declared object.

TAIIKU taiiku_slot adds to D_80149BA4[i] and then get_taiiku_seg (inlined)
reads the same halfword through `D_80149BA2__v3[i + 1].lo`.  With the old
`(*(T *)&D_80149BA2)` view GCC treated the two symbols as disjoint objects
and hoisted the read above the add: the port drew rand() one frame after the
console, and whether the festival ball race took its other path depended on
the build (plain vs reach-instrumented binary).

  fix:              pc/tests/ps1_view_harness.c with PS1_VIEW reads the
                    record after the add (prints 1, the console's answer);
  negative control: the same harness with the old `&name` view at -O2 reads
                    it before the add (prints 0);
  generator:        every view and pointer-cell macro in pc/src/overlay_*.c
                    goes through PS1_VIEW, and taiiku_slot's object reads
                    the record after the add.
"""
import glob
import os
import re
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# the game objects' optimisation flags (pc/Makefile CFLAGS)
CFLAGS = ['-std=gnu17', '-w', '-funsigned-char', '-fno-strict-aliasing',
          '-fno-aggressive-loop-optimizations', '-O2', '-I', 'pc/include']
LINK = ['-no-pie', '-Wl,--defsym=D_A=g_ram+2', '-Wl,--defsym=D_B=g_ram+4']


class Ps1View(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_harness(self, *defs):
        exe = os.path.join(self.tmp.name, 'view' + ''.join(defs).replace('-', '_'))
        subprocess.run(['gcc'] + CFLAGS + list(defs) + ['pc/tests/ps1_view_harness.c', '-o', exe]
                       + LINK, cwd=ROOT, check=True)
        return subprocess.run([exe], capture_output=True, text=True, check=True).stdout.strip()

    def test_ps1_view_reads_after_the_store(self):
        self.assertEqual(self.run_harness(), '1')

    def test_plain_view_is_reordered(self):
        # negative control: the old view; GCC's base-decl disambiguation
        out = self.run_harness('-DPLAIN_VIEW')
        if out == '1':
            self.skipTest('this GCC did not reorder the old view (control not reproduced)')
        self.assertEqual(out, '0')

    def test_generated_views_use_ps1_view(self):
        bad = []
        for path in sorted(glob.glob(os.path.join(ROOT, 'pc/src/overlay_*.c'))):
            with open(path, errors='replace') as fh:
                for n, line in enumerate(fh, 1):
                    if re.match(r'#define \w+__[vsa]\d+\b', line) and re.search(r'&\s*\w+\)', line):
                        bad.append('%s:%d: %s' % (os.path.basename(path), n, line.strip()))
        self.assertEqual(bad, [])

    def test_taiiku_slot_reads_after_the_add(self):
        binary = os.path.join(ROOT, 'pc/build/tokimeki-pc')
        if not os.path.exists(binary):
            self.skipTest('build first (make -f pc/Makefile all)')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn',
                              '--disassemble=TAIIKU_taiiku_slot', binary],
                             capture_output=True, text=True, check=True).stdout
        lines = dis.splitlines()
        add = next(i for i, l in enumerate(lines) if re.search(r'\badd\s+%e\w+,-0x4\(%r\w+\)', l))
        seg = next(i for i, l in enumerate(lines) if '<D_80149BA2>' in l)
        self.assertLess(add, seg, 'the D_80149BA2 record read precedes the D_80149BA4 add')


if __name__ == '__main__':
    unittest.main()
