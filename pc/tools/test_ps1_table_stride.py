# SPDX-License-Identifier: GPL-3.0-or-later
"""PS1 tables indexed at their console stride (4 bytes) in the overrides.

Each case compiles the matched source and the PC override with the flags
pc/Makefile gives the function's object (test_ps1_pointer_width.compile_obj)
and scans both objects for scale-8 indexed accesses to the table
(audit_ps1_wide_access.scan_object, which follows lea'd base registers):

  fix:              the override indexes the table at stride 4 (no
                    WIDE-INDEX row);
  negative control: the matched source indexes it at stride 8.
"""
import tempfile
import unittest
from pathlib import Path

import audit_ps1_wide_access as wide
import test_ps1_pointer_width as width

S = width.S

CASES = [
    # (function, table): the override's own source in src/main/pc-port
    ('holiday_screen_init', 'basyo_bg_load_tbl0'),
    ('config_screen_init', 'config_screen_init_tbl3'),
    ('holiday_club_select', 'album_info_disp_tbl1'),
    ('join_club_message', 'album_info_disp_tbl1'),
    ('magazine_main', 'magazine_main_tbl0'),
]


def index_rows(func, source, table):
    with tempfile.TemporaryDirectory() as d:
        obj = Path(d) / 'f.o'
        width.compile_obj(func, source, obj)
        addrs = {table: S[table]}
        rows = wide.scan_object(str(obj), {table}, addrs, sorted(addrs.values()))
    return [r for r in rows if r['cls'] == 'WIDE-INDEX' and r['func'] == func]


class TableStride(unittest.TestCase):
    def test_override_strides_4(self):
        for func, table in CASES:
            with self.subTest(func=func):
                self.assertEqual(index_rows(func, 'src/main/pc-port/%s.c' % func, table), [])

    def test_matched_strides_8(self):
        for func, table in CASES:
            with self.subTest(func=func):
                self.assertTrue(index_rows(func, 'src/main/matched/%s.c' % func, table))


if __name__ == '__main__':
    unittest.main()
