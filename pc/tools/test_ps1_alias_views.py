# SPDX-License-Identifier: GPL-3.0-or-later
"""Address-named views of PS1 objects resolve into PS1 RAM.

The matched C spells some PS1 objects with a second name so IDO keeps two
accesses apart (_sys_default_tpage_set_tbl0_w next to _sys_default_tpage_set_tbl0) or with a
decompiler's placeholder (kuchi_stop_w).  psx_globals.syms carries only the
D_<ADDR> and landed names, so such a view resolves through a PROVIDE() in
pc/src/missing_aliases.ld -- unless a host C file defines it, which detaches
it from PS1 RAM (pc/tools/audit_host_globals.py: SHADOWS).

Each case links the matched function the way pc/Makefile links the port
(--just-symbols=pc/ld/psx_globals.syms, the alias script, a mapped 2 MB
PS1 RAM at 0x80000000) and checks the PS1 RAM image against what the
retail code stores (addresses read off SLPS_000.65 / SHUGAKU.EXN):

  mascot_init(1)          mascot2 slot: img 0x800CC4D4, anim_tbl 0x800CC510,
                          hdr 0x800CC4CC, aux14 = *(s16 *)0x800CC514
  mascot_sel_exit         sb 6 -> 0x800EF7F3 (now_icon)
  bustup_req_clear_step   sw 0 -> 0x8012B200
  basyo_init2             sh 184 -> 0x8012DD10 (mes_base_y)
  shugaku_sel_girl13_set  sb 1 -> 0x800D07EC (date_place)
  SD_SerialFrame          gSDInfo 0x8012DD80: +4 &= ~0x200, +0x1C = +0x1E,
                          +0 = (+0 | 0x400) & ~0x100000

Negative control: the same link plus an object that defines the views as
host u32s (what missing_globals.c / pc_port_stubs.c did) fails every case.
"""
from pathlib import Path
import re
import struct
import tempfile
import unittest

from ps1_function_tables import adapt_function_tables
from ps1_pointer_cells import adapt_pointer_cells
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp, spell  # noqa: E402
CFLAGS = ['-std=gnu17', '-fpermissive', '-w', '-funsigned-char',
          '-fno-strict-aliasing', '-O2', '-D_GNU_SOURCE', '-DPC_PORT',
          '-include', 'pc/include/game/types.h',
          '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include')]
RAM = 0x80000000
RAM_SIZE = 0x200000

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned char u8;
void class_clear_n(int n) {}
void mascot_disp_switch(int n) {}
void goto_sub_sub_step(int n) {}
void sub_sub_step_up(void) {}
void basyo_init(int n) {}
void date_name_set(int n) {}
int get_CD_State(void) { return 4; }
void serial_cmd(int n) {}
void SD_SerialClear(void) {}
void SD_SerialVolSend(int n) {}
void SsSetMVol(int a, int b) {}
void SsSetSerialVol(int a, short b, short c) {}
void SsSetSerialAttr(int a, int b, int c) {}
extern void ENTRY();
int main(int argc, char **argv)
{
    FILE *f;
    u8 *ram = mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (u8 *)0x80000000UL) return 2;
    f = fopen(argv[1], "rb");
    if (!f || fread(ram, 1, 0x200000, f) != 0x200000) return 3;
    fclose(f);
    CALL;
    f = fopen(argv[2], "wb");
    fwrite(ram, 1, 0x200000, f);
    fclose(f);
    return 0;
}
"""

# what missing_globals.c / pc_port_stubs.c defined before the fix
HOST_DEFS = '''unsigned int now_icon_c, kuchi_stop_w, mes_base_y_h;
unsigned int _sys_default_tpage_set_tbl0_w, mascot_init_tbl0_w, mascot_init_tbl1_w, mascot_init_tbl2_s;
int D_800D07EC_b;
unsigned int gSDInfo_frame;
'''


def sh(cmd, ok=True):
    r = run([str(c) for c in cmd], cwd=ROOT, timeout=120)
    if ok and not r.ok:
        raise RuntimeError('%s\n%s' % (' '.join(map(str, cmd)), r.stdout[-3000:]))
    return r


def syms():
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r'^(\w+) = 0x([0-9A-Fa-f]+);', (ROOT / 'pc/ld/psx_globals.syms').read_text(), re.M)}


def build(tmp, tag, source, entry, call, host_defs):
    src = tmp / (tag + '.c')
    src.write_text(source)
    obj = tmp / (tag + '.o')
    sh(['gcc'] + CFLAGS + ['-c', src, '-o', obj])
    harness = tmp / (tag + '_main.c')
    harness.write_text(HARNESS.replace('ENTRY', entry).replace('CALL', call))
    objs = [harness, obj]
    if host_defs:
        defs = tmp / 'host_defs.c'
        defs.write_text(HOST_DEFS)
        objs.append(defs)
    exe = tmp / tag
    sh(['gcc', '-std=gnu17', '-O0', '-w', '-no-pie', '-o', exe] + objs
       + [ROOT / 'pc/src/missing_aliases.ld', '-Wl,--allow-multiple-definition',
          '-Wl,--just-symbols=%s' % (ROOT / 'pc/ld/psx_globals.syms')])
    return exe


def execute(exe, tmp, ram):
    image_in, image_out = tmp / 'ram.in', tmp / 'ram.out'
    image_in.write_bytes(bytes(ram))
    if image_out.exists():
        image_out.unlink()
    r = sh([exe, image_in, image_out], ok=False)
    if not r.ok or not image_out.exists():
        return None
    return image_out.read_bytes()


def u32(image, addr):
    return struct.unpack_from('<I', image, addr - RAM)[0]


class Ps1AliasViews(unittest.TestCase):
    S = syms()

    def check(self, tag, path, entry, call, ram, expect, overlay=False):
        source = (ROOT / spell(path)).read_text()
        if overlay:
            source = adapt_pointer_cells(adapt_function_tables(source))
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            got = execute(build(tmp, tag, source, entry, call, False), tmp, ram)
            self.assertIsNotNone(got, 'faulted')
            expect(got)
            bad = execute(build(tmp, tag + '_host', source, entry, call, True), tmp, ram)
            if bad is not None:                 # negative control
                with self.assertRaises(AssertionError):
                    expect(bad)

    def test_mascot_init(self):
        s = self.S
        ram = bytearray(RAM_SIZE)
        struct.pack_into('<h', ram, 0x800CC514 - RAM, 0x1234)

        def expect(image):
            self.assertEqual(u32(image, s['mascot2_cw_img']), 0x800CC4D4)
            self.assertEqual(u32(image, s['mascot2_cw_anim_tbl']), 0x800CC510)
            self.assertEqual(u32(image, s['mascot2_cw_hdr']), 0x800CC4CC)
            self.assertEqual(struct.unpack_from('<h', image, s['mascot2_cw_aux14'] - RAM)[0],
                             0x1234)
            # slot 1 names the same tables (the views exist only for IDO)
            self.assertEqual(u32(image, s['mascot_cw_img']), 0x800CC4D4)
        self.check('mascot', 'src/main/matched/mascot_init.c', 'mascot_init',
                   'mascot_init(1)', ram, expect)

    def test_mascot_sel_exit(self):
        def expect(image):
            self.assertEqual(image[0x800EF7F3 - RAM], 6)
        self.check('sel', 'src/main/matched/mascot_sel_exit.c', 'mascot_sel_exit',
                   'mascot_sel_exit()', bytearray(RAM_SIZE), expect)

    def test_bustup_req_clear_step(self):
        ram = bytearray(RAM_SIZE)
        struct.pack_into('<I', ram, 0x8012B200 - RAM, 0xA5A5A5A5)

        def expect(image):
            self.assertEqual(u32(image, 0x8012B200), 0)
        self.check('bustup', 'src/main/matched/bustup_req_clear_step.c',
                   'bustup_req_clear_step', 'bustup_req_clear_step()', ram, expect)

    def test_basyo_init2(self):
        def expect(image):
            self.assertEqual(struct.unpack_from('<H', image, 0x8012DD10 - RAM)[0], 184)
        self.check('basyo', 'src/main/matched/basyo_init2.c', 'basyo_init2',
                   'basyo_init2(3)', bytearray(RAM_SIZE), expect)

    def test_shugaku_sel_girl13_set(self):
        ram = bytearray(RAM_SIZE)
        struct.pack_into('<i', ram, self.S['serifu_no'] - RAM, 1)

        def expect(image):
            self.assertEqual(image[0x800D07EC - RAM], 1)
        self.check('shugaku', 'src/overlay/SHUGAKU/shugaku_sel_girl13_set.c',
                   'shugaku_sel_girl13_set', 'shugaku_sel_girl13_set()', ram,
                   expect, overlay=True)

    def test_sd_serial_frame(self):
        base = 0x8012DD80
        ram = bytearray(RAM_SIZE)
        struct.pack_into('<7i2h', ram, base - RAM, 0x00100003, 0x00000F00, 9, 0, 0,
                         0, 0, 11, 0x55)

        def expect(image):
            f = struct.unpack_from('<7i2h', image, base - RAM)
            self.assertEqual(f[0], 0x00000403)
            self.assertEqual(f[1], 0x00000D00)
            self.assertEqual(f[7], 0x55)
            self.assertEqual(f[8], 0x55)
        self.check('sdserial', 'src/main/matched/SD_SerialFrame.c', 'SD_SerialFrame',
                   'SD_SerialFrame()', ram, expect)


if __name__ == '__main__':
    unittest.main()
