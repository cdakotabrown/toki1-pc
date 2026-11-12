# SPDX-License-Identifier: GPL-3.0-or-later
"""Overlay definitions of PS1 objects are the PS1 object (ps1_resident_defs).

ETC etc_main1 defines `CamCE D_8015015C_s = {0, 0};` and `volatile CamCE
D_8015015C_v = {0, 0};` -- two spellings of the camera pair at 0x8015015C
that the pad handler of the same function reaches as D_8015015C.  TAIIKU
and DATE define objects with a file-local type, which the generator
renames `<name>__def` and aliases.  psx_globals.syms pins D_8015015C and
the real names into PS1 RAM; the views and the `__def`s were host
variables, so a reset or an init written through one spelling was never
seen through the other.

  * executable: a module-shaped source (one file writes through the view /
    __def, another reads the PS1 name), linked with --just-symbols and a
    mapped PS1 RAM, agrees after pinning; negative control: unpinned, the
    reader sees the old value;
  * the pinning keeps non-zero initializers as written (reported);
  * the tracked overlay modules carry no host definition of an
    address-named object and no `__def` alias (regression gate).
"""
from pathlib import Path
import re
import tempfile
import unittest

from ps1_resident_defs import (externalize_pinned_bases, find_definition, pin_definition,
                               pin_view_definitions, view_address)
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402

MODULE = r"""
typedef short s16;
typedef int s32;
typedef struct { s16 c; s16 e; } CamCE;
extern s16 D_8015015C[2];
volatile CamCE D_8015015C_v = {0, 0};
CamCE D_8015015C_s = {0, 0};
typedef struct { s16 angle0, angle1, angle2, unk06; } Ang;
Ang D_8015DDB0__def[11] = {0};
extern s16 D_8015DDB0[] __attribute__((alias("D_8015DDB0__def")));

void reset(void) { D_8015015C_s.c = 0; D_8015015C_s.e = 0; }
void decay(void) { D_8015015C_v.c = D_8015015C_v.c - 4; D_8015015C_v.e = D_8015015C_v.e - 5; }
void ang_init(void) { D_8015DDB0__def[1].angle1 = 210; }
"""

MAIN = r"""
#include <sys/mman.h>
typedef short s16;
extern s16 D_8015015C[2];
extern s16 D_8015DDB0[];
extern void reset(void), decay(void), ang_init(void);
int main(void)
{
    if (mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
        != (void *)0x80000000UL) return 2;
    D_8015015C[0] = 100; D_8015015C[1] = 50;     /* the pad handler */
    decay();
    if (D_8015015C[0] != 96 || D_8015015C[1] != 45) return 10;
    reset();
    if (D_8015015C[0] != 0 || D_8015015C[1] != 0) return 11;
    ang_init();
    if (D_8015DDB0[5] != 210) return 12;          /* the date_angle_set reader */
    return 0;
}
"""


def pin_module(text):
    """What generate_overlay_modules.py does to a defining file."""
    text = re.sub(r'\nextern s16 D_8015DDB0\[\] __attribute__\(\(alias\("D_8015DDB0__def"\)\)\);', '', text)
    text, ok = pin_definition(text, 'D_8015DDB0__def', view_address('D_8015DDB0__def'))
    assert ok
    text, pinned, left = pin_view_definitions(text)
    assert pinned == ['D_8015015C_s', 'D_8015015C_v'] and not left, (pinned, left)
    return text


class Ps1ResidentDefs(unittest.TestCase):
    def run_module(self, module):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            (tmp / 'm.c').write_text(module)
            (tmp / 'main.c').write_text(MAIN)
            (tmp / 'psx.syms').write_text('D_8015015C = 0x8015015C;\nD_8015DDB0 = 0x8015DDB0;\n')
            r = run(['gcc', '-O2', '-w', '-no-pie', '-fno-pie', '-o', str(tmp / 'p'),
                     str(tmp / 'main.c'), str(tmp / 'm.c'),
                     '-Wl,--allow-multiple-definition',
                     '-Wl,--just-symbols=%s' % (tmp / 'psx.syms')])
            self.assertTrue(r.ok, r.stdout)
            return run([str(tmp / 'p')]).returncode

    def test_pinned_module_is_one_object(self):
        self.assertEqual(self.run_module(pin_module(MODULE)), 0)

    def test_unpinned_module_splits(self):
        """Negative control: the module as generated before."""
        self.assertEqual(self.run_module(MODULE), 10)

    def test_module_defining_the_base(self):
        """ETC: etc_model_init2 DEFINES D_8015015C[2] in the module whose
        etc_main1 pins the views to it.  The `.set` resolves to that local
        definition, so the views were the module's host copy while every
        reference to D_8015015C went to PS1 RAM (on Linux; on Windows all
        of them were the copy, pc/tools/pe_rebind_relocs.py).
        externalize_pinned_bases turns the definition into an extern."""
        module = pin_module(MODULE).replace('extern s16 D_8015015C[2];\n',
                                            's16 D_8015015C[2] = { 0 };\n')
        self.assertEqual(self.run_module(module), 10)          # negative control
        fixed, done, left = externalize_pinned_bases(module)
        self.assertEqual((done, left), (['D_8015015C'], []))
        self.assertEqual(self.run_module(fixed), 0)

    def test_renamed_base(self):
        """The same object after the naming track landed a real name for it
        (ETC: etc_roll_rot = 0x8015015C).  Without the landed map nothing is
        externalized (negative control: the bug the 2026-11-02 rename
        reintroduced); with it the definition becomes an extern."""
        module = 'typedef short s16;\ns16 etc_roll_rot[2] = { 0 };\n' \
                 '__asm__(".set view_v, D_8015015C");\n'
        _out, done, left = externalize_pinned_bases(module)
        self.assertEqual((done, left), ([], []))
        out, done, left = externalize_pinned_bases(module, {0x8015015C: {'etc_roll_rot'}})
        self.assertEqual((done, left), (['etc_roll_rot'], []))
        self.assertIsNone(find_definition(out, 'etc_roll_rot'))

    def test_nonzero_initializer_left(self):
        text = 'typedef int s32;\ns32 D_80150000_t[2] = { 1, 0 };\n'
        out, pinned, left = pin_view_definitions(text)
        self.assertEqual((out, pinned, left), (text, [], ['D_80150000_t']))

    def test_declarator_kept(self):
        text = 'static volatile T D_8015015C_v[3] = {0}; /* c */\n'
        out, ok = pin_definition(text, 'D_8015015C_v', 0x8015015C)
        self.assertTrue(ok)
        self.assertTrue(out.startswith('extern volatile T D_8015015C_v[3];'), out)
        self.assertIn('.set D_8015015C_v, D_8015015C', out)
        self.assertIsNone(find_definition(out, 'D_8015015C_v'))

    def test_tracked_modules(self):
        bad = []
        for path in sorted((ROOT / 'pc/src').glob('overlay_*.c')):
            text = path.read_text()
            bad += ['%s: %s alias' % (path.name, m) for m in
                    re.findall(r'alias\("(D[A-Za-z]*_8[0-9A-Fa-f]{7}__def)"\)', text)]
            _out, pinned, left = pin_view_definitions(text)
            bad += ['%s: %s defined' % (path.name, n) for n in pinned + left]
            _out, done, left = externalize_pinned_bases(text, landed_overlay_data(path.name[8:-2]))
            bad += ['%s: %s defined beside its pinned views' % (path.name, n) for n in done + left]
        self.assertEqual(bad, [])


def landed_overlay_data(ovl):
    """{address: {landed names}} for one overlay (as the generator uses)."""
    import json
    doc = json.load(open(DECOMP / 'config/symbol_names.json'))['symbols']
    out = {}
    for key, e in doc.items():
        ov, sep, addr = key.partition(':')
        if sep and ov == ovl and e.get('landed') and e.get('kind') != 'func':
            try:
                out.setdefault(int(addr, 16), set()).add(e['name'])
            except ValueError:
                pass
    return out


if __name__ == '__main__':
    unittest.main()
