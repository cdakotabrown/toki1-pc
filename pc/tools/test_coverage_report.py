#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""coverage_report.py: universe, list parsing, merging; no game assets."""
import json
import os
import tempfile
import unittest

import coverage_report as cr
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP  # noqa: E402


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as fh:
        fh.write(text)


class Fixture(unittest.TestCase):
    """A tiny repo: overlay AAA (2 functions), dead overlay EVENT (1),
    three registered main functions (two game, one SDK) and one asm_only
    SDK function the port defines under an alias."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        r = self.root = self.tmp.name
        write(os.path.join(r, 'src/overlay/AAA/aaa_main.c'), '')
        write(os.path.join(r, 'src/overlay/AAA/aaa_sub.c'), '')
        write(os.path.join(r, 'src/overlay/EVENT/ev_main.c'), '')
        write(os.path.join(r, 'src/overlay/README.md'), '')
        write(os.path.join(r, 'config/overlay/AAA.sym'),
              '# map\n0x80132000:16:aaa_main\n0x80132010:8:aaa_sub\n')
        write(os.path.join(r, 'config/level3_functions.json'), json.dumps(dict(functions=[
            dict(name='game_main', address='0x80045000'),
            dict(name='load_done_flag_set', address='0x80086954'),
            dict(name='DecDCTReset', address='0x800869A0')])))
        write(os.path.join(r, 'config/main_function_audit_by_addr.json'), json.dumps(dict(addresses={
            '0x800A6B28': dict(kind='asm_only', aliases=['func_800A6B28', 'GsTMDfastF4NL']),
            '0x80045000': dict(kind='matched_c', aliases=['game_main']),
            '0x800A0000': dict(kind='asm_only', aliases=['func_800A0000', 'game_main'])})))
        self.uni = cr.load_universe(r)


class UniverseTests(Fixture):
    def test_groups(self):
        u = self.uni
        self.assertEqual(u['AAA_aaa_main'], dict(group='overlay:AAA', name='aaa_main',
                                                 addr='0x80132000'))
        self.assertEqual(u['EVENT_ev_main']['group'], 'dead:EVENT')
        self.assertEqual(u['game_main']['group'], 'main:game')
        # the Konami tail of the library region is game code; Sony starts at DecDCTReset
        self.assertEqual(u['load_done_flag_set']['group'], 'main:game')
        self.assertEqual(u['DecDCTReset']['group'], 'main:sdk')
        self.assertEqual(u['GsTMDfastF4NL']['group'], 'main:sdk')
        self.assertEqual(u['GsTMDfastF4NL']['aliases'], ['func_800A6B28'])
        # an asm_only entry whose alias is already a registered function is not a second copy
        self.assertNotIn('func_800A0000', u)
        self.assertEqual(len(u), 7)

    def test_report_counts_and_lists(self):
        reached = {'AAA_aaa_main', 'game_main', 'func_800A6B28', 'pc_only_helper',
                   'AAA_aaa_sub.part.0'}
        built = set(self.uni) - {'DecDCTReset'} | {'func_800A6B28', 'pc_only_helper'}
        rep = cr.build_report(self.uni, reached, built)
        g = rep['groups']
        self.assertEqual((g['overlay:AAA']['reached'], g['overlay:AAA']['in_build']), (2, 2))
        self.assertEqual(g['main:game']['reached'], 1)
        self.assertEqual([r['name'] for r in g['main:game']['unreached']], ['load_done_flag_set'])
        self.assertEqual(g['main:sdk']['reached'], 1)          # via the alias
        self.assertEqual([r['name'] for r in g['main:sdk']['not_in_build']], ['DecDCTReset'])
        self.assertEqual(rep['summary']['live_total'],
                         dict(total=4, in_build=4, reached=3, percent=75.0))
        self.assertEqual(rep['summary']['dead_overlays']['reached'], 0)
        self.assertEqual(rep['unattributed'], ['pc_only_helper'])
        md = cr.markdown(rep)
        self.assertIn('| overlay:AAA | 2 | 2 | 2 | 100.0 | 0 |', md)
        self.assertIn('`load_done_flag_set`', md)

    def test_no_binary_means_everything_built(self):
        rep = cr.build_report(self.uni, set())
        self.assertEqual(rep['summary']['live_total']['in_build'], 4)


class InputTests(Fixture):
    def test_lists_dirs_hex_and_json_merge(self):
        d = os.path.join(self.root, 'lists')
        write(os.path.join(d, 'a.1.txt'), 'AAA_aaa_main\n0x401000\n\n')
        write(os.path.join(d, 'b.2.txt'), 'game_main\n0xdead\n')
        write(os.path.join(d, 'notes.log'), 'AAA_aaa_sub\n')        # not a list
        names, unres, src = cr.read_inputs([d], {0x401000: 'AAA_aaa_sub'})
        self.assertEqual(names, {'AAA_aaa_main', 'AAA_aaa_sub', 'game_main'})
        self.assertEqual(unres, {'0xdead'})
        self.assertEqual(len(src), 2)
        # a report is an input too: the union of two runs
        rep = cr.build_report(self.uni, names, None, unres, src)
        j = os.path.join(self.root, 'r.json')
        write(j, json.dumps(rep))
        write(os.path.join(self.root, 'c.txt'), 'load_done_flag_set\n')
        names2, unres2, src2 = cr.read_inputs([j, os.path.join(self.root, 'c.txt')])
        self.assertEqual(names2, names | {'load_done_flag_set'})
        self.assertEqual(unres2, {'0xdead'})
        merged = cr.build_report(self.uni, names2)
        self.assertEqual(merged['summary']['live_total']['reached'], 4)

    def test_parse_nm_prefers_global(self):
        names, addrs = cr.parse_nm('0000000000401000 t local_alias\n'
                                   '0000000000401000 T real_name\n'
                                   '0000000000402000 D some_data\n'
                                   '                 U undefined\n')
        self.assertEqual(addrs, {0x401000: 'real_name'})
        self.assertEqual(set(names), {'local_alias', 'real_name'})

    def test_clone_suffixes(self):
        for n in ('f.part.0', 'f.isra.0', 'f.cold', 'f.constprop.0.isra.0', 'f.part.0.isra.0'):
            self.assertEqual(cr.base_name(n), 'f')
        self.assertEqual(cr.base_name('f'), 'f')


class SrcmapTests(unittest.TestCase):
    def test_game_objects(self):
        with tempfile.TemporaryDirectory() as t:
            write(os.path.join(t, 'obj', '.srcmap'),
                  'a.o src/main/matched/a.c\nb.o src/main/pc-port/b.c\n'
                  'c.o pc/hal/gpu.c\nd.o pc/src/overlay_TACO.c\ne.o pc/src/main.c\n'
                  'f.o pc/src/overlay/TT/tt_anim.c\n')
            old = cr.ROOT
            cr.ROOT = '/'
            try:
                self.assertEqual(cr.game_objects(t), {'a.o', 'b.o', 'd.o'})
            finally:
                cr.ROOT = old


class RepoUniverseTests(unittest.TestCase):
    """The real tree: the live overlay universe equals the .sym maps."""

    def test_live_overlays_match_sym_maps(self):
        uni = cr.load_universe()
        for ov in ('TACO', 'GEKO', 'DATE'):
            sym = os.path.join(DECOMP, 'config/overlay/%s.sym' % ov)
            n = sum(1 for l in open(sym) if l.startswith('0x'))
            self.assertEqual(sum(1 for f in uni.values() if f['group'] == 'overlay:' + ov), n)
        self.assertFalse(any(f['group'] == 'overlay:EVENT' for f in uni.values()))


if __name__ == '__main__':
    unittest.main()
