# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_pc_stubs: body shapes, classes, and the gate on a real build."""
import os
import subprocess
import tempfile
import unittest

import audit_pc_stubs as audit
from pc_paths import disc_input


def insns(*lines, reloc=None):
    out = [(l, None) for l in lines]
    if reloc:
        out[-1] = (out[-1][0], reloc)
    return out


class BodyShapeTests(unittest.TestCase):
    def test_return_zero_is_const(self):
        self.assertEqual(audit.classify_body(insns('endbr64', 'xor    %eax,%eax', 'ret',
                                                   'nopw   0x0(%rax,%rax,1)')),
                         ('const', '0'))

    def test_empty_and_constant(self):
        self.assertEqual(audit.classify_body(insns('endbr64', 'ret')), ('empty', ''))
        self.assertEqual(audit.classify_body(insns('mov    $0x1,%eax', 'ret')),
                         ('const', '0x1'))

    def test_passthrough_is_trivial(self):
        kind, _ = audit.classify_body(insns('endbr64', 'mov    %rcx,%rax', 'ret'))
        self.assertEqual(kind, 'passthru')

    def test_tail_jump_is_forwarder(self):
        # naked `jmp target` (objdump shows the reloc on the next line) + ud2
        body = [('endbr64', None), ('jmp    9 <get_CD_State2+0x9>', 'get_CD_State2'),
                ('ud2', None)]
        self.assertEqual(audit.classify_body(body), ('forward', 'get_CD_State2'))

    def test_real_body(self):
        body = insns('endbr64', 'sub    $0x28,%rsp', 'mov    0x0(%rip),%rax',
                     'call   0 <x>', 'add    $0x28,%rsp', 'ret')
        self.assertEqual(audit.classify_body(body)[0], 'real')

    def test_objdump_parse_attaches_relocs(self):
        text = ('0000000000000000 <a>:\n'
                '   0:\tendbr64\n'
                '   4:\tjmp    9 <a+0x9>\n'
                '\t\t\t5: R_X86_64_PLT32\tb-0x4\n'
                '   9:\tud2\n')
        self.assertEqual(audit.parse_objdump(text)['a'][1], ('jmp    9 <a+0x9>', 'b'))


def row(**kw):
    base = dict(wins=True, reached=True, body='const', status='c',
                body_names=['func_x'], dead_overlay_refs=[])
    base.update(kw)
    return base


class ClassTests(unittest.TestCase):
    def test_stub_over_aliased_body(self):
        self.assertEqual(audit.classify(row()), 'STUB-HIDES-MATCHED')

    def test_stub_without_c(self):
        self.assertEqual(audit.classify(row(status='asm_only', body_names=[])),
                         'STUB-NO-MATCH')
        # an address inside another window is an alt-entry, never its body
        self.assertEqual(audit.classify(row(status='interior', body_names=[])),
                         'STUB-NO-MATCH')

    def test_override_forwarder_pcimpl(self):
        self.assertEqual(audit.classify(row(body='real')), 'OVERRIDE')
        self.assertEqual(audit.classify(row(body='forward')), 'FORWARDER')
        self.assertEqual(audit.classify(row(body='real', status=None, body_names=[])),
                         'PC-IMPL')

    def test_linked_same_name_body(self):
        self.assertEqual(audit.classify(row(body='forward', shadows_linked=True)),
                         'SHADOWS-MATCHED')
        # a trivial stub over it is still the stub class
        self.assertEqual(audit.classify(row(shadows_linked=True)), 'STUB-HIDES-MATCHED')

    def test_link_loser_and_dead(self):
        self.assertEqual(audit.classify(row(wins=False)), 'SHADOWED')
        self.assertEqual(audit.classify(row(reached=False)), 'DEAD')
        self.assertEqual(audit.classify(row(reached=False, dead_overlay_refs=['overlay_EVENT.o'])),
                         'DEAD-OVERLAY')

    def test_gate_needs_game_reach(self):
        rows = [dict(row(), cls='STUB-HIDES-MATCHED', reached_by_game=False),
                dict(row(), cls='STUB-HIDES-MATCHED', reached_by_game=True),
                dict(row(), cls='SHADOWS-MATCHED', reached_by_game=True),
                dict(row(), cls='OVERRIDE', reached_by_game=True)]
        self.assertEqual(len(audit.gating(rows)), 2)

    def test_data_as_function(self):
        # game code loads/stores the name: gated even though the body is
        # an empty stub with no matched C (step_counter_u, 2026-10-31)
        r = row(status=None, body_names=[], body='empty',
                data_access=['title_fade_out.o:rw'])
        self.assertEqual(audit.classify(r), 'DATA-AS-FUNC')
        self.assertEqual(len(audit.gating([dict(r, cls='DATA-AS-FUNC',
                                                reached_by_game=True)])), 1)
        # the name spells a PS1 data address and something reaches it
        self.assertEqual(audit.classify(row(status=None, body_names=[], body='empty',
                                            data_name='0x800EF864=step_counter')),
                         'DATA-AS-FUNC')
        # negative control: a plain stub with neither is still a stub
        self.assertEqual(audit.classify(row(status=None, body_names=[], body='empty',
                                            data_access=[], data_name=None)),
                         'STUB-NO-MATCH')

    def test_tiers(self):
        self.assertEqual(audit.tier_of('pc/src/missing_funcs.c'), 'pcsrc')
        self.assertEqual(audit.tier_of('pc/src/overlay_DATE.c'), 'overlay')
        self.assertEqual(audit.tier_of('pc/src/overlay_symtab.c'), 'pcsrc')
        self.assertEqual(audit.tier_of('src/main/pc-port/foo.c'), 'pcport')


GAME_READER = """
extern unsigned int step_counter_u;
extern unsigned char now_icon_b;
extern int tbl_800EF6E8[];
int fade(int i) {
    now_icon_b = 0;
    return step_counter_u >= 128 ? tbl_800EF6E8[i] : 0;
}
"""
GAME_CALLER = """
extern void step_counter_u(void);
extern void now_icon_b(void);
extern void tbl_800EF6E8(void);
void (*hook)(void);
void step(void) {
    step_counter_u();
    hook = now_icon_b;
    tbl_800EF6E8();
}
"""


class DataAccessTests(unittest.TestCase):
    """data_accesses() on objects compiled the way pc/Makefile compiles the
    game: a load/store is a data access, a call / tail jump / function
    pointer is not (negative control)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        for name, src in (('reader', GAME_READER), ('caller', GAME_CALLER)):
            c = os.path.join(cls.tmp.name, name + '.c')
            with open(c, 'w') as fh:
                fh.write(src)
            subprocess.run(['gcc', '-O2', '-fno-pie', '-c', c, '-o',
                            os.path.join(cls.tmp.name, name + '.o')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    NAMES = {'step_counter_u', 'now_icon_b', 'tbl_800EF6E8'}

    def test_loads_and_stores_are_data_accesses(self):
        acc = audit.data_accesses(self.tmp.name, {'reader.o': set(self.NAMES)})
        self.assertEqual(acc['step_counter_u'], [('reader.o', 'rw')])
        self.assertEqual(acc['now_icon_b'], [('reader.o', 'rw')])
        self.assertEqual(acc['tbl_800EF6E8'], [('reader.o', 'index')])

    def test_calls_and_function_pointers_are_not(self):
        acc = audit.data_accesses(self.tmp.name, {'caller.o': set(self.NAMES)})
        self.assertEqual(dict(acc), {})


BUILD = os.path.join(audit.ROOT, 'pc/build/obj/.srcmap')


@unittest.skipUnless(os.path.exists(BUILD) and
                     os.path.exists(disc_input('disks/SLPS_000.65')),
                     'needs a PC build (make -f pc/Makefile all) and the retail executable')
class BuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = {r['name']: r for r in audit.audit('pc/build')}

    def test_head_entry_forwarder_is_not_a_stub(self):
        # cal_bio_disp (0x80068F6C) forwards to the matched body
        # system_moji_set.  (week_day_bg_load, the original case, now
        # defines its body under its own name: 2026-11-10.)
        r = self.rows['cal_bio_disp']
        self.assertEqual(r['cls'], 'FORWARDER')
        self.assertIn('system_moji_set', r['detail'])

    def test_step_counter_views_are_ps1_data(self):
        # title_fade_out's step_counter_u is step_counter in PS1 RAM, not
        # a PC function (the DATA-AS-FUNC fix): no PC-side definition
        for name in ('step_counter_u', 'pad_trig_w', 'now_icon_b'):
            self.assertNotIn(name, self.rows)
        nm = subprocess.run(['nm', os.path.join(audit.ROOT, 'pc/build/tokimeki-pc')],
                            capture_output=True, text=True).stdout
        got = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines())
               if len(p) == 3}
        self.assertEqual(got.get('step_counter_u'), 0x800EF864)
        self.assertEqual(got.get('pad_trig_w'), 0x800EF6E8)
        self.assertEqual(got.get('now_icon_b'), 0x800EF7F3)

    def test_no_stub_hides_matched_code_reached_by_the_game(self):
        bad = audit.gating(list(self.rows.values()))
        self.assertEqual([r['name'] for r in bad], [])


if __name__ == '__main__':
    unittest.main()
