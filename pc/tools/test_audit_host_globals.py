# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_host_globals.py classifies host-backed PS1 symbols correctly.

A synthetic program is linked the way pc/Makefile links the port (objects,
an alias linker script, --just-symbols for the PS1 addresses), with one
host definition per verdict, and the audit must name each one:

  D_80100000_o  4-byte host u32 written as an 8-byte record   OVERFLOW
  D_80100010_e  4-byte host u32 whose address is stored       ESCAPES
  D_80100020_s  read in bounds; D_80100020 also referenced    SHADOWS
  D_80100030_h  read in bounds, nothing else at its address   HOST-ONLY
  D_80100040_u  defined, never referenced                     UNUSED
  D_80100050_p  PROVIDE(= D_80100050): the fix form           (not listed)

Negative control: the same program with D_80100050_p DEFINED in C (the
defect ad25a133bc removed; other code reads D_80100050) lists it.
"""
from pathlib import Path
import tempfile
import unittest

from audit_host_globals import audit
from safe_process import run

LIB = r"""
typedef unsigned int u32;
typedef struct { u32 a, b; } Pair;
u32 D_80100000_o;
u32 D_80100010_e;
u32 D_80100020_s;
u32 D_80100030_h;
u32 D_80100040_u;
DEFINE_P
"""

USE = r"""
typedef unsigned int u32;
typedef struct { u32 a, b; } Pair;
extern Pair D_80100000_o;
extern u32 D_80100010_e;
extern u32 D_80100020_s;
extern u32 D_80100020;
extern u32 D_80100030_h;
extern u32 D_80100050_p;
extern u32 D_80100050;
u32 *volatile sink;
u32 use(void)
{
    D_80100000_o.b = 7;
    sink = &D_80100010_e;
    return D_80100020_s + D_80100020 + D_80100030_h + D_80100050_p
        + D_80100050;
}
"""

MAIN = r"""
#include <sys/mman.h>
extern unsigned int use(void);
int main(void) { return 0 && use(); }
"""

SYMS = """D_80100000 = 0x80100000;
D_80100010 = 0x80100010;
D_80100020 = 0x80100020;
D_80100030 = 0x80100030;
D_80100040 = 0x80100040;
D_80100050 = 0x80100050;
D_80100060 = 0x80100060;
"""


def link(tmp, define_p):
    objdir = tmp / 'obj'
    objdir.mkdir()
    for name, text in (('lib', LIB.replace('DEFINE_P', 'u32 D_80100050_p;' if define_p else '')),
                       ('use', USE), ('main', MAIN)):
        src = tmp / (name + '.c')
        src.write_text(text)
        r = run(['gcc', '-O2', '-fno-pie', '-c', str(src), '-o', str(objdir / (name + '.o'))])
        assert r.ok, r.stdout
    (tmp / 'aliases.ld').write_text('PROVIDE(D_80100050_p = D_80100050);\n')
    (tmp / 'psx.syms').write_text(SYMS)
    exe = tmp / 'prog'
    r = run(['gcc', '-no-pie', '-o', str(exe)] + sorted(str(o) for o in objdir.glob('*.o'))
            + [str(tmp / 'aliases.ld'), '-Wl,--allow-multiple-definition',
               '-Wl,--just-symbols=%s' % (tmp / 'psx.syms')])
    assert r.ok, r.stdout
    return exe, objdir


class AuditHostGlobals(unittest.TestCase):
    def verdicts(self, define_p):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            exe, objdir = link(tmp, define_p)
            rows = audit(exe, objdir, tmp / 'none.exe')
            return {r['name']: r for r in rows}

    def test_verdicts(self):
        rows = self.verdicts(define_p=False)
        got = {n: r['verdict'] for n, r in rows.items()}
        self.assertEqual(got, {'D_80100000_o': 'OVERFLOW',
                               'D_80100010_e': 'ESCAPES',
                               'D_80100020_s': 'SHADOWS',
                               'D_80100030_h': 'HOST-ONLY',
                               'D_80100040_u': 'UNUSED'})
        self.assertEqual(rows['D_80100000_o']['reach'], 8)
        self.assertEqual(rows['D_80100010_e']['extent'], 16)
        self.assertEqual(rows['D_80100020_s']['other_names'], ['D_80100020'])

    def test_defined_alias_is_flagged(self):
        """Negative control: the fix form undone."""
        rows = self.verdicts(define_p=True)
        self.assertIn('D_80100050_p', rows)
        self.assertEqual(rows['D_80100050_p']['verdict'], 'SHADOWS')


if __name__ == '__main__':
    unittest.main()
