# SPDX-License-Identifier: GPL-3.0-or-later
"""PS1 pointer cells READ at their console width (4 bytes).

Two matched functions the scripted playthrough reaches read 4-byte PS1
pointer cells through native pointer declarations, an 8-byte load on LP64
(pc/tools/audit_ps1_wide_access.py):

  card_file_write  card_file_save_tbl0 (0x800B97B8, a PS1 pointer to the
                   album-name string table) and card_file_save_tbl2
                   (0x800BBDB0, PS1 pointers to the 384-byte records,
                   indexed by the save's record index);
  xa_wait_m        xa_m_tbl / xa_m_tbl2 (0x8012DC44 / 48), stored by
                   xa_set_m as s32.

Each case compiles the function with the flags pc/Makefile gives its object,
links it the way the port links (psx_globals.syms, a 2 MB PS1 RAM at
0x80000000 built from pc/assets/raw/PSX.EXE) with recording stubs for its
callees, and checks the pointers the function hands on:

  fix:              the override passes the PS1 addresses the console
                    reads (card_file_write: the album name string and the
                    record of index 0, 1 and 5; xa_wait_m: the Pair at
                    xa_m_idx in both tables);
  negative control: the matched source passes a non-canonical or wrong
                    pointer (or faults) in the same scenarios.
"""
from pathlib import Path
import struct
import tempfile
import unittest

from safe_process import run
from psx_test_ram import PSX_EXE, retail_ram
import test_ps1_pointer_width as width

ROOT = width.ROOT
RAM = width.RAM
S = width.S

CARD_HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static unsigned long rec[16];
static int nrec;
static void note(const void *p) { rec[nrec++] = (unsigned long)p; }
void card_w_clear(void) {}
int card_file_make(int s) { return 0; }
void save_sys_set(int a) {}
void save_game_set(int a) {}
void save_name_set(void *b, int s) { memset(b, 'A', 8); }
unsigned char rec_table_trim(unsigned char *hi, unsigned char *lo, int s)
{ *hi = IDX; *lo = 0; return 0; }
void func_800B4820(void *d, int n) {}
void func_800B4830(void *d, const void *s, int n) {}
void func_800B4840(void *d, const void *s, int n) { note(s); }
void func_800B48B0(void *d, const void *s, int n) { note(s); }
void func_800B5920(int fd) {}
int func_800B5940(const void *p, int m) { return 3; }
int func_800B5970(int fd, const void *b, int n) { return n; }
int card_file_write(int slot);
int main(int argc, char **argv)
{
    FILE *f;
    int i;
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    f = fopen(argv[1], "rb");
    if (!f || fread(ram, 1, 0x200000, f) != 0x200000) return 3;
    fclose(f);
    card_file_write(SLOT);
    for (i = 0; i < nrec; i++) printf("%lx\n", rec[i]);
    return 0;
}
"""

XA_HARNESS = r"""
#include <stdio.h>
#include <sys/mman.h>
typedef struct { short x, y; } Pos;
static Pos pos[4];
static int cur[3];
static long got_y = -1, got_b = -1;
int get_now_k(void) { return 0; }
int k_disp_inc(void) { return 0; }
void k_disp_switch(void) {}
Pos *get_k_work(void) { return pos; }
unsigned get_CD_State(void) { return 2; }
int *get_XAsector(void) { return cur; }
void set_XAsector(int y, int b, int prev) { got_y = y; got_b = b; }
void device_cmd_send(void) {}
void cd_command_issue(void) {}
void mes_arrow_anim(void) {}
void yazirushi_disp_switch(void) {}
void k_disp_goto_line_end(void) {}
void xa_wait_m(void);
extern unsigned int xa_m_tbl, xa_m_tbl2;
extern int xa_m_mode, xa_m_idx;
extern unsigned char now_girl[4], voice_level;
int main(int argc, char **argv)
{
    unsigned int *t1 = (unsigned int *)0x801F0000UL, *t2 = (unsigned int *)0x801F0100UL;
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    /* two Pair tables in PS1 RAM; xa_set_m stores their PS1 addresses as
     * s32 into the adjacent 4-byte cells */
    t1[2 * IDX] = 0x111; t1[2 * IDX + 1] = 0x222;
    t2[2 * IDX] = 0x333; t2[2 * IDX + 1] = 0x444;
    xa_m_tbl = 0x801F0000u;
    xa_m_tbl2 = 0x801F0100u;
    xa_m_mode = 1;
    xa_m_idx = IDX;
    now_girl[3] = 0;
    voice_level = 3;
    cur[2] = 0x111;
    xa_wait_m();
    printf("%lx %lx %d\n", got_y, got_b, xa_m_mode);
    return 0;
}
"""


def sh(cmd, ok=True):
    r = run([str(c) for c in cmd], cwd=ROOT, timeout=120)
    if ok and not r.ok:
        raise RuntimeError('%s\n%s' % (' '.join(map(str, cmd)), r.stdout[-3000:]))
    return r


def build_and_run(func, source, harness_text, ram=None):
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        obj = tmp / 'f.o'
        width.compile_obj(func, source, obj)
        harness = tmp / 'main.c'
        harness.write_text(harness_text)
        exe = tmp / 'run'
        sh(['gcc', '-std=gnu17', '-O0', '-w', '-no-pie', '-o', exe, harness, obj] + width.ALIAS_LINK + [
            '-Wl,--allow-multiple-definition',
            '-Wl,--just-symbols=%s' % (ROOT / 'pc/ld/psx_globals.syms')])
        args = [exe]
        if ram is not None:
            image = tmp / 'ram.in'
            image.write_bytes(ram)
            args.append(image)
        r = sh(args, ok=False)
        return r.ok, r.stdout




def word(ram, a):
    return struct.unpack_from('<I', ram, a - RAM)[0]


@unittest.skipUnless(PSX_EXE.is_file(), 'needs pc/assets/raw/PSX.EXE')
class CardFileWrite(unittest.TestCase):
    RAM_IMAGE = retail_ram() if PSX_EXE.is_file() else b''

    def calls(self, source, slot, idx):
        text = (CARD_HARNESS.replace('SLOT', str(slot)).replace('IDX', str(idx)))
        ok, out = build_and_run('card_file_write', source, text, self.RAM_IMAGE)
        lines = [l for l in out.split() if all(c in '0123456789abcdef' for c in l)]
        return ok, [int(l, 16) for l in lines]

    def want(self, slot, idx):
        ram = self.RAM_IMAGE
        out = []
        if slot:
            names = word(ram, S['card_file_save_tbl0'])     # 0x800B9778
            out.append(word(ram, names + 4 * (slot - 1)))     # strncat source
        # memcpy(D_8012B660, &tbl1[idx * 32]) then the 384-byte record
        out.append(S['card_file_save_tbl1'] + idx * 32)
        out.append(word(ram, S['card_file_save_tbl2'] + 4 * idx))
        return out

    def test_retail_tables(self):
        self.assertEqual(word(self.RAM_IMAGE, S['card_file_save_tbl0']), 0x800B9778)
        self.assertEqual(word(self.RAM_IMAGE, S['card_file_save_tbl2']), 0x800BAA30)

    def test_override_reads_ps1_width(self):
        for slot, idx in ((0, 0), (1, 0), (3, 1), (2, 5)):
            with self.subTest(slot=slot, idx=idx):
                ok, got = self.calls('src/main/pc-port/card_file_write.c', slot, idx)
                self.assertTrue(ok)
                # the strncat of the slot-0 default string is not a table read
                got = [g for g in got if not (slot == 0 and g == S['card_file_write_str2'])]
                self.assertEqual([hex(g) for g in got], [hex(w) for w in self.want(slot, idx)])

    def test_matched_reads_wide(self):
        """Negative control: for an album slot the matched C dereferences
        the non-canonical 0xB292CC91800B9778 (the process faults before
        strncat), and for idx != 0 it hands on the wrong record."""
        ok, got = self.calls('src/main/matched/card_file_write.c', 1, 0)
        self.assertFalse(ok)
        ok, got = self.calls('src/main/matched/card_file_write.c', 0, 1)
        self.assertTrue(ok)
        self.assertNotEqual(got[-1], self.want(0, 1)[-1])
        self.assertGreater(got[-1], 0xFFFFFFFF)


class XaWaitM(unittest.TestCase):
    def result(self, source, idx):
        ok, out = build_and_run('xa_wait_m', source, XA_HARNESS.replace('IDX', str(idx)))
        return ok, out.split()[-3:] if ok else None

    def test_override_reads_ps1_width(self):
        for idx in (0, 3):
            with self.subTest(idx=idx):
                ok, got = self.result('src/main/pc-port/xa_wait_m.c', idx)
                self.assertTrue(ok)
                # x == cur[2] and x != y: set_XAsector(y, b, ...), mode 0
                self.assertEqual(got, ['333', '444', '0'])

    def test_matched_faults(self):
        """Negative control: (xa_m_tbl2 << 32) | xa_m_tbl is not a pointer."""
        ok, _ = self.result('src/main/matched/xa_wait_m.c', 0)
        self.assertFalse(ok)


if __name__ == '__main__':
    unittest.main()
