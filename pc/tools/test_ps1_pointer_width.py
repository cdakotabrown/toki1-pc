# SPDX-License-Identifier: GPL-3.0-or-later
"""PS1 pointer cells written at their console width (4 bytes).

The matched writers below declare 4-byte PS1 cells as native pointers
(`u32 *`, `void *`, `u8 *`); on LP64 every store is 8 bytes and its high
half (0: PS1 addresses fit in 32 bits) lands on the next PS1 word
(pc/tools/audit_ps1_wide_access.py).  The src/main/pc-port overrides store
u32.

Each case compiles the function with the flags pc/Makefile gives its
object (the matched store order depends on them), links it the way
pc/Makefile links the port
(--just-symbols=pc/ld/psx_globals.syms, a 2 MB PS1 RAM at 0x80000000
built from pc/assets/raw/PSX.EXE), with its callees as no-ops, fills each
pointer cell and the word after it with a sentinel, calls it and reads
the PS1 RAM back.  The oracle is written from the source: a pointer cell
holds its target's PS1 address; the word after it holds what the function
assigns there, or the sentinel where it assigns nothing.

  fix:              the override gives exactly the oracle;
  negative control: the matched source, compiled and linked the same
                    way, leaves 0 in (bytes of) the words MATCHED_LOSES
                    lists.  telephone_choice_menu_init, konami_mark_set
                    and serifu_run_end re-store every word they wipe in
                    the build's store order; for them
                    test_matched_store_is_wide is the control: the matched
                    object makes 8-byte stores to the cells, the override
                    none (checked for every case).
"""
from pathlib import Path
import os
import struct
import tempfile
import unittest

from safe_process import run
from psx_test_ram import PSX_EXE, retail_ram

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp, spell  # noqa: E402
CFLAGS = ['-std=gnu17', '-fpermissive', '-w', '-funsigned-char',
          '-fno-strict-aliasing', '-fno-aggressive-loop-optimizations', '-O2',
          '-D_GNU_SOURCE', '-DPC_PORT',
          '-include', 'pc/include/game/types.h',
          '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include')]
RAM = 0x80000000
SENTINEL = 0xA5A55A5A


def syms():
    out = {}
    for line in (ROOT / 'pc/ld/psx_globals.syms').read_text().splitlines():
        p = line.split()
        if len(p) == 3 and p[1] == '=':
            out[p[0]] = int(p[2].rstrip(';'), 16)
    return out


S = syms()


def tbl3(ram):
    """basyo_init_tbl3 (s16), as the slot's aux14 halfword."""
    return struct.unpack_from('<H', ram, S['basyo_init_tbl3'] - RAM)[0]


def word(name):
    return lambda ram: struct.unpack_from('<I', ram, S[name] - RAM)[0]


# function -> dict(
#   call: the C call, stubs: callees made no-ops,
#   ptr: {cell: target symbol}: the cell holds the target's PS1 address,
#   succ: {word: value}: the console's value of each word after a pointer
#         cell that is not itself a pointer cell (a callable of the RAM
#         image for loaded values; SENTINEL where nothing is assigned))
CASES = {
    'cal_class_init': dict(
        call='cal_class_init()', stubs=['class_clear_n', 'schedule_mark_on'],
        ptr={'cal_cw_img': 'basyo_init_tbl1', 'cal_cw_anim_tbl': 'basyo_init_tbl2',
             'cal_cw_hdr': '_sys_default_tpage_set_tbl2'},
        # aux14 (s16) = basyo_init_tbl3 and anim (+2) unassigned; attr
        succ={'cal_cw_aux14': lambda ram: (SENTINEL & 0xFFFF0000) | tbl3(ram),
              'cal_cw_attr': 0x40000000}),
    'telephone_choice_menu_init': dict(
        call='telephone_choice_menu_init()',
        stubs=['class_clear_n', 'set_kanji_string', 'menu_set'],
        ptr={'cal_cw_img': 'basyo_init_tbl1', 'cal_cw_anim_tbl': 'basyo_init_tbl2',
             'cal_cw_hdr': '_sys_default_tpage_set_tbl2'},
        # aux14 = basyo_init_tbl3, anim = 0; attr
        succ={'cal_cw_aux14': tbl3, 'cal_cw_attr': 0x40000000}),
    'yazirushi_disp_switch': dict(
        call='yazirushi_disp_switch(1)', stubs=['class_clear_n'],
        ptr={'yazirushi_cw_img': 'basyo_init_tbl1',
             'yazirushi_cw_anim_tbl': 'basyo_init_tbl2',
             'yazirushi_cw_hdr': '_sys_default_tpage_set_tbl2'},
        # +20 of class_w slot 2 is not assigned; attr
        succ={'0x8012724C': SENTINEL, 'yazirushi_cw_attr': 0x40000000}),
    'konami_mark_set': dict(
        call='konami_mark_set(1)', stubs=['class_clear_n'],
        ptr={'konami_mark_cw_img': 'basyo_init_tbl1',
             'konami_mark_cw_anim_tbl': 'basyo_init_tbl2',
             'konami_mark_cw_hdr': '_sys_default_tpage_set_tbl2'},
        # aux14 = basyo_init_tbl3, anim = 0; attr = 0
        succ={'konami_mark_cw_aux14': tbl3, 'konami_mark_cw_attr': 0}),
    'serifu_run_end': dict(
        call='serifu_run_end()', stubs=['serifu_main'],
        # ss_serifu is 0 in the RAM image: a non-zero value would show a
        # loss of ss_s (the build's store order re-stores it)
        poke={'ss_serifu': 0x00010203},
        ptr={'now_s': 'now_serifu', 'now_s_in': 'now_serifu_in'},
        succ={'ss_s': word('ss_serifu')}),
    'graph_init': dict(
        call='graph_init()',
        stubs=['GsInitGraph', 'GsSetDrawBuffOffset', 'GsDefDispBuff', 'GsInit3D',
               'InitGeom', 'GsInitVcount', 'GsGetActiveBuff', 'draw_and_disp_env_init'],
        ptr={'ot_work0_org': 'ot_buff0', 'ot_work1_org': 'ot_buff1'},
        # the GsOT `offset` word after each org is not assigned
        succ={'0x800F1158': SENTINEL, '0x800F116C': SENTINEL}),
}

# words the matched 8-byte stores leave (partly) 0 (the negative control),
# for the store order the build's flags give.  In the game all of them are
# latent: class_clear_n zeroes the class_w slot first (cal_cw_anim, slot 2
# +20) and the GsOT `offset` words are 0 at boot -- the stores wipe words
# that are 0 already, until a store order or a caller changes.
# Since class_w goes through pc/include/game/ps1_alias_refs.h (2026-10-31)
# GCC keeps the matched stores in source order, and the 8-byte stores wipe
# one more class_w word each in three functions.
MATCHED_LOSES = {
    'cal_class_init': ['0x80128b44', '0x80128b68'],   # cal_cw_anim (upper half), +0x24
    'telephone_choice_menu_init': ['0x80128b68'],
    'yazirushi_disp_switch': ['0x8012724c', '0x80127270'],   # slot 2 +20, +0x24
    'konami_mark_set': [],                     # every wiped word re-stored
    'serifu_run_end': [],                      # ss_s stored after now_s_in
    'graph_init': ['0x800f1158', '0x800f116c'],  # GsOT offset words
}

HARNESS = r"""
#include <stdio.h>
#include <sys/mman.h>
STUBS
int main(int argc, char **argv)
{
    FILE *f;
    unsigned int i, *w;
    static const unsigned int cells[] = { CELLS 0 };
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    f = fopen(argv[1], "rb");
    if (!f || fread(ram, 1, 0x200000, f) != 0x200000) return 3;
    fclose(f);
    for (i = 0; cells[i]; i++) {
        w = (unsigned int *)(unsigned long)cells[i];
        *w = SENTINEL;
    }
    CALL;
    f = fopen(argv[2], "wb");
    fwrite(ram, 1, 0x200000, f);
    fclose(f);
    return 0;
}
"""


def sh(cmd):
    r = run([str(c) for c in cmd], cwd=ROOT, timeout=120)
    if not r.ok:
        raise RuntimeError('%s\n%s' % (' '.join(map(str, cmd)), r.stdout[-3000:]))
    return r


def addr(name):
    return int(name, 16) if name.startswith('0x') else S[name]


def cells_of(case):
    return sorted({addr(c) for c in list(case['ptr']) + list(case['succ'])})


def seeded(func, ram):
    """The RAM image with the case's pokes applied."""
    ram = bytearray(ram)
    for name, v in CASES[func].get('poke', {}).items():
        struct.pack_into('<I', ram, S[name] - RAM, v)
    return bytes(ram)


_FLAGS = {}


def build_flags(func):
    """The compile flags pc/Makefile gives the function's object (the store
    order of the matched C depends on them), or CFLAGS without a Makefile
    answer."""
    if func not in _FLAGS:
        import audit_ps1_wide_access as wide
        argv = wide.compile_command('pc/build', func + '.o')
        flags = None
        if argv:
            flags, skip = [], False
            for a in argv[1:]:
                if skip:
                    skip = False
                elif a in ('-MF', '-MT', '-MQ', '-o'):
                    skip = True
                elif a not in ('-MMD', '-MD', '-MP', '-c') and not a.endswith('.c'):
                    flags.append(a)
        _FLAGS[func] = flags or CFLAGS
    return _FLAGS[func]


# pc/Makefile forces pc/include/game/ps1_alias_refs.h into game objects and
# pc-port overrides: their accesses to PS1 globals with a second C name call
# ps1_ref_<name>() (pc/src/ps1_alias_refs.c), whose aliases the two linker
# scripts define.  A harness links them as the port does.
ALIAS_LINK = [ROOT / 'pc/src/ps1_alias_refs.c', ROOT / 'pc/src/missing_aliases.ld',
              ROOT / 'pc/src/layout_aliases.ld']

GAME_TU_FLAGS = ['-include', 'pc/include/game/sony_bios_alias.h',
                 '-fno-tree-vectorize', '-fno-tree-loop-distribute-patterns']


def compile_obj(func, source, obj):
    """Compile `source` as pc/Makefile compiles the function's object; a
    matched source (the negative control, out of the link once the
    override exists) gets the game-TU flags pc/Makefile gives GAME_OBJS."""
    flags = list(build_flags(func))
    if str(source).startswith('src/main/matched/') and GAME_TU_FLAGS[1] not in flags:
        flags += GAME_TU_FLAGS
    sh(['gcc'] + flags + ['-c', ROOT / spell(source), '-o', obj])


def run_case(func, source, ram):
    case = CASES[func]
    cells = cells_of(case)
    ram = seeded(func, ram)
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        obj = tmp / 'f.o'
        compile_obj(func, source, obj)
        stubs = ''.join('void %s() {}\n' % s for s in case['stubs'])
        stubs = stubs.replace('void GsGetActiveBuff() {}',
                              'int GsGetActiveBuff() { return 0; }')
        stubs += 'void %s();\n' % func
        harness = tmp / 'main.c'
        harness.write_text(HARNESS.replace('STUBS', stubs)
                           .replace('CELLS', ''.join('0x%Xu, ' % c for c in cells))
                           .replace('SENTINEL', '0x%Xu' % SENTINEL)
                           .replace('CALL', case['call']))
        exe = tmp / 'run'
        sh(['gcc', '-std=gnu17', '-O0', '-w', '-no-pie', '-o', exe, harness, obj] + ALIAS_LINK + [
            '-Wl,--allow-multiple-definition',
            '-Wl,--just-symbols=%s' % (ROOT / 'pc/ld/psx_globals.syms')])
        image_in, image_out = tmp / 'ram.in', tmp / 'ram.out'
        image_in.write_bytes(ram)
        sh([exe, image_in, image_out])
        out = image_out.read_bytes()
    return {c: struct.unpack_from('<I', out, c - RAM)[0] for c in cells}


def expected(func, ram):
    """The console's result for every cell read back."""
    case = CASES[func]
    want = {}
    for c, target in case['ptr'].items():
        want[addr(c)] = addr(target)
    for c, v in case['succ'].items():
        want[addr(c)] = v(seeded(func, ram)) if callable(v) else v
    return want


def hexd(d):
    return {hex(k): hex(v) for k, v in d.items()}


@unittest.skipUnless(PSX_EXE.is_file(), 'needs pc/assets/raw/PSX.EXE')
class PointerWidth(unittest.TestCase):
    RAM_IMAGE = retail_ram() if PSX_EXE.is_file() else b''

    def test_override_stores_ps1_width(self):
        for func in CASES:
            with self.subTest(func=func):
                got = run_case(func, 'src/main/pc-port/%s.c' % func, self.RAM_IMAGE)
                self.assertEqual(hexd(got), hexd(expected(func, self.RAM_IMAGE)))

    def test_matched_loses_neighbours(self):
        """Negative control: the matched sources, linked the same way."""
        lost = {}
        for func in CASES:
            got = run_case(func, 'src/main/matched/%s.c' % func, self.RAM_IMAGE)
            want = expected(func, self.RAM_IMAGE)
            lost[func] = sorted(hex(c) for c in want if got[c] != want[c])
            for c in want:
                if got[c] != want[c]:
                    # the high half of a pointer store is 0: the lost
                    # word reads back with bytes of `want` turned to 0
                    self.assertEqual(got[c] & want[c], got[c], hex(c))
        self.assertEqual(lost, MATCHED_LOSES)

    def test_matched_store_is_wide(self):
        """Negative control for every case: the matched object makes 8-byte
        stores to the pointer cells and the override makes none."""
        import audit_ps1_wide_access as wide
        starts = sorted(set(S.values()))
        for func, case in CASES.items():
            for tier, wide_expected in (('matched', True), ('pc-port', False)):
                with self.subTest(func=func, tier=tier):
                    with tempfile.TemporaryDirectory() as d:
                        obj = os.path.join(d, 'f.o')
                        compile_obj(func, 'src/main/%s/%s.c' % (tier, func), obj)
                        rows = wide.scan_object(obj, set(case['ptr']), S, starts)
                    stores = {r['name'] for r in rows if r['cls'] == 'WIDE-STORE'}
                    self.assertEqual(stores, set(case['ptr']) if wide_expected else set())


if __name__ == '__main__':
    unittest.main()
