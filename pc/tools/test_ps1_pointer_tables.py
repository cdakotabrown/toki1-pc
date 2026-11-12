# SPDX-License-Identifier: GPL-3.0-or-later
"""PS1 pointer TABLES and cells read at console width on the date path.

The phone call, the date day, the date and the save menu read 4-byte PS1
pointers through native declarations (pc/tools/audit_ps1_wide_access.py,
WIDE-INDEX / WIDE-LOAD):

  telephone_choice_menu_init  telephone_choice_menu_init_tbl0 (13 PS1 name
                              pointers, read at stride 8) and tbl1 / tbl2
                              (`s32` cast to a pointer: sign-extended);
  date_name_set               date_name_set_tbl0 (14 PS1 name pointers,
                              read at stride 8);
  dec_bg_load_check           mdec_bs (a 4-byte PS1 pointer cell, the next
                              word is mdec_rl);
  bustup_check_init,          bustup_check_init_tbl0 / tbl1 / tbl2: adjacent
  bustup_load_step,           4-byte PS1 pointer cells into the bust-up
  zyotai_sprite_set_chk       archive (0x80180000);
  save_file_select_init,      the album-name table through
  menu_step_main6             card_file_save_tbl0 (`*(u8 **)(tbl + 4*i)`);
  album_info_disp             album_info_disp_tbl0 / tbl1 (PS1 string
                              pointers, read at stride 8).

Each case links the function the way the port does (psx_globals.syms, a
2 MB PS1 RAM at 0x80000000 built from pc/assets/raw/PSX.EXE) with recording
stubs for its callees (test_ps1_pointer_reads.build_and_run):

  fix:              the override hands on the PS1 addresses the console
                    reads;
  negative control: the matched source hands on a joined, sign-extended or
                    non-canonical pointer (or faults) in the same case.
"""
import struct
import unittest

import test_ps1_pointer_reads as reads
from psx_test_ram import PSX_EXE, retail_ram

S = reads.S

PROLOGUE = r"""
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static unsigned long rec[64];
static int nrec;
static void note(unsigned long p) { if (nrec < 64) rec[nrec++] = p; }
static int load_ram(const char *path)
{
    FILE *f;
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    f = fopen(path, "rb");
    if (!f || fread(ram, 1, 0x200000, f) != 0x200000) return 3;
    fclose(f);
    return 0;
}
static void dump(void)
{
    int i;
    for (i = 0; i < nrec; i++) printf("%lx\n", rec[i]);
}
"""

TEL_HARNESS = PROLOGUE + r"""
int set_kanji_string(int x, int y, int c, unsigned long s, int a) { note(s); return 0; }
void menu_set(int a, int n, short *x, short *y, short *w, short *h) {}
void class_clear_n(int n) {}
void telephone_choice_menu_init(void);
extern unsigned char screen_w[];
int main(int argc, char **argv)
{
    int i, r = load_ram(argv[1]);
    if (r) return r;
    /* every girl met: bit 3 of the word at screen_w + i*56 + 400 */
    for (i = 0; i < 14; i++) *(unsigned int *)(screen_w + i * 56 + 400) |= 8;
    telephone_choice_menu_init();
    dump();
    return 0;
}
"""

NAME_HARNESS = PROLOGUE + r"""
void func_800B4890(void *d, unsigned long s) { note(s); }
void get_p_name(void *d, int i) {}
void get_g_name(void *d, int i) {}
void date_name_set(int girl);
int main(int argc, char **argv)
{
    int r = load_ram(argv[1]);
    if (r) return r;
    date_name_set(GIRL);
    dump();
    return 0;
}
"""

DEC_HARNESS = PROLOGUE + r"""
static int inits, ups;
void dec_bg_init(int a) { inits++; }
void sub_sub_step_up(void) { ups++; }
int dec_bg_load_check(void);
extern unsigned int mdec_bs, mdec_rl;
extern unsigned char now_sub_sub_step;
int main(int argc, char **argv)
{
    unsigned int *bs = (unsigned int *)0x801F0000UL;
    int r = load_ram(argv[1]);
    if (r) return r;
    /* dec_env_set: mdec_bs = 0x801F0000 (here: a valid MDEC header),
     * mdec_rl = 0x80167000 in the next PS1 word */
    bs[0] = 0x38000000u | 0x1234u;
    bs[1] = 0x00010000u;
    mdec_bs = 0x801F0000u;
    mdec_rl = 0x80167000u;
    now_sub_sub_step = 11;
    dec_bg_load_check();
    printf("%d %d %d\n", inits, ups, now_sub_sub_step);
    return 0;
}
"""


def word(ram, a):
    return struct.unpack_from('<I', ram, a - 0x80000000)[0]


def hexes(out):
    return [int(l, 16) for l in out.split() if all(c in '0123456789abcdef' for c in l)]


@unittest.skipUnless(PSX_EXE.is_file(), 'needs pc/assets/raw/PSX.EXE')
class PointerTables(unittest.TestCase):
    RAM = retail_ram() if PSX_EXE.is_file() else b''

    def run_case(self, func, source, harness):
        return reads.build_and_run(func, source, harness, self.RAM)

    # ---- telephone_choice_menu_init
    def tel_want(self):
        ram = self.RAM
        out = [S['telephone_choice_menu_init_str0']]
        for i in range(14):
            if i == 0:
                out.append(word(ram, S['telephone_choice_menu_init_tbl2']))
            elif i == 13:
                out.append(S['telephone_choice_menu_init_str1'])
            elif i == 12:
                out.append(word(ram, S['telephone_choice_menu_init_tbl1']))
            else:
                out.append(word(ram, S['telephone_choice_menu_init_tbl0'] + 4 * i))
        return out

    def test_tel_retail_tables(self):
        want = self.tel_want()
        self.assertEqual(want[1], 0x800B9A00)        # tbl2: Yoshio
        self.assertTrue(all(0x80000000 <= p < 0x80200000 for p in want))

    def test_tel_override(self):
        ok, out = self.run_case('telephone_choice_menu_init',
                                'src/main/pc-port/telephone_choice_menu_init.c', TEL_HARNESS)
        self.assertTrue(ok, out)
        self.assertEqual([hex(p) for p in hexes(out)], [hex(p) for p in self.tel_want()])

    def test_tel_matched_negative_control(self):
        """the matched C passes tbl2 sign-extended and joins tbl0 entries"""
        ok, out = self.run_case('telephone_choice_menu_init',
                                'src/main/matched/telephone_choice_menu_init.c', TEL_HARNESS)
        self.assertTrue(ok, out)
        got = hexes(out)
        self.assertEqual(got[1], 0xFFFFFFFF800B9A00)
        self.assertTrue(any(p > 0xFFFFFFFF for p in got[2:13]))
        self.assertNotEqual(got, self.tel_want())

    # ---- date_name_set
    def test_date_name_override(self):
        for girl in (0, 1, 5, 13):
            with self.subTest(girl=girl):
                ok, out = self.run_case('date_name_set', 'src/main/pc-port/date_name_set.c',
                                        NAME_HARNESS.replace('GIRL', str(girl)))
                self.assertTrue(ok, out)
                got = hexes(out)
                # strcpy(p_name), strcpy(pp_name), strcpy(g_name, tbl0[girl])
                self.assertEqual(got[2], word(self.RAM, S['date_name_set_tbl0'] + 4 * girl))

    def test_date_name_matched_negative_control(self):
        """girl 0: 0x800B9A6C800B9A64 (two entries joined)"""
        ok, out = self.run_case('date_name_set', 'src/main/matched/date_name_set.c',
                                NAME_HARNESS.replace('GIRL', '0'))
        self.assertTrue(ok, out)
        got = hexes(out)
        self.assertEqual(got[2], (word(self.RAM, S['date_name_set_tbl0'] + 4) << 32)
                         | word(self.RAM, S['date_name_set_tbl0']))

    # ---- dec_bg_load_check
    def test_dec_bg_override(self):
        ok, out = self.run_case('dec_bg_load_check', 'src/main/pc-port/dec_bg_load_check.c',
                                DEC_HARNESS)
        self.assertTrue(ok, out)
        # a valid header: dec_bg_init(1), one step up, no rewind
        self.assertEqual(out.split()[-3:], ['1', '1', '11'])

    def test_dec_bg_matched_negative_control(self):
        """0x80167000801F0000 is not an address: the matched read faults"""
        ok, _ = self.run_case('dec_bg_load_check', 'src/main/matched/dec_bg_load_check.c',
                              DEC_HARNESS)
        self.assertFalse(ok)


BUSTUP_HARNESS = PROLOGUE + r"""
static int ups;
int block_checksum_check(int n, unsigned int a) { return 1; }
int get_g_zyotai_h(int g) { return 0; }
void bustup_sprite_init(int a0, unsigned long a1, unsigned long a2, unsigned long a3, int a4)
{ note(a1); note(a2); note(a3); }
int sub_sub_step_up(void) { ups++; return 0; }
void bustup_cd_read(void) {}
int get_CD_State2(void) { return 1; }
int FUNC(void);
extern unsigned int bustup_check_init_tbl0, bustup_check_init_tbl1, bustup_check_init_tbl2;
extern unsigned char now_step, now_sub_sub_step, step_mode;
int main(int argc, char **argv)
{
    static const unsigned int img[5] = { 0x801820ACu, 0x801860ACu, 0x8018A0ACu,
                                         0x8018E0ACu, 0x801920ACu };
    unsigned int *t0 = (unsigned int *)0x801F0000UL, *t1 = (unsigned int *)0x801F0100UL,
                 *t2 = (unsigned int *)0x801F0200UL;
    int r = load_ram(argv[1]);
    if (r) return r;
    memcpy(t0, img, sizeof img);
    t1[0] = 0x80180000u;
    t2[0] = 0x8018005Cu; t2[1] = 0x80180070u;
    /* bustup_cd_read's archive header: three adjacent PS1 pointer cells */
    bustup_check_init_tbl0 = 0x801F0000u;
    bustup_check_init_tbl1 = 0x801F0100u;
    bustup_check_init_tbl2 = 0x801F0200u;
    now_step = 80;
    now_sub_sub_step = 37;
    step_mode = 1;
    FUNC();
    dump();
    printf("ups %d sss %d mode %d\n", ups, now_sub_sub_step, step_mode);
    return 0;
}
"""


@unittest.skipUnless(PSX_EXE.is_file(), 'needs pc/assets/raw/PSX.EXE')
class BustupCells(unittest.TestCase):
    RAM = retail_ram() if PSX_EXE.is_file() else b''
    FUNCS = ('bustup_check_init', 'bustup_load_step', 'zyotai_sprite_set_chk')

    def run_case(self, func, source):
        return reads.build_and_run(func, source, BUSTUP_HARNESS.replace('FUNC', func), self.RAM)

    def test_overrides_accept_the_archive(self):
        """the header checks pass: one step up, no rewind; the sprite set
        gets the three PS1 addresses"""
        for func in self.FUNCS:
            with self.subTest(func=func):
                ok, out = self.run_case(func, 'src/main/pc-port/%s.c' % func)
                self.assertTrue(ok, out)
                self.assertIn('ups 1 sss 37', out)
                if func != 'bustup_load_step':
                    self.assertEqual(hexes(out)[:3], [0x801F0000, 0x801F0100, 0x801F0200])

    def test_matched_negative_control(self):
        """(tbl1 << 32) | tbl0 is not an address: the matched reads fault"""
        for func in self.FUNCS:
            with self.subTest(func=func):
                ok, _ = self.run_case(func, 'src/main/matched/%s.c' % func)
                self.assertFalse(ok)


SLOT_HARNESS = PROLOGUE + r"""
int set_kanji_string(int x, int y, int c, unsigned long s, int a) { note(s); return 0; }
void text_sys_init(void) {}
void basyo_init2(void *s) {}
void menu_set(int a, int n, short *x, short *y, short *w, short *h) {}
void k_sub_reset_point_set(void) {}
void goto_sub_sub_step(int s) {}
void sub_sub_step_up(void) {}
void album_info_disp(int a, int b) {}
void save_file_msg_disp(void) {}
void save_file_box_disp(void) {}
void save_msg_show(void) {}
void slot_box_disp(void) {}
void FUNC(void);
int main(int argc, char **argv)
{
    int r = load_ram(argv[1]);
    if (r) return r;
    FUNC();
    dump();
    return 0;
}
"""

ALBUM_HARNESS = PROLOGUE + r"""
void func_800B4890(void *d, unsigned long s) { note(s); }          /* strcpy */
void func_800B48B0(void *d, unsigned long s, int n) { note(s); }   /* strncat */
int set_kanji_string(int x, int y, int c, void *s, int a) { return 0; }
void k_sub_reset(void) {}
void k_sub_disp_start(int a) {}
void album_info_disp(int a0, int a1);
extern unsigned char now_album, album_slot;
extern unsigned char album_work[];
int main(int argc, char **argv)
{
    int r = load_ram(argv[1]);
    if (r) return r;
    album_work[3806] = 0;       /* mode 0: the album_chr[album_slot] record */
    album_work[4 + 1] = 1;      /* album 1 holds a game */
    now_album = 1;
    album_slot = 0;
    album_info_disp(0, 0);
    dump();
    return 0;
}
"""


@unittest.skipUnless(PSX_EXE.is_file(), 'needs pc/assets/raw/PSX.EXE')
class SaveMenus(unittest.TestCase):
    """save_file_select_init / menu_step_main6 (the album-name table through
    card_file_save_tbl0) and album_info_disp (its two string tables)"""
    RAM = retail_ram() if PSX_EXE.is_file() else b''
    SLOTS = ('save_file_select_init', 'menu_step_main6')

    def names(self):
        tbl = word(self.RAM, S['card_file_save_tbl0'])
        return [word(self.RAM, tbl + 4 * i) for i in range(14)]

    def slot_case(self, func, source):
        return reads.build_and_run(func, source, SLOT_HARNESS.replace('FUNC', func), self.RAM)

    def test_slot_menus_override(self):
        for func in self.SLOTS:
            with self.subTest(func=func):
                ok, out = self.slot_case(func, 'src/main/pc-port/%s.c' % func)
                self.assertTrue(ok, out)
                self.assertEqual(hexes(out), self.names())

    def test_slot_menus_matched_negative_control(self):
        """entry i read 8 bytes wide: names i and i+1 joined"""
        for func in self.SLOTS:
            with self.subTest(func=func):
                ok, out = self.slot_case(func, 'src/main/matched/%s.c' % func)
                self.assertTrue(ok, out)
                got = hexes(out)
                n = self.names()
                self.assertEqual(got[0], (n[1] << 32) | n[0])

    def album_case(self, source):
        return reads.build_and_run('album_info_disp', source, ALBUM_HARNESS, self.RAM)

    def test_album_info_override(self):
        ok, out = self.album_case('src/main/pc-port/album_info_disp.c')
        self.assertTrue(ok, out)
        got = hexes(out)
        self.assertEqual(got[0], word(self.RAM, S['album_info_disp_tbl0'] + 4 * 1))
        self.assertTrue(got and all(p <= 0xFFFFFFFF for p in got))

    def test_album_info_matched_negative_control(self):
        ok, out = self.album_case('src/main/matched/album_info_disp.c')
        self.assertTrue(ok, out)
        got = hexes(out)
        t = S['album_info_disp_tbl0']
        self.assertEqual(got[0], (word(self.RAM, t + 12) << 32) | word(self.RAM, t + 8))


if __name__ == '__main__':
    unittest.main()
