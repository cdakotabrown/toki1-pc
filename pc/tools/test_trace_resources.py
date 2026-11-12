# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import refemu
import trace_compare as trace


class TraceResourceTests(unittest.TestCase):
    def test_decoded_trace_memory_budget(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'trace'
            p.write_text('frame=1 frame_counter=01000000 now_step=12\n')
            with patch.object(trace.sys, 'getsizeof', return_value=128 * 1024 * 1024):
                with self.assertRaisesRegex(ValueError, '64 MiB'):
                    trace.parse_trace(p)

    def test_trace_record_limit(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'trace'
            p.write_text('frame=1 frame_counter=01000000 now_step=12\n' * 50001)
            with self.assertRaisesRegex(ValueError, '50,000'):
                trace.parse_trace(p)

    def test_trace_fields_decode_little_endian(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'trace'
            p.write_text('frame=12 pad=4000 frame_counter=02000000 now_step=12 missing=??\n')
            row = trace.parse_trace(p)[0]
            self.assertEqual(row['frame'], 12)
            self.assertEqual(row['pad'], 0x4000)
            self.assertEqual(row['frame_counter'], 2)
            self.assertIsNone(row['missing'])

    def test_reference_timeout_remains_a_text_result(self):
        # the harness is local reference-emulator tooling (pc/tools/refemu.py)
        script = refemu.DIR / 'refemu_harness.py'
        if not script.is_file():
            self.skipTest(f'local reference-emulator tooling absent ({script})')
        spec = importlib.util.spec_from_file_location('harness_resource_test', script)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            fake = p / 'regtest'
            fake.write_text('#!/usr/bin/env python3\nimport sys,time\n'
                            "print('RAM Hash: abc123',file=sys.stderr,flush=True)\n"
                            'time.sleep(30)\n')
            fake.chmod(0o755)
            disc = p / 'disc.cue'
            disc.touch()
            h = module.RefEmuHarness(regtest_path=fake, disc_path=disc)
            r = h.run(frames=100, timeout=.1)
            self.assertFalse(r.success)
            self.assertEqual(r.frames_run, 0)
            self.assertEqual(r.ram_hash, 'abc123')
            self.assertIn('TIMEOUT', r.error)
            self.assertIsInstance(r.stdout, str)


if __name__ == '__main__':
    unittest.main()
