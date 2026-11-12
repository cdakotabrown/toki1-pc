# SPDX-License-Identifier: GPL-3.0-or-later
"""Overlay call statements survive module generation; head addresses resolve.

Two defects made overlay scenes stall on the PC:

  * generate_overlay_modules.strip_func_forward_decls deleted any line of
    the shape `<word> <callee>(args);`, so `return sub_sub_step_up();` and
    `else\\n    sub_step_up();` vanished (TACO's loader never advanced,
    DATE's per-girl dispatchers lost `default: return game_reset();`);
  * gen_overlay_symtab keyed each function by its .sym row only, so a call
    to a widened window's cross-prologue head (`@SymbolStart`, e.g. TACO
    taco_step_dispatch4 -> func_80144948 = taco_view_reset_step) resolved
    to nothing.

Checks the transforms directly (prototypes still go, statements stay; the
negative control is the pre-fix regex) and the tracked outputs (the TACO
module keeps its loader tail; the symtab maps TACO's head address).
"""
from pathlib import Path
import re
import unittest

import generate_overlay_modules as gom
import gen_overlay_symtab as gos

ROOT = Path(__file__).resolve().parents[2]

SRC = """\
extern void sub_sub_step_up(void);
s32 sub_sub_step_up();
void *func_80133980(s32 a);
s32 f(s32 x)
{
    if (x)
        func_80133980(1);
    else
        sub_sub_step_up();
    switch (x) {
    case 1: return sub_sub_step_up();
    }
    goto out;
out:
    return func_80133980(2);
}
"""


def old_strip(content, names):
    """The pre-fix rule (no statement-keyword guard): the negative control."""
    for name in names:
        pattern = (rf'^[ \t]*(?!static\b)(?:extern\s+)?[A-Za-z_][\w \t\*]*?[\s\*]+'
                   rf'{re.escape(name)}\s*\([^)]*\)\s*;[ \t]*\n?')
        content = re.sub(pattern, '', content, flags=re.MULTILINE)
    return content


class StripForwardDecls(unittest.TestCase):
    def strip(self, text):
        return gom.strip_func_forward_decls(
            text, [0x80133980, 0], {0x80133980: 'func_80133980', 0: 'sub_sub_step_up'})

    def test_prototypes_removed(self):
        out = self.strip(SRC)
        self.assertNotIn('extern void sub_sub_step_up(void);', out)
        self.assertNotIn('s32 sub_sub_step_up();', out)
        self.assertNotIn('void *func_80133980(s32 a);', out)

    def test_statements_kept(self):
        out = self.strip(SRC)
        self.assertIn('return func_80133980(2);', out)
        self.assertIn('case 1: return sub_sub_step_up();', out)
        self.assertRegex(out, r'else\s+sub_sub_step_up\(\);')
        self.assertIn('        func_80133980(1);', out)

    def test_negative_control_old_rule_drops_statements(self):
        out = old_strip(SRC, ['func_80133980', 'sub_sub_step_up'])
        self.assertNotIn('return func_80133980(2);', out)
        self.assertNotRegex(out, r'else\s+sub_sub_step_up\(\);')


class TrackedOutputs(unittest.TestCase):
    def test_taco_loader_tail_present(self):
        text = (ROOT / 'pc/src/overlay_TACO.c').read_text(errors='replace')
        body = text[text.index('s32 TACO_taco_zk_cd_load() {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('return sub_sub_step_up();', body)
        self.assertIn('return printf(', body)

    def test_symbol_start_heads_in_symtab(self):
        self.assertEqual(gos.symbol_start('TACO', 'taco_view_reset_step'), 0x80144948)
        text = (ROOT / 'pc/src/overlay_symtab.c').read_text(errors='replace')
        self.assertIn('{ 0x80144948u, (void *)TACO_taco_view_reset_step }', text)


if __name__ == '__main__':
    unittest.main()
