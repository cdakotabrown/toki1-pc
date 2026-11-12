# SPDX-License-Identifier: GPL-3.0-or-later
"""Absorbed overlay heads resolve to their host, not to a `return 0` stub.

An overlay window widened over a hoisted head keeps the labelled entry in
config/overlay/<OV>.sym and the true start only in the C header's
`@SymbolStart`; callers keep the head address (date_main0 stores
func_80135F0C in its step table).  gen_overlay_symtab.py maps that address
to the host since 2026-10-31; before, gen_missing_funcs.py stubbed it.
"""
from pathlib import Path
import re
import unittest

import gen_overlay_symtab as gen

ROOT = Path(__file__).resolve().parents[2]


class OverlayHeadTests(unittest.TestCase):
    def test_symbol_start_directive_only(self):
        self.assertEqual(gen.symbol_start('DATE', 'date_parade_step'), 0x80135F0C)
        # prose that quotes another file's directive is not a directive
        self.assertIsNone(gen.symbol_start('RPG_BAT', 'rpg_bat_cam_set'))
        self.assertIsNone(gen.symbol_start('DATE', 'no_such_function'))

    def test_generated_table_and_no_stub(self):
        symtab = (ROOT / 'pc/src/overlay_symtab.c').read_text()
        for addr, native in ((0x80135F0C, 'DATE_date_parade_step'),
                             (0x8013B5F8, 'TAIIKU_taiiku_tama_girl1_event_step'),
                             (0x80133F08, 'KANGEI_kangei_serifu_run')):
            self.assertIn('{ 0x%08Xu, (void *)%s }' % (addr, native), symtab)
            self.assertIn('func_%08X:\\n movl $0x%08X' % (addr, addr), symtab)
        stubs = (ROOT / 'pc/src/missing_funcs_gen.c').read_text() + \
            (ROOT / 'pc/src/pc_port_stubs.c').read_text()
        for name in ('func_80135F0C', 'func_8013B5F8', 'func_80133F08'):
            self.assertIsNone(re.search(r'\b%s\(\) \{ return 0; \}' % name, stubs))


if __name__ == '__main__':
    unittest.main()
