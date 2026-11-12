# SPDX-License-Identifier: GPL-3.0-or-later
"""Unified modules retain shared PC-only headers once, in stable order."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import generate_overlay_modules as generator


class OverrideHeaderTests(unittest.TestCase):
    def test_shared_headers_are_hoisted_only_from_pc_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            retail = root / 'retail/TT'
            overrides = root / 'overrides/TT'
            retail.mkdir(parents=True)
            overrides.mkdir(parents=True)
            for name in ('tt_text_disp', 'tt_recs_disp', 'tt_board_disp'):
                (retail / f'{name}.c').write_text(
                    f'#include "retail_only.h"\nvoid {name}(void) {{}}\n')
            for name in ('tt_text_disp', 'tt_recs_disp'):
                (overrides / f'{name}.c').write_text(
                    '#include "types.h"\n#include "z_layout.h"\n'
                    '#include "a_layout.h"\n' + f'void {name}(void) {{}}\n')
            with patch.object(generator, 'PC_OVERLAY_OVERRIDE', overrides.parent):
                with redirect_stdout(StringIO()):
                    output, count = generator.process_overlay('TT', retail)
            self.assertEqual(count, 3)
            self.assertEqual(output.count('#include "types.h"'), 1)
            self.assertEqual(output.count('#include "a_layout.h"'), 1)
            self.assertEqual(output.count('#include "z_layout.h"'), 1)
            self.assertNotIn('retail_only.h', output)
            self.assertLess(output.index('#include "a_layout.h"'),
                            output.index('#include "z_layout.h"'))
            self.assertLess(output.index('#include "z_layout.h"'),
                            output.index('TT_tt_text_disp'))


if __name__ == '__main__':
    unittest.main()
