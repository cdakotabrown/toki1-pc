# SPDX-License-Identifier: GPL-3.0-or-later
"""sync_symbol_names: comments, file-name lists and forwarders are not renamed,
and a tree already in sync is a no-op."""
import subprocess
import sys
import unittest

import sync_symbol_names as S

FWD = {'FUNC_80042D48': 'pad_type_check', 'FUNC_80053364': 'grp_table_calc',
       'D_800BA260': 'XAsector'}


class ApplyToTextTests(unittest.TestCase):
    def test_code_is_renamed(self):
        out, hits = S.apply_to_text('extern s32 D_800BA260[4];\n', FWD)
        self.assertEqual(out, 'extern s32 XAsector[4];\n')
        self.assertEqual(len(hits), 1)

    def test_comments_are_kept(self):
        src = '/* func_80042D48->pad_type_check */\n// D_800BA260 here\nx = D_800BA260;\n'
        out, _ = S.apply_to_text(src, FWD)
        self.assertEqual(out, '/* func_80042D48->pad_type_check */\n// D_800BA260 here\nx = XAsector;\n')

    def test_makefile_comment_lines_kept(self):
        src = '# func_80042D48->pad_type_check\nL = func_80053364\n'
        out, _ = S.apply_to_text(src, FWD, is_code=False)
        self.assertEqual(out, '# func_80042D48->pad_type_check\nL = grp_table_calc\n')

    def test_file_names_are_kept(self):
        out, _ = S.apply_to_text('SEMI = func_80042D48 func_80053364\n', FWD,
                                 is_code=False, keep={'func_80042D48'})
        self.assertEqual(out, 'SEMI = func_80042D48 grp_table_calc\n')

    def test_forwarder_does_not_call_itself(self):
        src = 'void func_80053364(u8 *o) { grp_table_calc(o); }\n'
        out, _ = S.apply_to_text(src, FWD)
        self.assertEqual(out, src)

    def test_idempotent(self):
        once, _ = S.apply_to_text('x = D_800BA260;\n', FWD)
        twice, hits = S.apply_to_text(once, FWD)
        self.assertEqual(once, twice)
        self.assertFalse(hits)


class TreeTests(unittest.TestCase):
    def test_generated_and_address_definition_files_skipped(self):
        files = set(S.pc_files())
        for f in ('pc/src/ps1_alias_refs.c', 'pc/include/game/ps1_alias_refs.h',
                  'pc/hal/gte_address_entries.c'):
            self.assertNotIn(f, files)

    def test_dry_run_on_synced_tree_is_noop(self):
        r = subprocess.run([sys.executable, S.__file__, '--dry-run'],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('0 files, 0 token renames, 0 file moves', r.stdout)


if __name__ == '__main__':
    unittest.main()
