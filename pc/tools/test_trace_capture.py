# SPDX-License-Identifier: GPL-3.0-or-later
"""Trace capture path, sandbox and incomplete-calibration regressions."""
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import trace_compare as trace


class TraceCaptureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.script = self.directory / 'script'
        self.script.write_text('hold 2\n')
        self.fake = self.directory / 'fake'
        self.out = self.directory / 'capture.trace'

    def executable(self, body):
        self.fake.write_text('#!/usr/bin/env python3\n' + body)
        self.fake.chmod(0o755)

    def test_diff_rejects_incompatible_observation_phase(self):
        # Reject incompatible metadata even when the sampled bytes agree.
        reference = self.directory / 'ps1.trace'
        row = 'frame=1 frame_counter=01000000 now_step=12\n'
        reference.write_text(row)
        for phase in ('pre_irq', 'post_irq', 'invalid', None):
            with self.subTest(phase=phase), redirect_stdout(io.StringIO()) as log:
                header = f'# observation={phase}\n' if phase else ''
                self.out.write_text(header + row)
                result = trace.diff(self.out, reference)
                self.assertEqual(result, 0 if phase in ('pre_irq', None) else 2)
                if phase is None:
                    self.assertIn('phase is unverified', log.getvalue())
                elif phase != 'pre_irq':
                    self.assertNotIn('IDENTICAL', log.getvalue())

    def test_input_script_byte_and_record_limits(self):
        for data in (b'#' * 4097, b'\n' * (1024 * 1024 + 1)):
            self.script.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'exceeds'):
                trace.parse_script(str(self.script))

    def test_native_relative_output_is_absolute_and_workdir_is_private(self):
        root = self.directory / 'root'
        (root / 'pc/assets').mkdir(parents=True)
        (root / 'disks').mkdir()
        original_dump = root / 'audio_dump.wav'
        original_dump.write_text('preserve me')
        work_record = self.directory / 'cwd'
        self.executable("import os\n"
                        "assert os.path.isabs(os.environ['PC_TRACE'])\n"
                        "assert os.environ['PC_TRACE_PHASE'] == 'pre_irq'\n"
                        "assert os.path.isabs(os.environ['PC_INPUT'])\n"
                        "assert os.path.isdir('pc/assets') and os.path.isdir('disks')\n"
                        f"open({str(work_record)!r}, 'w').write(os.getcwd())\n"
                        "open('audio_dump.wav', 'w').write('private')\n"
                        "open(os.environ['PC_TRACE'], 'w').write("
                        "'frame=1 frame_counter=01000000 now_step=12\\n')\n")
        with patch.object(trace, 'ROOT', str(root)), patch.object(trace, 'PC_BIN', str(self.fake)), \
                redirect_stdout(io.StringIO()):
            rows = trace.run_pc(str(self.script), 1, os.path.relpath(self.out))
        self.assertEqual(rows[0]['frame_counter'], 1)
        self.assertEqual(original_dump.read_text(), 'preserve me')
        self.assertNotEqual(work_record.read_text(), str(root))
        self.assertFalse(Path(work_record.read_text()).exists())

    def test_reference_output_and_state_are_absolute_despite_tmp_cwd(self):
        self.executable("import os,sys\n"
                        "path=sys.argv[sys.argv.index('-trace')+1]\n"
                        "assert os.path.isabs(path)\n"
                        "assert os.path.isabs(sys.argv[sys.argv.index('-loadstate')+1])\n"
                        "assert os.path.isabs(sys.argv[-1])\n"
                        "open(path,'w').write('frame=1 frame_counter=01000000\\n')\n")
        with patch.object(trace, 'REGTEST', str(self.fake)), patch.object(trace, 'DISC', 'disc.cue'):
            rows = trace.regtest(1, os.path.relpath(self.out), [], 2, state='save.state', timeout=5)
        self.assertEqual(rows[0]['frame_counter'], 1)

    def test_missing_native_trace_cannot_reuse_a_stale_capture(self):
        self.executable('pass\n')
        self.out.write_text('frame=1 frame_counter=01000000\n')
        with patch.object(trace, 'PC_BIN', str(self.fake)):
            with self.assertRaisesRegex(RuntimeError, 'no trace records'):
                trace.run_pc(str(self.script), 1, str(self.out), timeout=5)
        self.assertFalse(self.out.exists())

    def test_short_native_trace_is_not_a_complete_capture(self):
        self.executable("import os\n"
                        "open(os.environ['PC_TRACE'], 'w').write("
                        "'frame=1 frame_counter=01000000\\n')\n")
        with patch.object(trace, 'PC_BIN', str(self.fake)):
            with self.assertRaisesRegex(RuntimeError, 'only 1/2 game frames'):
                trace.run_pc(str(self.script), 2, str(self.out), timeout=5)

    def test_missing_reference_trace_does_not_expand_host_budget(self):
        with patch.object(trace, 'regtest', return_value=[]) as capture:
            with self.assertRaisesRegex(RuntimeError, 'no game-frame records'):
                trace.run_ps1(str(self.script), 10, str(self.out))
        self.assertEqual(capture.call_count, 1)

    def test_unreachable_input_and_short_capture_fail_closed(self):
        rows = [{'frame': 1, 'frame_counter': 0}]
        with patch.object(trace, 'regtest', return_value=rows), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'only 0/10 game frames'):
                trace.run_ps1(str(self.script), 10, str(self.out), boot_slack=30000)
            self.script.write_text('hold 2\n1:cross\n')
            with self.assertRaisesRegex(RuntimeError, 'input was not applied'):
                trace.run_ps1(str(self.script), 10, str(self.out), boot_slack=30000)


if __name__ == '__main__':
    unittest.main()
