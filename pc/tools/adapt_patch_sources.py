#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""adapt_patch_sources.py -- the English patch engine's C, built natively (PC).

src/patch/main/*.c is the translation track's code: IDO-compiled for the
console, where a pointer is 4 bytes.  The PC compiles the same files for
LP64 (docs/pc-port/TRANSLATION-ON-PC.md).  Where the C reads or writes a
POINTER STORED IN PS1 MEMORY (a 4-byte cell: a retail pointer table, a
pointer variable in the retail data, a class_w record field) it must go
through a 4-byte cell on the PC, exactly the rewrite pc/tools/
adapt_ps1_cells.py and the overlay module generator apply to matched code.

The source is NOT edited.  This filter writes an adapted copy for the
compiler, starting with a #line directive naming the original.  Every rule
is an exact text match that must hit the expected number of times, so an
edit of the patch code by the translation track fails the PC build loudly
instead of compiling a stale rewrite: re-derive the rule from the new code.

usage: adapt_patch_sources.py SRC OUT
"""
import os
import sys

# helper macros prepended to every adapted file
PRELUDE = r'''#include <stdint.h>
/* PC port: a 4-byte PS1 pointer cell read as a native pointer */
#define PT_PC_CELL(T, word) ((T)(uintptr_t)(u32)(word))
#define PT_PC_WORD(p) ((u32)(uintptr_t)(p))
'''

# file -> [(old, new, count)]
RULES = {
    'text_misc.c': [
        # class_w record: the three handles are 4-byte PS1 words
        ('    s32 *f12;\n    s32 *f16;\n', '    u32 f12;\n    u32 f16;\n', 1),
        ('    s32 *f52;\n', '    u32 f52;\n', 1),
        ('class_w.rec[i].f12 = &basyo_init_tbl1;', 'class_w.rec[i].f12 = PT_PC_WORD(&basyo_init_tbl1);', 1),
        ('class_w.rec[i].f16 = &basyo_init_tbl2;', 'class_w.rec[i].f16 = PT_PC_WORD(&basyo_init_tbl2);', 1),
        ('class_w.rec[i].f52 = &_sys_default_tpage_set_tbl2;',
         'class_w.rec[i].f52 = PT_PC_WORD(&_sys_default_tpage_set_tbl2);', 1),
        # girl family-name table: s32 words holding PS1 string addresses
        ('strcpy(name, date_name_set_tbl0[i]);',
         'strcpy(name, PT_PC_CELL(u8 *, date_name_set_tbl0[i]));', 1),
        # the phone book: pointer words / a table of 4-byte pointer cells
        ('extern u32 *telephone_choice_menu_init_tbl0[];', 'extern u32 telephone_choice_menu_init_tbl0[];', 1),
        ('(u8 *)telephone_choice_menu_init_tbl2)', 'PT_PC_CELL(u8 *, telephone_choice_menu_init_tbl2))', 1),
        ('(u8 *)telephone_choice_menu_init_tbl1)', 'PT_PC_CELL(u8 *, telephone_choice_menu_init_tbl1))', 1),
        ('(u8 *)telephone_choice_menu_init_tbl0[i])', 'PT_PC_CELL(u8 *, telephone_choice_menu_init_tbl0[i]))', 1),
        # calendar sprite handles: pointer variables in the retail data
        ('extern u32 *cal_cw_img;\nextern u32 *cal_cw_anim_tbl;\nextern u32 *cal_cw_hdr;',
         'extern u32 cal_cw_img;\nextern u32 cal_cw_anim_tbl;\nextern u32 cal_cw_hdr;', 1),
        ('cal_cw_img = (u32 *)&basyo_init_tbl1;', 'cal_cw_img = PT_PC_WORD(&basyo_init_tbl1);', 1),
        ('cal_cw_anim_tbl = (u32 *)&basyo_init_tbl2;', 'cal_cw_anim_tbl = PT_PC_WORD(&basyo_init_tbl2);', 1),
        ('cal_cw_hdr = (u32 *)&_sys_default_tpage_set_tbl2;',
         'cal_cw_hdr = PT_PC_WORD(&_sys_default_tpage_set_tbl2);', 1),
        # pt_pn: retail passes the name as a 32-bit word
        ('strcpy(dst, (s32)name);', 'strcpy(dst, name);', 1),
    ],
    'text_event.c': [
        # the voice table: two levels of 4-byte pointer cells
        ('e = ((u32 ***)voice)[i1][i2];',
         'e = PT_PC_CELL(u32 *, ((u32 *)PT_PC_CELL(u32 *, ((u32 *)voice)[i1]))[i2]);', 1),
        ('e = ((u32 ***)voice)[i1][i2] + serifu_no * 2;',
         'e = PT_PC_CELL(u32 *, ((u32 *)PT_PC_CELL(u32 *, ((u32 *)voice)[i1]))[i2]) + serifu_no * 2;', 1),
        ('(u8 *)date_name_set_tbl0[0]', 'PT_PC_CELL(u8 *, date_name_set_tbl0[0])', 1),
    ],
    'text_name_ent.c': [
        ('extern u8 *D_8014CD6C[];', 'extern u32 D_8014CD6C[];', 1),
        ('extern u8 *D_8014CDB0[];', 'extern u32 D_8014CDB0[];', 1),
        ('extern u16 *D_8014CE64;', 'extern u32 D_8014CE64;', 1),
        ('extern u8 **D_8014CE68;', 'extern u32 D_8014CE68;', 1),
        ('D_8014CD6C[col / 6 + row * 3]', 'PT_PC_CELL(u8 *, D_8014CD6C[col / 6 + row * 3])', 1),
        ('D_8014CDB0[D_8014CCB4]', 'PT_PC_CELL(u8 *, D_8014CDB0[D_8014CCB4])', 2),
        ('D_8014CE64[38]', 'PT_PC_CELL(u16 *, D_8014CE64)[38]', 1),
        ('D_8014CE64[D_8014CCB4]', 'PT_PC_CELL(u16 *, D_8014CE64)[D_8014CCB4]', 2),
        ('D_8014CE68[38]', 'PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8014CE68)[38])', 1),
        ('D_8014CE68[D_8014CCB4]', 'PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8014CE68)[D_8014CCB4])', 2),
    ],
    'text_ov_date2.c': [
        ('extern u8 **D_8013B5B4;\nextern u8 **D_8013B5B8;\nextern u8 **D_8013B5BC;\nextern u8 **D_8013B5C0;',
         'extern u32 D_8013B5B4;\nextern u32 D_8013B5B8;\nextern u32 D_8013B5BC;\nextern u32 D_8013B5C0;', 1),
        ('D_8013B5B4[i]', 'PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8013B5B4)[i])', 1),
        ('D_8013B5B8[i]', 'PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8013B5B8)[i])', 1),
        ('D_8013B5BC[i]', 'PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8013B5BC)[i])', 1),
        ('D_8013B5C0[i]', 'PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8013B5C0)[i])', 1),
    ],
    'text_ov_labels.c': [
        ('extern u8 ***D_80143BB4;\nextern u8 **D_80143BB8;', 'extern u32 D_80143BB4;\nextern u32 D_80143BB8;', 1),
        ('**D_80143BB8 = 7;', '*PT_PC_CELL(u8 *, *PT_PC_CELL(u32 *, D_80143BB8)) = 7;', 1),
        ('(**D_80143BB4)[', 'PT_PC_CELL(u8 *, *PT_PC_CELL(u32 *, *PT_PC_CELL(u32 *, D_80143BB4)))[', 3),
        ('extern u8 **D_8013B760;', 'extern u32 D_8013B760;', 1),
        ('extern u32 *D_8013B764;', 'extern u32 D_8013B764;', 1),
        ('s = D_8013B760[row];', 's = PT_PC_CELL(u8 *, PT_PC_CELL(u32 *, D_8013B760)[row]);', 1),
        ('p = &D_8013B764[step_mode];', 'p = &PT_PC_CELL(u32 *, D_8013B764)[step_mode];', 1),
    ],
}


def adapt(name, text):
    for old, new, count in RULES.get(name, ()):
        got = text.count(old)
        if got != count:
            raise SystemExit('adapt_patch_sources: %s: expected %d of %r, found %d -- the patch '
                             'code changed; re-derive the PC rule' % (name, count, old, got))
        text = text.replace(old, new)
    return text


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    src, out = argv[1], argv[2]
    name = os.path.basename(src)
    text = open(src, encoding='utf-8', errors='surrogateescape').read()
    body = adapt(name, text)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', errors='surrogateescape') as fh:
        fh.write(PRELUDE)
        fh.write('#line 1 "%s"\n' % src)
        fh.write(body)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
