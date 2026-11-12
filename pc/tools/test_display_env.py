# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for the GTE entry formerly mistaken for GPU scroll."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class DisplayEnvironmentTests(unittest.TestCase):
    def test_geometry_alias_and_signed_offsets(self):
        # Include the real implementation to inspect private OFX/OFY state;
        # no assets, SDL window, copied retail code or image oracle needed.
        source = r'''
#include "pc/hal/gte.c"
int main(void) {
    InitGeom();
    SetGeomOffset(160, 120);
    if (gte.ofx != 10485760 || gte.ofy != 7864320) return 1;
    SetGeomOffset(-17, -23);
    if (gte.ofx != -1114112 || gte.ofy != -1507328) return 2;
    SetGeomOffset(65536, -65536);
    if (gte.ofx != 0 || gte.ofy != 0) return 3;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            fixture = directory / 'fixture.c'
            fixture.write_text(source)
            binary = directory / 'fixture'
            run(['cc', '-std=c89', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                 '-I.', '-Ipc/include', '-Ipc/include/game', str(fixture),
                 'pc/tests/gte_test_tables.c',
                 '-lm', '-o', str(binary)],
                cwd=ROOT, timeout=30).require_ok()
            run([str(binary)], cwd=directory, timeout=10).require_ok()

    def test_matched_setup_is_not_shadowed_or_overwritten(self):
        makefile = (ROOT / 'pc/Makefile').read_text()
        excludes = []
        lines = iter(makefile.splitlines())
        for line in lines:
            if line.startswith(('HAL_EXCLUDES =', 'HAL_EXCLUDES +=')):
                definition = line
                while definition.rstrip().endswith('\\'):
                    definition = definition.rstrip()[:-1] + ' ' + next(lines)
                excludes.extend(definition.split('=', 1)[1].split())
        for name in ('GsInitGraph', 'GsDefDispBuff', 'GsInit3D'):
            self.assertNotIn(name, excludes)
        self.assertNotIn('hal_display_apply(', (ROOT / 'pc/src/main.c').read_text())


if __name__ == '__main__':
    unittest.main()
