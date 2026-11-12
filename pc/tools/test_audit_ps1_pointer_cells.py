# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_ps1_pointer_cells.py: PS1 pointer tables walked through a host `T **`.

  positive fixtures: the three 2026-11-01 defects in their pre-fix shapes
                     -- ENDING ending_serifu_cmd's `s32 ***tbl` fed an s32
                     PS1 address through the generator's `((s32 (*)())f)`
                     view, DATE2 date2_serifu_prefix_set's `pp = (u8
                     **)(unsigned long)..; p = *pp;`, TACO taco_fan_disp's
                     `Prim **out` fed taco_burst_disp's `s32 buf[40]` --
                     are PS1-CELL / PS1-CELL / NARROW-CELL, as is a
                     PS1-resident pointer array reached through a local;
  negative controls: the fixed spellings (4-byte u32 cells, zero-extended
                     where used), host pointer arrays, `&local`, and a
                     direct explicit cast (pointer_casts' CELL-READ, not
                     reported twice) are not;
  the tree:          every record_width_plan.json entry of this class,
                     applied to the matched source, leaves its function
                     clean, and the matched source itself is flagged (the
                     pre-fix source of all five fixes, from the matched
                     files the generator reads).
"""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_ps1_pointer_cells as cells  # noqa: E402
import audit_ps1_pointer_casts as apc  # noqa: E402
import ps1_source_edits as pe  # noqa: E402
from pc_paths import spell  # noqa: E402

HAVE = shutil.which(apc.CLANG) is not None
ROOT = Path(cells.ROOT)

ENDING = r'''
extern s32 sss_so;
s32 serifu_cmd();
#define serifu_cmd__c80132B14 ((s32 (*)())serifu_cmd)
void serifu_main(void) { serifu_cmd__c80132B14(0, sss_so, 1, 2); }
s32 serifu_cmd(sel, tbl, row, col)
s32 sel; s32 ***tbl; u16 row; u16 col;
{
    s32 *ent;
    if (tbl[row][col][0] != 0) {
        ent = tbl[row][col];
        return ent[1];
    }
    return sel;
}
'''

ENDING_FIXED = r'''
extern s32 sss_so;
s32 serifu_cmd();
#define serifu_cmd__c80132B14 ((s32 (*)())serifu_cmd)
void serifu_main(void) { serifu_cmd__c80132B14(0, sss_so, 1, 2); }
s32 serifu_cmd(sel, tbl, row, col)
s32 sel; s32 tbl; u16 row; u16 col;
{
    s32 *ent;
    ent = ((s32 *)(unsigned long)((u32 *)(unsigned long)((u32 *)(unsigned long)(u32)tbl)[row])[col]);
    return ent[0] ? ent[1] : sel;
}
'''

DATE2 = r'''
extern u8 *ss_serifu;
extern s16 now_serifu;
void prefix_set(u8 c)
{
    u8 *base; u8 **pp; u8 *p;
    base = ss_serifu;
    pp = ((u8 **)(unsigned long)(*(u32 *)(base + now_serifu * 4 + 4)));
    p = *pp;
    p[0] = c;
}
'''

DATE2_FIXED = DATE2.replace('p = *pp;', 'p = (u8 *)(unsigned long)*(u32 *)pp;')

TACO = r'''
typedef struct { u32 tag; u8 r0, g0, b0, code; s16 x0, y0; } Prim;
extern Prim *prim_alloc(s32, s32);
s32 fan();
#define fan__c80153A70 ((s32 (*)())fan)
void burst(void) { s32 buf[40]; s32 unused; fan__c80153A70(buf, &unused); }
s32 fan(out, unused)
Prim **out; s32 *unused;
{
    Prim **q; s32 i;
    q = out;
    for (i = 0; i < 40; i++) {
        *q = prim_alloc(28, 0);
        (*q)->x0 = i;
        q++;
    }
    return 0;
}
'''

TACO_FIXED = TACO.replace('Prim **out;', 'u32 *out;').replace('Prim **q;', 'u32 *q;') \
    .replace('*q = prim_alloc(28, 0);', '*q = (u32)(unsigned long)prim_alloc(28, 0);') \
    .replace('(*q)->x0 = i;', '((Prim *)(unsigned long)*q)->x0 = i;')

PS1_ARRAY = r'''
extern u8 *labels[];
extern void show(u8 *);
void menu(void)
{
    u8 **p; s32 y;
    p = labels;
    for (y = 0; y < 16; y++) { show(*p); p++; }
}
'''

HOST = r'''
extern void show(u8 *);
extern u8 *ps1_tbl[];
static void walk(u8 **v, s32 n) { s32 i; for (i = 0; i < n; i++) show(v[i]); }
void host(void)
{
    u8 *arr[4]; u8 *one; u8 **pp;
    arr[0] = arr[1] = arr[2] = arr[3] = 0;
    walk(arr, 4);              /* a host array of host pointers */
    pp = &one;                 /* a local's address */
    *pp = 0;
    show(*(u8 **)ps1_tbl);     /* an explicit cast: pointer_casts' CELL-READ */
}
'''


@unittest.skipUnless(HAVE, 'needs clang')
class PointerCells(unittest.TestCase):
    def rows(self, *texts):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, t in enumerate(texts):
                p = os.path.join(d, 'f%d.c' % i)
                with open(p, 'w') as fh:
                    fh.write(t)
                paths.append(p)
            rows, errors = cells.audit_sources(paths)
        self.assertEqual(errors, [])
        return rows

    def shapes(self, rows, func):
        return sorted({r['shape'] for r in rows if r['func'] == func})

    def test_ending_integer_into_pointer_chain(self):
        rows = self.rows(ENDING)
        self.assertEqual(self.shapes(rows, 'serifu_cmd'), ['PS1-CELL'])
        # both levels of the chain, on both lines
        self.assertGreaterEqual(len([r for r in rows if r['func'] == 'serifu_cmd']), 4)
        self.assertIn("'int' into", rows[0]['origin'])
        self.assertEqual(self.rows(ENDING_FIXED), [])

    def test_date2_cast_then_dereference(self):
        rows = self.rows(DATE2)
        self.assertEqual(self.shapes(rows, 'prefix_set'), ['PS1-CELL'])
        self.assertEqual(self.rows(DATE2_FIXED), [])

    def test_taco_stack_buffer_of_cells(self):
        rows = self.rows(TACO)
        self.assertEqual(self.shapes(rows, 'fan'), ['NARROW-CELL'])
        self.assertIn("'int *' into", rows[0]['origin'])
        self.assertEqual(self.rows(TACO_FIXED), [])

    def test_ps1_array_through_a_local(self):
        self.assertEqual(self.shapes(self.rows(PS1_ARRAY), 'menu'), ['PS1-CELL'])

    def test_calls_resolve_across_units(self):
        """the caller in another translation unit, as the matched files are"""
        head, callee = TACO.split('s32 fan(out, unused)')
        caller = 's32 fan();\nvoid burst(void) { s32 buf[40]; ((s32 (*)())fan)(buf, buf); }\n'
        definition = head.split('s32 fan();')[0] + 's32 fan(out, unused)' + callee
        self.assertEqual(self.shapes(self.rows(caller, definition), 'fan'), ['NARROW-CELL'])
        # without the caller nothing names the parameter's origin
        self.assertEqual(self.shapes(self.rows(definition), 'fan'), [])

    def test_host_memory_is_not_reported(self):
        self.assertEqual(self.rows(HOST), [])


# the record_width_plan.json entries of this class: (key, matched files,
# function the rows name)
FIXES = [
    ('ENDING/ending_serifu_cmd', ['src/overlay/ENDING/ending_serifu_cmd.c',
                                  'src/overlay/ENDING/ending_serifu_main.c'], 'ending_serifu_cmd'),
    ('DATE2/date2_serifu_prefix_set', ['src/overlay/DATE2/date2_serifu_prefix_set.c'],
     'date2_serifu_prefix_set'),
    ('TACO/taco_fan_disp', ['src/overlay/TACO/taco_fan_disp.c',
                            'src/overlay/TACO/taco_burst_disp.c'], 'taco_fan_disp'),
    ('ETC/etc_menu_dispatch', ['src/overlay/ETC/etc_menu_dispatch.c'], 'etc_menu_dispatch'),
    ('OPTION/option_menu1_main1', ['src/overlay/OPTION/option_menu1_main1.c'],
     'option_menu1_main1'),
]


@unittest.skipUnless(HAVE, 'needs clang')
class TreeFixes(unittest.TestCase):
    def audit(self, files, edit=None):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, f in enumerate(files):
                text = (ROOT / spell(f)).read_text(errors='replace')
                if i == 0 and edit is not None:
                    text = pe.apply_entry(text, edit)
                p = os.path.join(d, os.path.basename(f))
                with open(p, 'w') as fh:
                    fh.write(text)
                paths.append(p)
            # the matched files include "types.h" (include/), as the
            # generator's module does
            rows, errors = cells.audit_sources(paths, ['gcc'] + apc.GAME_FLAGS)
        self.assertEqual(errors, [])
        return rows

    def test_pre_fix_sources_are_flagged_and_plans_clean_them(self):
        plans = dict(pe.load_plans())['record_width_plan.json']['functions']
        for key, files, func in FIXES:
            with self.subTest(key):
                self.assertIn(key, plans)
                before = [r for r in self.audit(files) if r['func'] == func]
                self.assertTrue(before, '%s: the matched source is not flagged' % key)
                after = [r for r in self.audit(files, plans[key]) if r['func'] == func]
                self.assertEqual(after, [], '%s: rows left after the plan' % key)


class AllowList(unittest.TestCase):
    def test_every_line_has_a_reason(self):
        path = ROOT / 'pc/tools/pointer_cell_allow.txt'
        for line in path.read_text().splitlines():
            body = line.split('#')[0].strip()
            if body:
                self.assertIn('#', line, line)
                self.assertTrue(line.split('#', 1)[1].strip(), line)


if __name__ == '__main__':
    unittest.main()
