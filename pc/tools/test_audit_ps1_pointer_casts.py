# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_ps1_pointer_casts.py: the two pointer shapes no object audit sees.

  fix:              the PS1-width overrides carry neither shape
                    (save_file_select_init, telephone_choice_menu_init,
                    InitSoundSeq, InitSoundSep);
  negative control: the matched sources they replace are reported --
                    CELL-READ (`*(u8 **)(card_file_save_tbl0 + offset)`),
                    SEXT-PTR (`(u8 *)` of an `s32`), RECORD-PTR (the score
                    record's `u8 *ptr`).
"""
import shutil
import tempfile
import unittest
from pathlib import Path

import audit_ps1_pointer_casts as pc
from pc_paths import spell

HAVE = shutil.which(pc.CLANG) is not None


def rows(path):
    """path: repository-relative (a decomp file is read in toki1-decomp)"""
    r, err = pc.scan_source(['gcc'] + pc.GAME_FLAGS, spell(path))
    if err:
        raise AssertionError(err)
    return r


def shapes(path):
    return {r['shape'] for r in rows(path)}


@unittest.skipUnless(HAVE, 'needs clang')
class Shapes(unittest.TestCase):
    def test_cell_read(self):
        self.assertIn('CELL-READ', shapes('src/main/matched/save_file_select_init.c'))
        self.assertEqual(shapes('src/main/pc-port/save_file_select_init.c'), set())

    def test_sign_extension(self):
        r = [x for x in rows('src/main/matched/telephone_choice_menu_init.c')
             if x['shape'] == 'SEXT-PTR']
        self.assertTrue(r)
        self.assertEqual(shapes('src/main/pc-port/telephone_choice_menu_init.c'), set())

    def test_record_pointer(self):
        for f in ('InitSoundSeq', 'InitSoundSep'):
            with self.subTest(f=f):
                self.assertIn('RECORD-PTR', shapes('src/main/matched/%s.c' % f))
                self.assertEqual(shapes('src/main/pc-port/%s.c' % f), set())

    def test_synthetic(self):
        src = r'''
typedef struct { u32 a; u8 *p; } Rec;
s32 g;
u8 *f(s32 a, u32 b, u8 *q)
{
    u8 *local[2];
    Rec r;
    u8 *x = (u8 *)a;                 /* SEXT-PTR */
    u8 *y = (u8 *)b;                 /* zero-extends: fine */
    u8 *z = (u8 *)0x80100000;        /* constant: fine */
    u8 *w = *(u8 **)(b + 4);         /* CELL-READ */
    u8 *v = *(u8 **)&local[1];       /* host local: fine */
    u8 *u = ((Rec *)q)->p;           /* CELL-MEMBER */
    Rec *rp = (Rec *)q;
    r.p = q;                         /* a host local record: fine */
    return x + (long)y + (long)z + (long)w + (long)v + (long)u + (long)rp->p; /* RECORD-PTR */
}
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 's.c'
            p.write_text(src)
            got = sorted((r['shape'], r['line']) for r in rows(str(p)))
        self.assertEqual(got, [('CELL-MEMBER', 13), ('CELL-READ', 11), ('RECORD-PTR', 16),
                               ('SEXT-PTR', 8)])


if __name__ == '__main__':
    unittest.main()
