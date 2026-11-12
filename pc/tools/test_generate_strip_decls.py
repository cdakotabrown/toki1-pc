# SPDX-License-Identifier: GPL-3.0-or-later
"""generate_overlay_modules.strip_func_forward_decls keeps statements.

`return f();` and `else f();` (the call on the next line, too) have the
shape `<word> name();` that the forward-declaration pattern removes.  With
`return`/`else` taken for a return type the statements were deleted from
the unified modules: 276 restored statements in 20 overlays, among them DATE's
date_event_sched closing `return sub_sub_step_up();` (the date never left
sub-sub-step 15) and many `if (...) a(); else b();` second arms.

  fix:              declarations go, the statements stay;
  negative control: GENERATOR_SOURCE=<the previous generator> deletes them.
"""
import importlib.util
import os
import unittest

SOURCE = '''extern void sub_sub_step_up(void);
extern s32 func_80132020(s32);
s32 func_80132020(s32);
s32 f(s32 a)
{
    if (a)
        func_80132020(1);
    else
        func_80132020(2);
    if (a > 3) return func_80132020(3);
    return sub_sub_step_up();
}
'''


def load(path):
    spec = importlib.util.spec_from_file_location('gen_under_test', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def strip(mod):
    # sub_sub_step_up is a landed main-binary name (FUNC_NAME); func_80132020
    # is an overlay-local function
    return mod.strip_func_forward_decls(SOURCE, [0x80132020], {0x80132020: 'func_80132020'})


HERE = os.path.dirname(os.path.abspath(__file__))


class StripDecls(unittest.TestCase):
    def check(self, out):
        self.assertNotIn('extern void sub_sub_step_up(void);', out)
        self.assertNotIn('s32 func_80132020(s32);', out)
        self.assertIn('return sub_sub_step_up();', out)
        self.assertIn('return func_80132020(3);', out)
        self.assertIn('else\n        func_80132020(2);', out)

    def test_statements_survive(self):
        self.check(strip(load(os.path.join(HERE, 'generate_overlay_modules.py'))))

    def test_negative_control_previous_generator(self):
        old = os.environ.get('GENERATOR_SOURCE')
        if not old:
            self.skipTest('set GENERATOR_SOURCE=<previous generate_overlay_modules.py>')
        with self.assertRaises(AssertionError):
            self.check(strip(load(old)))


if __name__ == '__main__':
    unittest.main()
