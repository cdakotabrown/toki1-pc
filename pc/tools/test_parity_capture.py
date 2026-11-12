# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiled native capture contract tests, independent of assets and SDL."""
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class ParityCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / 'capture'
        run(['cc', '-std=c89', '-Wall', '-Wextra', '-Werror',
             '-Ipc/include/game', '-Ipc/hal', 'pc/hal/parity.c',
             'pc/tests/parity_capture_harness.c', 'pc/hal/gpu_scanout.c',
             '-o', str(cls.binary)],
            cwd=ROOT).require_ok()

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def invoke(self, directory, frames='1,2,4', count=8, **extra):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith('PC_PARITY_')}
        env.update(PC_PARITY_DIR=str(directory), PC_PARITY_FRAMES=frames)
        env.update(extra)
        return run([str(self.binary), str(count)], env=env, timeout=10)

    @staticmethod
    def records(directory):
        return [json.loads(line) for line in
                (directory / 'state.jsonl').read_text().splitlines()]

    def test_disabled_default_and_no_anchor(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('PC_PARITY_')}
            run([str(self.binary)], env=env, cwd=tmp).require_ok()
            self.assertEqual(list(Path(tmp).iterdir()), [])
            self.invoke(Path(tmp), NO_ANCHOR='1').require_ok()
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_bounds_and_strict_parsing(self):
        invalid = ['', '0', '-1', '1801', '1,1', '2,1', '1,', ',1',
                   '1,x', '1.0', ' 1', '1, 2', '9' * 100,
                   ','.join(map(str, range(1, 18)))]
        with tempfile.TemporaryDirectory() as tmp:
            for spec in invalid:
                with self.subTest(spec=spec):
                    self.assertEqual(self.invoke(Path(tmp), spec).returncode, 2)
            result = self.invoke(Path('relative'), '1')
            self.assertEqual(result.returncode, 2)

    def test_image_state_audio_and_exit_flush(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.invoke(directory).require_ok()
            records = self.records(directory)
            self.assertEqual([row['frame'] for row in records], [1, 2, 4])
            for row in records:
                frame = row['frame']
                samples = frame * 566204 // 768
                self.assertEqual(row['audio_samples'], samples)
                self.assertEqual(row['audio_bytes'], samples * 4)
                self.assertEqual(row['frame_counter'], 1003 + frame)
                self.assertEqual(row['vblank'], 3 + frame)
                self.assertEqual(row['now_step'], 15)
                self.assertEqual(row['crop'], [32, 256, 320, 240])
                image = (directory / row['image']).read_bytes()
                header = b'P6\n320 240\n255\n'
                self.assertTrue(image.startswith(header))
                self.assertEqual(len(image), len(header) + 320 * 240 * 3)
                self.assertEqual(image[len(header):len(header) + 9],
                                 bytes([248, 0, 0, 0, 248, 0, 0, 0, 248]))
                vram = (directory / row['vram']).read_bytes()
                self.assertEqual(len(vram), 1024 * 512 * 2)
                self.assertEqual(struct.unpack_from('<H', vram,
                                                    (256 * 1024 + 32) * 2)[0], 31)
            pcm = (directory / 'audio.s16le').read_bytes()
            self.assertEqual(len(pcm), 4 * (4 * 566204 // 768))
            # Boot synthesis advanced deterministically, anchor reset only timebase.
            self.assertEqual(struct.unpack_from('<hh', pcm), (2211, -2211))
            self.assertEqual(len(list(directory.glob('*.ppm'))), 3)

    def test_repeatability_reset_and_small_dimensions(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            a, b = Path(left), Path(right)
            options = dict(RESET_ANCHOR='1', SMALL_VIEWPORT='1')
            self.invoke(a, **options).require_ok()
            self.invoke(b, **options).require_ok()
            self.assertEqual({p.name: p.read_bytes() for p in a.iterdir()},
                             {p.name: p.read_bytes() for p in b.iterdir()})
            rows = self.records(a)
            self.assertEqual([r['frame_counter'] for r in rows], [1005, 1006, 1008])
            self.assertEqual(rows[0]['audio_samples'], 737)
            self.assertEqual(rows[0]['crop'], [32, 256, 16, 8])

    def test_maximum_budget_under_32_mib(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            points = list(range(120, 1801, 120))
            points.insert(0, 1)
            self.invoke(directory, ','.join(map(str, points)), count=1900).require_ok()
            rows = self.records(directory)
            self.assertEqual(len(rows), 16)
            self.assertEqual(rows[-1]['audio_samples'], 1800 * 566204 // 768)
            self.assertLess(sum(p.stat().st_size for p in directory.iterdir()),
                            32 * 1024 * 1024)
            self.assertEqual((directory / 'audio.s16le').stat().st_size,
                             rows[-1]['audio_bytes'])

    def test_integration_uses_paused_device_and_pre_exit_boundary(self):
        source = (ROOT / 'pc/hal/spu.c').read_text()
        # the device opened at init (the FMV close / reopen went with
        # str_player.c, STATE-MACHINES.md C14): never unpaused under parity
        self.assertEqual(source.count('if (!parity_enabled()) SDL_PauseAudioDevice'), 1)
        self.assertIn('if (!parity_enabled()) g_wav_file', source)
        trace = (ROOT / 'pc/hal/trace.c').read_text()
        callback = trace[trace.index('void trace_vblank(void)'):]
        self.assertLess(callback.index('apply_pokes();'), callback.index('parity_vblank();'))
        self.assertLess(callback.index('parity_vblank();'), callback.index('_exit(0);'))
        self.assertIn('parity_anchor();', trace)


if __name__ == '__main__':
    unittest.main()
