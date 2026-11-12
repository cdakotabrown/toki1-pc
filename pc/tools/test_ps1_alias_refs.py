# SPDX-License-Identifier: GPL-3.0-or-later
"""PS1 globals with a second C name reach game code through an opaque
address (pc/tools/gen_ps1_alias_refs.py, pc/include/game/ps1_alias_refs.h).

GCC assumes distinct declared objects are disjoint, and that an access
wider than a declared object cannot touch it.  Both were false for the
port's alias names and both miscompiled (found by
pc/tools/build_compare.py, plain vs -fno-schedule-insns2, 2026-10-31):

  NAME_ENT name_ent_record_set: the u16 read of screen_w + 248 was
      scheduled above the store to p_birthday (a u8 at that address), and
      the day's read-modify-write put the old birth month back;
  TT tt_main: `D_800F022D = tt_config_flags_b | 1` was deleted as a dead store,
      because the next statement wrote D_800F022D from tt_config_flags_u, a
      "different" object at the same address.

  fix:              pc/tests/ps1_alias_refs_harness.c through the route keeps
                    the month and both flag bits (the console's result);
  negative control: the same harness naming the symbols directly, at -O2,
                    loses both;
  generator:        the checked-in header and address file match the .ld
                    files, the port's objects reach p_birthday through the
                    route, and name_ent_record_set stores the month before
                    it reads the halfword.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_ps1_alias_refs as gen  # noqa: E402

ROOT = gen.ROOT
CFLAGS = ['-std=gnu17', '-w', '-funsigned-char', '-fno-strict-aliasing',
          '-fno-aggressive-loop-optimizations', '-O2']
LINK = ['-no-pie', '-Wl,--defsym=screen_w=g_ram', '-Wl,--defsym=p_birthday=g_ram+248',
        '-Wl,--defsym=D_800F022D=g_ram+300', '-Wl,--defsym=tt_config_flags_b=g_ram+300',
        '-Wl,--defsym=tt_config_flags_u=g_ram+300']


class Harness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_harness(self, *defs):
        exe = os.path.join(self.tmp.name, 'refs' + ''.join(defs).replace('-', '_'))
        subprocess.run(['gcc'] + CFLAGS + list(defs) +
                       ['pc/tests/ps1_alias_refs_harness.c', 'pc/tests/ps1_alias_refs_addr.c',
                        '-o', exe] + LINK, cwd=ROOT, check=True)
        return subprocess.run([exe], capture_output=True, text=True, check=True).stdout.strip()

    def test_route_keeps_month_and_flags(self):
        self.assertEqual(self.run_harness(), 'month 3 day 17 flags 3')

    def test_direct_names_are_miscompiled(self):
        out = self.run_harness('-DDIRECT')
        if out == 'month 3 day 17 flags 3':
            self.skipTest('this GCC kept the order (control not reproduced)')
        self.assertNotEqual(out, 'month 3 day 17 flags 3')


class Generated(unittest.TestCase):
    def test_files_match_the_linker_scripts(self):
        self.assertEqual(gen.main(['--check']), 0)

    def test_both_sides_of_a_pair_are_routed(self):
        laundered, kept = gen.names()
        for n in ('screen_w', 'p_birthday', 'D_800F022D', 'tt_config_flags_b', 'tt_config_flags_u'):
            self.assertIn(n, laundered)
        self.assertIn('cdq_head', kept)        # CDQ_init.c defines it
        self.assertIn('cd_state2', laundered)  # its member

    def test_no_routed_name_is_a_local_or_parameter(self):
        self.assertEqual(gen.audit_declarations(), [])

    def test_audit_sees_a_parameter(self):
        # negative control: route libsnd's `seq_no` (a gSDInfo member that
        # GetMetaEvent also uses as a parameter name) and the audit names it
        with tempfile.TemporaryDirectory() as d:
            h = os.path.join(d, 'refs.h')
            with open(h, 'w') as fh:
                fh.write('#define seq_no (*ps1_ref_seq_no())\n')
            hits = gen.audit_declarations(header=h, files=['src/main/pc-port/GetMetaEvent.c'])
        self.assertTrue(any(fn == 'ps1_ref_seq_no' for _, _, fn, _ in hits), hits)
        self.assertNotIn('seq_no', gen.names()[0])

    def test_record_set_stores_the_month_first(self):
        binary = os.path.join(ROOT, 'pc/build/tokimeki-pc')
        if not os.path.exists(binary):
            self.skipTest('build first (make -f pc/Makefile all)')
        dis = subprocess.run(['objdump', '-d', '--no-show-raw-insn',
                              '--disassemble=NAME_ENT_name_ent_record_set', binary],
                             capture_output=True, text=True, check=True).stdout.splitlines()
        month = next(i for i, l in enumerate(dis) if '<D_8014CC90>' in l)
        store = next(i for i in range(month, len(dis)) if re.search(r'\bmov\s+%\w+l,\(%r\w+\)', dis[i]))
        read = next(i for i in range(month, len(dis)) if re.search(r'movzwl 0xf8\(%r\w+\)', dis[i]))
        self.assertLess(store, read, 'the halfword at screen_w + 248 is read before p_birthday is stored')


if __name__ == '__main__':
    unittest.main()
