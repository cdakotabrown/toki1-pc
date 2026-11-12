# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_ps1_call_args.py: calls that rely on MIPS argument registers.

  fix:              a call that passes every parameter the definition
                    reads, and a short call whose missing parameters the
                    body never reads (SHORT-UNUSED, not gated);
  negative control: the shapes matched C uses to leave an argument in
                    $a0-$a3 -- a `(void)` prototype for a one-parameter
                    definition, the overlay generator's `((s32 (*)())f)`
                    view, a K&R definition called short -- are SHORT-READ,
                    naming the parameter read.
"""
import os
import shutil
import tempfile
import unittest

import audit_ps1_call_args as ca
import audit_ps1_pointer_casts as pc
from pc_paths import DECOMP, decomp, spell

HAVE = shutil.which(pc.CLANG) is not None

DEFS = r'''
void take(s32 id) { s32 x = id; (void)x; }
void ign(s32 a, s32 b) { (void)a; }
void knr(a, b) s32 a; s32 b; { s32 c = b; (void)c; }
void vd(s32 a) { (void)a; }
'''

# another translation unit, as the matched callers are
CALLS = r'''
extern void take(void);
void knr();
void ign();
void vd();
void caller(s32 x)
{
    take();                          /* SHORT-READ: id */
    ((s32 (*)())knr)(1);             /* SHORT-READ: b */
    ign(1);                          /* SHORT-UNUSED: b is never read */
    ((void (*)(s32, s32))ign)(1, 2); /* fine */
    vd();                            /* SHORT-UNUSED: `(void)a;` is no read */
}
'''


@unittest.skipUnless(HAVE, 'needs clang')
class CallArgs(unittest.TestCase):
    def scan(self, *texts):
        defs, calls = {}, []
        with tempfile.TemporaryDirectory() as d:
            for i, text in enumerate(texts):
                path = os.path.join(d, 'f%d.c' % i)
                with open(path, 'w') as fh:
                    fh.write(text)
                df, cs, err = ca.scan_source(['gcc'] + pc.GAME_FLAGS, path)
                self.assertIsNone(err)
                defs.update(df)
                calls += cs
        return ca.judge(calls, defs.get)

    def test_shapes(self):
        rows = {(r['callee'], r['shape'], tuple(r['read'])) for r in self.scan(DEFS, CALLS)}
        self.assertEqual(rows, {('take', 'SHORT-READ', ('id',)),
                                ('knr', 'SHORT-READ', ('b',)),
                                ('ign', 'SHORT-UNUSED', ()),
                                ('vd', 'SHORT-UNUSED', ())})

    def test_void_read(self):
        """a void definition whose value a caller uses (the $v0 its last
        call left on the console) is VOID-READ; a statement call, a
        `(void)` cast and a call of a non-void definition are not"""
        defs = ('s32 other(s32 a) { return a + 1; }\n'
                'void pass(s32 a) { other(a); }\n')
        calls = ('s32 pass();\ns32 other();\n'
                 's32 user(s32 x)\n{\n'
                 '    s32 y = pass(1);\n'                 # VOID-READ (init)
                 '    pass(2);\n'
                 '    (void)pass(3);\n'
                 '    if (pass(4)) y++;\n'                # VOID-READ (cond)
                 '    y += other(5);\n'
                 '    return pass(6) + y;\n'              # VOID-READ (expr)
                 '}\n')
        rows = [r for r in self.scan(defs, calls) if r['shape'] == 'VOID-READ']
        self.assertEqual(sorted(r['line'] for r in rows), [5, 8, 10])
        self.assertEqual({r['callee'] for r in rows}, {'pass'})

    def test_naked_alias_is_not_void(self):
        """missing_funcs_gen.c's `naked void func_X(void) { jmp target }`
        aliases return the target's value: not VOID-READ (they were 232 of
        the 579 rows before 2026-11-01)"""
        defs = '__attribute__((naked)) void alias(void) { __asm__("jmp real"); }\n'
        calls = 's32 alias();\ns32 user(void) { return alias(); }\n'
        self.assertEqual([r for r in self.scan(defs, calls) if r['shape'] == 'VOID-READ'], [])

    def test_call_through_a_header_macro(self):
        """sony_bios_alias.h maps `strncpy` to `func_800B4830` by a macro:
        clang locates such a call in the header.  It is still the unit's
        call (NAME_ENT name_ent_caption_disp passed two of strncpy's three
        arguments, and the audit skipped it until 2026-11-01)"""
        with tempfile.TemporaryDirectory() as d:
            hdr = os.path.join(d, 'alias.h')
            with open(hdr, 'w') as fh:
                fh.write('#define cpy real_cpy\n')
            defs = 's32 real_cpy(u8 *d, u8 *s, s32 n) { return d[0] + s[0] + n; }\n'
            calls = ('#include "%s"\nextern s32 cpy();\n'
                     'void user(u8 *a, u8 *b) { cpy(a, b, 32); cpy(a, b); }\n' % hdr)
            rows = self.scan(defs, calls)
        self.assertEqual([(r['callee'], r['shape'], r['read']) for r in rows],
                         [('real_cpy', 'SHORT-READ', ['n'])])

    def test_call_by_address_name(self):
        """An overlay source may call another function of its overlay by
        its address name (`func_80137CCC()` for taco_xa_sector_set); the
        PC build links that to overlay_symtab.c's trampoline, so no
        definition carries the name.  A one-function source with an
        `@SymbolStart: 0xADDR` window answers to `func_ADDR` (the --source
        path), and overlay_entries() maps (overlay, ADDR) to the prefixed
        definition (the full audit).  Unjudged until 2026-11-06: TACO's
        OPERATIONS narration index went in $a0 only."""
        rows = self.scan('/* @SymbolStart: 0x80137CCC */\n'
                         'void taco_xa(u8 idx) { s32 x = idx; (void)x; }\n',
                         'extern void func_80137CCC(void);\n'
                         'void user(void) { func_80137CCC(); }\n')
        self.assertEqual([(r['callee'], r['shape'], r['read']) for r in rows],
                         [('func_80137CCC', 'SHORT-READ', ['idx'])])
        with tempfile.TemporaryDirectory() as d:
            tab = os.path.join(d, 'symtab.c')
            with open(tab, 'w') as fh:
                fh.write('    { 0x80137CCCu, (void *)TACO_taco_xa_sector_set },\n'
                         '    { 0x80137CCCu, (void *)EN_NICHI_other },\n')
            self.assertEqual(ca.overlay_entries(tab),
                             {('TACO', 0x80137CCC): 'TACO_taco_xa_sector_set',
                              ('EN_NICHI', 0x80137CCC): 'EN_NICHI_other'})

    def test_full_calls_are_clean(self):
        self.assertEqual(self.scan('void f(s32 a, s32 b) { (void)(a + b); }\n'
                                   'void g(void) { f(1, 2); }\n'), [])


# PC overrides that pass what the console passes: (caller as matched, the
# override, the callee's definition, the callee).  The matched caller is
# SHORT-READ (the negative control); the override is clean.
FIXES = [
    ('src/overlay/TACO/taco_actor_ramp.c',
     'pc/src/overlay/TACO/taco_actor_ramp.c',
     'src/overlay/TACO/taco_actor_exit.c', 'taco_actor_exit'),
    ('src/overlay/TACO/taco_actor_step5.c',
     'pc/src/overlay/TACO/taco_actor_step5.c',
     'src/overlay/TACO/taco_actor_exit.c', 'taco_actor_exit'),
    ('src/overlay/TACO/taco_actor_step12.c',
     'pc/src/overlay/TACO/taco_actor_step12.c',
     'src/overlay/TACO/taco_actor_exit.c', 'taco_actor_exit'),
    ('src/overlay/TACO/taco_actor_step15.c',
     'pc/src/overlay/TACO/taco_actor_step15.c',
     'src/overlay/TACO/taco_actor_exit.c', 'taco_actor_exit'),
    ('src/main/matched/voice_off.c',
     'src/main/pc-port/voice_off.c',
     'src/main/matched/SsVoKeyOff.c', 'SsVoKeyOff'),
    ('src/main/matched/voice_restart.c',
     'src/main/pc-port/voice_restart.c',
     'src/main/matched/SsVoKeyOff.c', 'SsVoKeyOff'),
    ('src/main/matched/SsUtGetProgAtr.c',
     'src/main/pc-port/SsUtGetProgAtr.c',
     'src/main/matched/SpuVmVSetUp.c', 'SpuVmVSetUp'),
    ('src/main/matched/ending_main.c',
     'src/main/pc-port/ending_main.c',
     'src/main/matched/block_checksum_check.c', 'block_checksum_check'),
    ('src/main/matched/near_click_check.c',
     'src/main/pc-port/near_click_check.c',
     'src/main/matched/set_XAsector.c', 'set_XAsector'),
    ('src/overlay/KANGEI/kangei_girl_set.c',
     'pc/src/overlay/KANGEI/kangei_girl_set.c',
     'src/main/matched/date_name_set.c', 'date_name_set'),
    ('src/overlay/SHOUGATU/shougatu_part8_init.c',
     'pc/src/overlay/SHOUGATU/shougatu_part8_init.c',
     'src/main/matched/date_name_set.c', 'date_name_set'),
    ('src/overlay/GEKO/geko_return_schedule2.c',
     'pc/src/overlay/GEKO/geko_return_schedule2.c',
     'src/main/matched/event_log_add.c', 'event_log_add'),
]
# the decomp files live in toki1-decomp (pc_paths.spell)
FIXES = [(spell(m), spell(o), spell(d), c) for m, o, d, c in FIXES]


@unittest.skipUnless(HAVE, 'needs clang')
class Overrides(unittest.TestCase):
    def short(self, caller, callee_def, callee):
        defs, calls = {}, []
        for path in (callee_def, caller):
            d, cs, err = ca.scan_source(['gcc'] + pc.GAME_FLAGS, path)
            self.assertIsNone(err, path)
            defs.update(d)
            calls += cs
        return [r for r in ca.judge(calls, defs.get)
                if r['shape'] == 'SHORT-READ' and r['callee'] == callee]

    def test_overrides(self):
        for matched, override, callee_def, callee in FIXES:
            with self.subTest(override=override):
                self.assertTrue(self.short(matched, callee_def, callee))
                self.assertEqual(self.short(override, callee_def, callee), [])


if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(HAVE, 'needs clang')
class VoidRead(unittest.TestCase):
    """return_plan.json: GsSortObject4 stores TmdTG3's result as the packet
    cursor.  Negative control: the matched (void) TmdTG3 is VOID-READ; the
    plan-edited copy returns the builder's cursor and is clean."""

    def rows(self, tmd_src):
        defs, calls = {}, []
        for path in (tmd_src, 'src/main/pc-port/GsSortObject4.c'):
            d, cs, err = ca.scan_source(['gcc'] + pc.GAME_FLAGS, path)
            self.assertIsNone(err, path)
            defs.update(d)
            calls += cs if 'GsSortObject4' in path else []
        return [r for r in ca.judge(calls, defs.get)
                if r['shape'] == 'VOID-READ' and r['callee'] == 'TmdTG3']

    def test_tmd_dispatcher(self):
        import ps1_source_edits as pe
        self.assertEqual(len(self.rows(decomp('src/main/matched/TmdTG3.c'))), 2)
        entry = dict(pe.load_plans())['return_plan.json']['functions']['main/TmdTG3']
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'TmdTG3.c')
            with open(path, 'w') as fh:
                fh.write(pe.apply_entry(open(os.path.join(DECOMP, 'src/main/matched/TmdTG3.c')).read(), entry))
            self.assertEqual(self.rows(path), [])
