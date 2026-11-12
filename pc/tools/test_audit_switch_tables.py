# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_switch_tables.py: C case labels against the retail jump tables.

Byte-perfect matched functions whose jump table is retail .rodata never had
their case labels compared; the PC compiles the labels.  The audit compiles
each switch with the PS1 compiler and compares its table with retail's
word for word; pc/tools/switch_labels.json carries the relabel the PC
build applies (generate_overlay_modules.py), checked by re-compiling.

  fix:              the relabeled C compiles to retail's tables;
  negative control: label swaps of the matched C (TEL tel 6<->9,
                    olh_main10 16<->32, rensyu outer 4<->5).
  Fixed in the matched C (2026-11-06): BUNKASAI, TEL, OLH, RENSYU and
  every other function now carry retail's labels; their tests check the
  matched C agrees.
"""
import os
import shutil
import tempfile
import unittest

import audit_switch_tables as sw
from pc_paths import DECOMP, decomp

IDO = (DECOMP / 'tools/ido-static-recomp/build/5.2/out/cc').exists()
HAVE = IDO and sw.have_inputs('TEL') and sw.have_inputs('BUNKASAI') and sw.have_inputs('OLH')


def measure(key):
    ov, fn = key.split('/')
    src = decomp('src/overlay/%s/%s.c' % (ov, fn))
    plan = sw.load_plan()['functions'].get(key)
    return sw.measure((key, 'ov', (ov, fn), src, src, plan))


def mutated(key, entry):
    """(path, text): the matched C of `key` with the relabel `entry` applied,
    written to a temporary file (a wrong-label negative control)."""
    ov, fn = key.split('/')
    text = sw.apply_relabel((DECOMP / ('src/overlay/%s/%s.c' % (ov, fn))).read_text(), entry)
    fd, path = tempfile.mkstemp(suffix='.c', prefix=fn + '_')
    with os.fdopen(fd, 'w') as f:
        f.write(text)
    return path, text


def swap(a, b, switch=0):
    return {'switches': [{'switch': switch, 'relabel': {str(a): [b], str(b): [a]}}]}


def table_of(res, jr=None):
    return [s for s in res if jr is None or s['jr'] == jr]


class Relabel(unittest.TestCase):
    """apply_relabel on synthetic sources (no inputs needed)"""
    SRC = ('void f(int x) {\n'
           '    switch (x) {\n'
           '    case 0: a(); break;\n'
           '    case 1: case 2: b(); break;\n'
           '    case 3: c(); break;\n'
           '    }\n'
           '}\n')

    def test_permutes_and_merges(self):
        out = sw.apply_relabel(self.SRC, {'switches': [
            {'switch': 0, 'relabel': {'0': [3], '1': [], '2': [0, 1], '3': [2]}}]})
        self.assertIn('case 3: a();', out)
        self.assertIn('case 0: case 1: b();', out)
        self.assertIn('case 2: c();', out)
        self.assertEqual(out.count('case'), 4)

    def test_span_value_at_end(self):
        out = sw.apply_relabel(self.SRC, {'switches': [
            {'switch': 0, 'relabel': {'3': [], '@end': [3]}}]})
        self.assertIn('case 3: break; }', out)

    def test_stale_plan_raises(self):
        with self.assertRaises(ValueError):
            sw.apply_relabel(self.SRC, {'switches': [{'switch': 0, 'relabel': {'7': [1]}}]})
        with self.assertRaises(ValueError):
            sw.relabel_switches('X/f', self.SRC, {'functions': {'X/f': {'switches': [
                {'switch': 1, 'relabel': {'0': [1]}}]}}})

    def test_nested_switch_labels_are_their_own(self):
        src = ('void f(int x, int y) { switch (x) { case 0: switch (y) { case 5: g(); }'
               ' break; case 1: h(); } }')
        s = sw.switches(src)
        self.assertEqual([v for v, _ in s[0]['labels']], [0, 1])
        self.assertEqual([v for v, _ in s[1]['labels']], [5])


@unittest.skipUnless(HAVE, 'needs the IDO 5.2 recomp, the EXNs (pc/assets) and disks/SLPS_000.65')
class RetailTables(unittest.TestCase):
    def check(self, key, slots):
        r = measure(key)
        self.assertEqual(r['status'], 'measured')
        bad = {s['bias'] + i for s in r['matched'] if s['status'] == 'disagree'
               for i in s['slots']}
        # negative control: the matched C's own table disagrees with retail
        self.assertTrue(slots <= bad, (key, sorted(bad)))
        # fix: the relabeled C compiles to the same code and retail's table
        self.assertNotIn('relabeled_error', r)
        self.assertTrue(r['relabeled'])
        self.assertEqual({s['status'] for s in r['relabeled']} - {'agree'}, set(), key)

    def agrees(self, key):
        """the matched C's own table equals retail's"""
        r = measure(key)
        self.assertEqual(r['status'], 'measured')
        self.assertEqual({s['status'] for s in r['matched']}, {'agree'}, key)
        return r

    def check_swapped(self, key, a, b, slots, switch=0):
        """negative control + fix on a label swap of the (correct) matched
        C: the swapped C disagrees in `slots`; swapping back (as a plan
        entry) compiles to the same code and retail's table"""
        ov, fn = key.split('/')
        path, _ = mutated(key, swap(a, b, switch))
        try:
            r = sw.measure((key, 'ov', (ov, fn), path, path, swap(a, b, switch)))
        finally:
            os.unlink(path)
        self.assertEqual(r['status'], 'measured')
        bad = {s['bias'] + i for s in r['matched'] if s['status'] == 'disagree'
               for i in s['slots']}
        self.assertEqual(bad, slots, key)
        self.assertNotIn('relabeled_error', r)
        self.assertEqual({s['status'] for s in r['relabeled']} - {'agree'}, set(), key)

    def test_tel(self):
        """the phone call: tel case 6 runs tel_dialog6 (0x801361E0) in
        retail (the matched C said tel_dialog9 until 2026-11-06); the
        label swap 6<->9 is the negative control"""
        r = self.agrees('TEL/tel_dialog_dispatch')
        s = r['matched'][0]
        self.assertEqual(s['retail_table'], '0x8013F3D8')
        self.assertEqual(sw.first_call(sw.overlay_word_fn('TEL'), 0x80134B0C + s['r'][6 - s['bias']])[0],
                         0x801361E0)
        self.check_swapped('TEL/tel_dialog_dispatch', 6, 9, {6, 9})

    def test_tel_main(self):
        self.agrees('TEL/tel_rusuban_basyo_miss_main')
        self.agrees('TEL/tel_rusuban_date_forget_main')

    def check_fixed(self, key):
        """the matched C itself carries retail's labels: its own tables
        agree with retail and no PC relabel is recorded for it"""
        r = measure(key)
        self.assertEqual(r['status'], 'measured')
        self.assertTrue(r['matched'])
        self.assertEqual({s['status'] for s in r['matched']}, {'agree'}, key)
        self.assertNotIn(key, sw.load_plan()['functions'])

    def test_bunkasai_event10(self):
        """step 9 is serifu_run in retail and the nested switch's case 0
        sets now_serifu = 10 (the C once had every label from 9 up one too
        low and broke out of case 0); the matched C is now relabeled"""
        self.check_fixed('BUNKASAI/bunkasai_girl0_club0_95_96_scene')

    def test_olh_sparse_table(self):
        """olh_main10: retail dispatches 16/32/../96 (the C labelled 2..7
        until 2026-11-06); swapping 16 and 32 moves both slots"""
        self.agrees('OLH/olh_sys_page_main')
        self.check_swapped('OLH/olh_sys_page_main', 16, 32, {16, 32})

    def test_override_is_compared_exactly(self):
        """an override that compiles to the matched code is audited
        exactly.  RENSYU rensyu_bustup_read was such an override until its
        matched C took retail's labels (2026-11-06: retail's outer slot 4
        enters a body laid out after the nested switch); here the correct
        C overrides a swapped (4<->5) copy of itself"""
        key = 'RENSYU/rensyu_bustup_read'
        src = decomp('src/overlay/RENSYU/rensyu_bustup_read.c')
        self.agrees(key)
        path, text = mutated(key, swap(4, 5))
        try:
            r = sw.measure((key, 'ov', ('RENSYU', 'rensyu_bustup_read'), src, path, None))
        finally:
            os.unlink(path)
        self.assertIn('override', r)
        self.assertEqual({s['status'] for s in r['override']}, {'agree'})
        self.assertEqual(sw.verdict(r, None, False)[0], 'override-ok')
        # negative control: the swapped C disagrees, and the first-call
        # heuristic names case 4
        start, _ = sw._window_overlay('RENSYU', 'rensyu_bustup_read', text)
        rows = sw.heuristic_rows(text, r['matched'], sw.overlay_word_fn('RENSYU'), start, 'RENSYU')
        self.assertIn(4, {x['case'] for x in rows})
        self.assertEqual(sw.heuristic_rows((sw.ROOT / src).read_text(), r['matched'],
                                           sw.overlay_word_fn('RENSYU'), start, 'RENSYU'), [])

    def test_generator_applies_the_plan(self):
        """generate_overlay_modules.py relabels the matched C it reads"""
        text = (DECOMP / 'src/overlay/TEL/tel_dialog_dispatch.c').read_text()
        # TEL needs no entry: its matched C carries retail's labels
        self.assertEqual(sw.relabel_switches('TEL/tel_dialog_dispatch', text), text)
        out = sw.relabel_switches('TEL/tel_dialog_dispatch', text, {'functions': {'TEL/tel_dialog_dispatch': swap(6, 9)}})
        self.assertNotEqual(out, text)
        self.assertRegex(out, r'case 6:\s*tel_dialog9\(\)')
        self.assertRegex(text, r'case 6:\s*tel_dialog6\(\)')

    def test_main_binary_tables_agree(self):
        """main-binary switches are audited too (scene_entry_set's 129-slot
        table among them) and all agree with retail"""
        reg = [e for e in sw.main_sources().values() if e[0]['name'] == 'scene_entry_set']
        self.assertTrue(reg)
        e, mp, pp = reg[0]
        r = sw.measure(('main/scene_entry_set', 'main', e, mp, mp, None))
        self.assertEqual(r['status'], 'measured')
        self.assertTrue(any(s['n'] == 129 for s in r['matched']))
        self.assertEqual({s['status'] for s in r['matched']}, {'agree'})


class PlanStaysEmpty(unittest.TestCase):
    """Since 2026-11-06 every matched C switch carries retail's case labels
    (audit_switch_tables --matched --strict: 0 disagreeing slots), so the PC
    relabel plan is empty and must stay so: a new entry would mean a matched
    C with the wrong labels, and the fix belongs in the C."""

    def test_switch_labels_plan_is_empty(self):
        self.assertEqual(sw.load_plan()['functions'], {})


if __name__ == '__main__':
    unittest.main()
