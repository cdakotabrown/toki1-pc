# SPDX-License-Identifier: GPL-3.0-or-later
"""Overlay records that hold PS1 handles keep the console's width.

Two matched overlay functions read retail records whose pointer members
are 4-byte PS1 handles, through a struct that declares them as native
pointers (8 bytes on LP64):

  * RPG_BAT rpg_bat_pick_list_set / rpg_bat_msg_build copy 16-byte .data
    tables of four string handles (`StrSet {u8 *v[4]}`) by value: the host
    copy is 32 bytes, so v[0] fuses handles 0 and 1 and v[2]/v[3] come
    from the next table.
  * TACO node_pose_set reads the 16-byte node table taco_obj (written by
    taco_actor_alloc as four s32) as `{s32; Rec *f4; s32; s32}`: 24 bytes,
    so node idx is read 8*idx bytes too far and f4 from +8.

The overrides in pc/src/overlay/<OV>/<f>.c declare the handles as u32.

Each case links the function into a host binary with every data symbol it
names at its PS1 address inside a mapped 2 MB PS1 RAM (as the PC build
does through pc/ld/psx_globals.syms), seeds synthetic tables there, and
compares the outcome with the console semantics computed here:

  negative control: the MATCHED function, adapted exactly as
                    generate_overlay_modules.py adapts it for the module,
                    faults or leaves a different PS1 RAM image;
  fix:              the override leaves exactly the expected image.
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
from pc_paths import DECOMP, decomp, spell  # noqa: E402

CFLAGS = ['-std=gnu17', '-fpermissive', '-w', '-funsigned-char',
          '-fno-strict-aliasing', '-O2', '-D_GNU_SOURCE', '-DPC_PORT',
          '-include', 'pc/include/game/types.h',
          '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include')]
OVERRIDES = ROOT / 'pc/src/overlay'
RAM = 0x80000000
RAM_SIZE = 0x200000

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef unsigned char u8;
typedef unsigned int u32;
extern int FUNC_ENTRY();
static const int script[] = {SCRIPT};
static int pos;
EXTRA
int get_rand_mod(int n) { int v = script[pos % (int)(sizeof(script) / sizeof(script[0]))]; pos++; return v % n; }
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



def landed_data_addresses():
    """name -> PS1 address for every landed DATA name in config/symbol_names.json
    (main and overlay scopes; overlay data names are tree-unique)."""
    import json
    doc = json.loads((DECOMP / 'config' /
                      'symbol_names.json').read_text())['symbols']
    out = {}
    for key, e in doc.items():
        if e.get('kind') == 'data' and e.get('landed'):
            out[e['name']] = int(key.split(':')[-1], 16)
    return out

def sh(cmd, ok=True):
    r = run([str(c) for c in cmd], cwd=ROOT, timeout=120)
    if ok and not r.ok:
        raise RuntimeError('%s\n%s' % (' '.join(map(str, cmd)), r.stdout[-3000:]))
    return r


def w32(ram, addr, value):
    struct.pack_into('<I', ram, addr - RAM, value)


def build(tmp, tag, source, entry, call, script, symbols, extra=''):
    """One binary: `source` (the C text of the function) + harness, every
    D_<ADDR> data symbol at its PS1 address."""
    src = tmp / ('%s.c' % tag)
    src.write_text(source)
    obj = tmp / ('%s.o' % tag)
    sh(['gcc'] + CFLAGS + ['-c', src, '-o', obj])
    harness = tmp / ('%s_main.c' % tag)
    harness.write_text(HARNESS.replace('FUNC_ENTRY', entry).replace('CALL', call)
                       .replace('EXTRA', extra).replace('SCRIPT', ', '.join(map(str, script))))
    undefined = set(re.findall(r'^\s+U (\w+)$', sh(['nm', obj]).stdout, re.M))
    defsyms = []
    landed = landed_data_addresses()
    for name in sorted(undefined):
        m = re.match(r'D_([0-9A-F]{8})$', name)
        if m:
            defsyms.append('-Wl,--defsym,%s=0x%s' % (name, m.group(1)))
        elif name in landed and name not in symbols:
            # a data name landed by the naming waves (config/symbol_names.json):
            # the same PS1 address its D_ spelling encoded
            defsyms.append('-Wl,--defsym,%s=0x%08X' % (name, landed[name]))
    for name, addr in symbols.items():
        defsyms.append('-Wl,--defsym,%s=0x%08X' % (name, addr))
    exe = tmp / tag
    sh(['gcc', '-std=gnu17', '-O0', '-w', '-no-pie', '-o', exe, harness, obj] + defsyms)
    return exe


def execute(exe, tmp, ram):
    image_in, image_out = tmp / 'ram.in', tmp / 'ram.out'
    image_in.write_bytes(bytes(ram))
    if image_out.exists():
        image_out.unlink()
    r = sh([exe, image_in, image_out], ok=False)
    if not r.ok or not image_out.exists():
        return None                     # faulted
    return image_out.read_bytes()


def module_view(path):
    """The matched function adapted as generate_overlay_modules.py adapts
    it for the overlay module."""
    return adapt_pointer_cells(adapt_function_tables((ROOT / spell(path)).read_text()))


# ---- RPG_BAT ------------------------------------------------------------
STRINGS = 0x80170000          # synthetic strings
LINES = 0x80171000            # ten 40-byte line buffers


def rpg_ram(tables, prefixes):
    ram = bytearray(RAM_SIZE)
    text = {}
    addr = STRINGS
    for table, n in tables:
        for i in range(4):
            s = ('T%X_%d' % (table & 0xFFF, i)).encode()
            ram[addr - RAM:addr - RAM + len(s) + 1] = s + b'\0'
            w32(ram, table + 4 * i, addr)
            text[(table, i)] = s
            addr += 16
    for i in range(10):
        w32(ram, 0x8015EB70 + 4 * i, LINES + 40 * i)
    for p, s in prefixes.items():
        ram[p - RAM:p - RAM + len(s) + 1] = s + b'\0'
    return ram, text


def line(image, i):
    raw = image[LINES - RAM + 40 * i:LINES - RAM + 40 * i + 40]
    return raw.split(b'\0')[0]


class Ps1HandleRecords(unittest.TestCase):
    def check(self, tag, path, entry, call, script, ram, expect, symbols=None,
              extra=''):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            ovr = build(tmp, tag + '_ovr', (OVERRIDES / path).read_text(),
                        entry, call, script, symbols or {}, extra)
            mat = build(tmp, tag + '_mat', module_view('src/overlay/' + path),
                        entry, call, script, symbols or {}, extra)
            got = execute(ovr, tmp, ram)
            self.assertIsNotNone(got, 'override faulted')
            expect(got)
            raw = execute(mat, tmp, ram)
            if raw is not None:                 # negative control
                with self.assertRaises(AssertionError):
                    expect(raw)

    def test_rpg_bat_pick_list_set(self):
        tables = [(0x8015EFA0, 4), (0x8015EFB0, 4), (0x8015EFC0, 4)]
        prefix = b'P:'
        for sel, table in ((0x10000, 0x8015EFA0), (0x20000, 0x8015EFB0),
                           (0x40000, 0x8015EFC0)):
            ram, text = rpg_ram(tables, {0x8015D858: prefix})
            w32(ram, 0x8015EB98, sel)
            # flags: get_rand_mod(5) -> 0 picks three; then slots 3, 0, 2
            script = [0, 3, 0, 2]
            picked = sorted({3, 0, 2})

            def expect(image, table=table, text=text, picked=picked):
                for k, i in enumerate(picked):
                    self.assertEqual(line(image, k + 4), prefix + text[(table, i)])
                self.assertEqual(struct.unpack_from('<i', image, 0x8015ED14 - RAM)[0],
                                 len(picked) + 4)
            with self.subTest(sel=hex(sel)):
                self.check('pick', 'RPG_BAT/rpg_bat_pick_list_set.c',
                           'rpg_bat_pick_list_set', 'rpg_bat_pick_list_set()',
                           script, ram, expect)

    def test_rpg_bat_msg_build(self):
        tables = [(0x8015EFE0, 4), (0x8015EFF0, 4)]
        pre_a, pre_b = b'A:', b'B:'
        ram, text = rpg_ram(tables, {0x8015E1A8: pre_a, 0x8015E1B4: pre_b})
        # get_rand_mod(5) = 1 -> two picks: 9 -> v[3], 5 -> v[1]; tail 2
        script = [1, 9, 5, 2]

        def expect(image):
            self.assertEqual(line(image, 3), pre_a + text[(0x8015EFE0, 1)])
            self.assertEqual(line(image, 4), pre_a + text[(0x8015EFE0, 3)])
            self.assertEqual(line(image, 5), pre_b + text[(0x8015EFF0, 2)])
            self.assertEqual(struct.unpack_from('<i', image, 0x8015ED14 - RAM)[0], 6)
        self.check('msg', 'RPG_BAT/rpg_bat_msg_build.c', 'rpg_bat_msg_build',
                   'rpg_bat_msg_build()', script, ram, expect)

    # ---- TACO -------------------------------------------------------------
    def test_taco_node_pose_set(self):
        poses = 0x8012F4F0
        ram = bytearray(RAM_SIZE)
        for i in range(64):             # the taco_actor_alloc image
            struct.pack_into('<IIii', ram, 0x8012F0F0 + 16 * i - RAM,
                             0x80000000, poses + 80 * i, 0x11111111 * (i & 7), -1)
            w32(ram, poses + 80 * i, 0xA5A5A5A5)
        pa = 0x80172000
        pb = 0x80172100
        a = [7 * k - 20 for k in range(12)]
        b = [3 * k + 1 for k in range(36)]
        ram[pa - RAM:pa - RAM + 24] = struct.pack('<12h', *a)
        ram[pb - RAM:pb - RAM + 72] = struct.pack('<36h', *b)
        idx, n = 5, 2
        call = ('node_pose_set(%d, %d, (short *)0x%08XUL, (short *)0x%08XUL, 0, 0)'
                % (idx, n, pa, pb))

        def expect(image):
            rec = poses + 80 * idx - RAM
            flag, = struct.unpack_from('<i', image, rec)
            self.assertEqual(flag, 0)
            m = struct.unpack_from('<9h', image, rec + 4)
            t = struct.unpack_from('<3i', image, rec + 4 + 20)
            self.assertEqual(list(m), b[n * 9:n * 9 + 9])
            self.assertEqual(list(t), a[n * 3:n * 3 + 3])
            # nothing else in PS1 RAM changed
            changed = [o for o in range(0, RAM_SIZE, 4)
                       if image[o:o + 4] != ram[o:o + 4]]
            self.assertTrue(all(rec <= o < rec + 80 for o in changed), changed[:8])
        self.check('pose', 'TACO/node_pose_set.c', 'node_pose_set', call, [0],
                   ram, expect)

    # ---- `&t[i]` walks over 4-byte handle tables ----------------------------
    def band_ram(self, words):
        """TACO D_8015F2A0: cells idx=1 / idx+1=2 bound a packet stream (as
        rec_word_copy writes them, 4 bytes each); cells 0 and 3 hold decoy
        handles so a fused or misaligned read lands elsewhere."""
        start = 0x80173000
        ram = bytearray(RAM_SIZE)
        for i, w in enumerate(words):
            w32(ram, start + 4 * i, w)
        decoy = 0x80174000
        for i in range(8):
            w32(ram, decoy + 4 * i, 0x21010001)
        w32(ram, 0x8015F2A0, decoy)
        w32(ram, 0x8015F2A4, start)
        w32(ram, 0x8015F2A8, start + 4 * len(words))
        w32(ram, 0x8015F2AC, decoy + 32)
        return ram, start

    BAND_STUB = ('int taco_addr_check(void *p) { return 1; }\n'
                 'int func_801542EC(void *p) { return 1; }')  # matched C / PC override spelling

    def expect_words(self, ram, start, words):
        def expect(image):
            got = list(struct.unpack_from('<%dI' % len(words), image, start - RAM))
            self.assertEqual(got, words)
            changed = [o for o in range(0, RAM_SIZE, 4)
                       if image[o:o + 4] != ram[o:o + 4]]
            self.assertTrue(all(start - RAM <= o < start - RAM + 4 * len(words)
                                for o in changed), changed[:8])
        return expect

    def test_taco_band_bright(self):
        words = [0x21010001, 0x00102030, 0x12345678, 0x21010304, 0x44F0E0D0,
                 0x21010002, 0x7F000000, 0x99999999]
        ram, start = self.band_ram(words)
        ofs = 0x20
        want = list(words)
        i = 0
        while i < len(want):            # the console semantics
            if want[i] in (0x21010001, 0x21010002, 0x21010304):
                i += 1
                c = want[i]
                r = min(((c >> 16) & 0xFF) + ofs, 255)
                g = min(((c >> 8) & 0xFF) + ofs, 255)
                b = min((c & 0xFF) + ofs, 255)
                want[i] = (c & 0xFF000000) + (r << 16) + (g << 8) + b
            i += 1
        self.assertNotEqual(want, words)
        self.check('bright', 'TACO/taco_band_bright.c', 'taco_band_bright',
                   'taco_band_bright(1, %d)' % ofs, [0], ram,
                   self.expect_words(ram, start, want), extra=self.BAND_STUB)

    def test_taco_band_chan_set(self):
        words = [0x24000507, 0x11111111, 0x0BCD5678, 0x34000609, 0, 0x01F0ABCD,
                 0x55555555, 0x24000014, 0, 0xFFFFFFFF]
        ram, start = self.band_ram(words)
        a1 = 0x2B
        want = list(words)
        i = 0
        while i < len(want):
            if want[i] in (0x24000507, 0x34000609, 0x24000014):
                v = want[i + 2]
                i += 2
                want[i] = ((v & 0xFFFF) + (v & 0x01E00000) + ((a1 & 0x1F) << 16)) & 0xFFFFFFFF
            i += 1
        self.assertNotEqual(want, words)
        self.check('chan', 'TACO/taco_band_chan_set.c', 'taco_band_chan_set',
                   'taco_band_chan_set(1, %d)' % a1, [0], ram,
                   self.expect_words(ram, start, want), extra=self.BAND_STUB)

    # ---- OPTION -----------------------------------------------------------
    def test_option_menuf_main1(self):
        """The sixteen legend strings reach debug_string_show in order, at
        x=-32 and y=-96..84; the stub logs (string, x, y) into PS1 RAM."""
        log = 0x80175000
        strings = 0x80176000
        ram = bytearray(RAM_SIZE)
        text = []
        for i in range(16):
            s = ('LEGEND_%02d' % i).encode()
            ram[strings + 16 * i - RAM:strings + 16 * i - RAM + len(s) + 1] = s + b'\0'
            w32(ram, 0x8013E894 + 4 * i, strings + 16 * i)
            text.append(s)
        w32(ram, 0x8013E894 + 64, 0)    # the word after the table (retail: 0)
        extra = r"""
short cursor_cw_x, cursor_cw_y; int pad_trig; unsigned char now_icon;
unsigned char return_schedule_tbl0; unsigned short basyo_pack;
static int n;
void menu_check() {} void func_80050030() {} void menu_cursor_move() {}
void menu_bar_show() {}
void bzero(void *p, unsigned long k) { memset(p, 0, k); }
void goto_step() {} void text_sys_init() {} void class_clear() {}
void debug_string_show(char *s, int x, int y)
{
    char *d = (char *)0x%08XUL + 24 * n++;
    strncpy(d, s, 15);
    memcpy(d + 16, &x, 4);
    memcpy(d + 20, &y, 4);
}
""" % log

        def expect(image):
            for i in range(16):
                rec = image[log - RAM + 24 * i:log - RAM + 24 * i + 24]
                self.assertEqual(rec[:16].split(b'\0')[0], text[i])
                self.assertEqual(struct.unpack_from('<ii', rec, 16), (-32, -96 + 12 * i))
            self.assertEqual(image[log - RAM + 24 * 16:log - RAM + 24 * 17], bytes(24))
        self.check('legend', 'OPTION/option_menuf_main1.c', 'option_menuf_main1',
                   'option_menuf_main1()', [0], ram, expect,
                   symbols={'work_miura': 0x8012B2B0}, extra=extra)


if __name__ == '__main__':
    unittest.main()
