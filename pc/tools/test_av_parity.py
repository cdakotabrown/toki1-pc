# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic oracle tests: a mismatch must never become a parity pass."""
import json
import re
import shutil
import wave
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

import av_parity
from parity_metrics import audio_arrays, pcm, records, viewport, visual, window_audio
from safe_process import run

import refemu

# the reference instrumentation is local reference-emulator tooling
# (pc/tools/refemu.py): its two tests skip when it is absent
try:
    _bpr = refemu.local_module('build_parity_reference')
except refemu.RefEmuUnavailable as _e:
    _bpr, NO_REFEMU = None, str(_e)
else:
    NO_REFEMU = None


class ParityMetricsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for side in ('native', 'reference'):
            (self.root / side).mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def image(self, side, words, crop=(0, 0, 2, 2)):
        path = self.root / side
        vram = np.zeros((512, 1024), dtype='<u2')
        vram[:2, :2] = words
        vram.tofile(path / 'frame.vram')
        return {'vram': 'frame.vram', 'viewport': list(crop)}

    def test_exact_color_and_mask_bit(self):
        left = self.image('native', [[31, 992], [31744, 32767]])
        right = self.image('reference', [[31 | 32768, 992], [31744, 32767]])
        image = viewport(self.root / 'native', left)
        self.assertEqual(image[0, 0].tolist(), [248, 0, 0])
        result = visual(self.root / 'native', left, self.root / 'reference', right)
        self.assertEqual(result['status'], 'FRAMEBUFFER_MATCH')
        self.assertFalse(result['display_mask_parity_evaluated'])

    def test_geometry_not_resized_to_pass_and_black_not_proof(self):
        left = self.image('native', 0)
        right = self.image('reference', 0, (0, 0, 1, 2))
        self.assertEqual(visual(self.root / 'native', left, self.root / 'reference', right)['status'],
                         'DIMENSION_MISMATCH')
        right['viewport'] = [0, 0, 2, 2]
        self.assertEqual(visual(self.root / 'native', left, self.root / 'reference', right)['status'],
                         'BLACK_ONLY_UNVERIFIED')

    def test_changed_pixels_and_unsupported_vram(self):
        left = self.image('native', 1)
        right = self.image('reference', 2)
        result = visual(self.root / 'native', left, self.root / 'reference', right)
        self.assertEqual(result['status'], 'PIXELS_DIFFER')
        self.assertEqual(result['common_top_left_diagnostic']['changed_pixels'], 4)
        right['gpu_status'] = 1 << 21
        right['viewport'][0] = 2
        with self.assertRaisesRegex(ValueError, 'RGB24 origin'):
            viewport(self.root / 'reference', right)
        left['vram'] = '../outside.vram'
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            viewport(self.root / 'native', left)

    def test_rgb24_retains_all_eight_color_bits(self):
        path = self.root / 'reference' / 'packed.vram'
        raw = bytearray(1024 * 512 * 2)
        raw[:6] = bytes([1, 2, 3, 254, 239, 223])
        path.write_bytes(raw)
        row = dict(vram=path.name, viewport=[0, 0, 2, 1], gpu_status=1 << 21, display_start_x=0)
        self.assertEqual(viewport(path.parent, row).tolist(), [[[1, 2, 3], [254, 239, 223]]])
        # Source X=0 can also mean programmed X=1023 plus a wrapped skip.
        # Neither that case nor missing provenance may be decoded as byte0.
        for base in (1023, None):
            row['display_start_x'] = base
            with self.assertRaisesRegex(ValueError, 'RGB24 origin'):
                viewport(path.parent, row)
        row['display_start_x'] = 4
        row['viewport'][0] = 4
        raw[8:14] = bytes([1, 2, 3, 254, 239, 223])
        path.write_bytes(raw)
        self.assertEqual(viewport(path.parent, row).tolist(), [[[1, 2, 3], [254, 239, 223]]])

    def test_native_rgb24_metadata_selects_packed_bytes(self):
        path = self.root / 'native' / 'packed.vram'
        raw = bytearray(1024 * 512 * 2)
        raw[8:14] = bytes([1, 2, 3, 254, 239, 223])
        path.write_bytes(raw)
        row = dict(vram=path.name, crop=[4, 0, 2, 1], display=[4, 0, 2, 1],
                   image_source='gpu_scanout', display_rgb24=1, display_enabled=1)
        self.assertEqual(viewport(path.parent, row).tolist(),
                         [[[1, 2, 3], [254, 239, 223]]])

    def test_native_rgb24_requires_programmed_origin_provenance(self):
        row = self.image('native', 1)
        row['display_rgb24'] = 1
        with self.assertRaisesRegex(ValueError, 'RGB24 origin'):
            viewport(self.root / 'native', row)

    def test_known_disabled_displays_are_black_but_not_a_content_pass(self):
        left = self.image('native', 31)
        right = self.image('reference', 992)
        left['display_enabled'] = 0
        right['gpu_status'] = 1 << 23
        self.assertFalse(viewport(self.root / 'native', left).any())
        result = visual(self.root / 'native', left, self.root / 'reference', right)
        self.assertEqual(result['status'], 'BLACK_ONLY_UNVERIFIED')
        self.assertTrue(result['display_mask_parity_evaluated'])
        self.assertTrue(result['display_mask_equal'])

    def test_black_pixels_do_not_hide_known_display_mask_mismatch(self):
        left = self.image('native', 0)
        right = self.image('reference', 0)
        left['display_enabled'] = 1
        right['gpu_status'] = 1 << 23
        result = visual(self.root / 'native', left, self.root / 'reference', right)
        self.assertEqual(result['status'], 'DISPLAY_MASK_MISMATCH')
        self.assertFalse(result['display_mask_equal'])

    def test_black_pixels_do_not_hide_known_display_mode_mismatch(self):
        left = self.image('native', 0, (0, 0, 2, 1))
        right = self.image('reference', 0, (0, 0, 2, 1))
        left['display_rgb24'] = 0
        right.update(gpu_status=1 << 21, display_start_x=0)
        result = visual(self.root / 'native', left, self.root / 'reference', right)
        self.assertEqual(result['status'], 'DISPLAY_MODE_MISMATCH')

    def test_display_metadata_rejects_invalid_and_contradictory_flags(self):
        invalid = [dict(display_enabled='0'), dict(display_rgb24=2),
                   dict(gpu_status='0'), dict(gpu_status=0, display_enabled=0),
                   dict(gpu_status=0, display_rgb24=1)]
        for flags in invalid:
            with self.subTest(flags=flags):
                row = self.image('native', 31)
                row.update(flags)
                with self.assertRaisesRegex(ValueError, 'flag|status'):
                    viewport(self.root / 'native', row)

    def test_pcm_exact_missing_silence_and_shorter(self):
        samples = np.arange(200, dtype=np.int16).reshape(100, 2)
        self.assertEqual(audio_arrays(samples, samples)['status'], 'PCM_MATCH')
        self.assertEqual(audio_arrays(samples, samples[:-1])['status'], 'PCM_DIFFERENT')
        silence = np.zeros_like(samples)
        self.assertEqual(audio_arrays(silence, silence)['status'], 'SILENCE_ONLY_UNVERIFIED')
        self.assertEqual(audio_arrays(samples, silence)['status'], 'MISSING_AUDIO')
        with self.assertRaises(ValueError):
            audio_arrays(samples[:0], samples)

    def test_phase_alignment_is_diagnostic_only(self):
        rng = np.random.default_rng(8)
        samples = rng.integers(-1000, 1000, size=(1024, 2), dtype=np.int16)
        delayed = np.concatenate([np.zeros((12, 2), dtype=np.int16), samples])
        result = audio_arrays(delayed, samples, max_lag=20)
        self.assertEqual(result['status'], 'PCM_DIFFERENT')
        self.assertEqual(result['phase_diagnostic']['lag_samples'], 12)
        self.assertAlmostEqual(result['phase_diagnostic']['stereo_correlation'], 1)
        self.assertFalse(result['phase_diagnostic']['changes_verdict'])

    def test_pcm_count_and_checkpoint_completeness(self):
        path = self.root / 'native'
        np.arange(8, dtype='<i2').tofile(path / 'audio.s16le')
        row = dict(audio_bytes=16, audio_samples=4, audio_rate=44100)
        self.assertEqual(pcm(path, row).shape, (4, 2))
        row['audio_samples'] = 3
        with self.assertRaises(ValueError):
            pcm(path, row)
        (path / 'state.jsonl').write_text(json.dumps({'frame': 1}) + '\n')
        with self.assertRaisesRegex(ValueError, 'missing'):
            records(path / 'state.jsonl', [1, 60])

    def test_truncated_wav_is_rejected(self):
        path = self.root / 'reference' / 'audio.wav'
        with wave.open(str(path), 'wb') as stream:
            stream.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            stream.writeframes(bytes(16))
        path.write_bytes(path.read_bytes()[:-4])
        with self.assertRaisesRegex(ValueError, 'truncated WAV'):
            pcm(path.parent, {})

    def test_explicit_complete_vblank_audio_window(self):
        raw = np.arange(2211 * 2, dtype='<i2').reshape(-1, 2)
        raw.tofile(self.root / 'native' / 'audio.s16le')
        with wave.open(str(self.root / 'reference' / 'audio.wav'), 'wb') as stream:
            stream.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            stream.writeframes(raw[737:].tobytes())
        native = dict(frame=3, audio_bytes=len(raw) * 4, audio_samples=len(raw),
                      audio_rate=44100, ticks_per_vblank=566204, clock_hz=33868800)
        reference = dict(audio_start_frame=1, audio_start_tick=10000, tick=10000 + 566204 * 2)
        result = window_audio(self.root / 'native', native, self.root / 'reference', reference)
        self.assertEqual(result['status'], 'PCM_MATCH')
        self.assertEqual(result['window']['native_excluded_prefix_samples'], 737)
        reference['tick'] += 2000
        result = window_audio(self.root / 'native', native, self.root / 'reference', reference)
        self.assertEqual(result['status'], 'WINDOW_UNVERIFIED')
        reference.pop('audio_start_frame')
        with self.assertRaisesRegex(ValueError, 'boundary-anchored'):
            window_audio(self.root / 'native', native, self.root / 'reference', reference)


class ParityRunnerTests(unittest.TestCase):
    def test_strict_ranges_and_checkpoints(self):
        self.assertEqual(av_parity.scene_list('0-2,18,208'), [0, 1, 2, 18, 208])
        self.assertEqual(av_parity.checkpoint_list('1,60,1200'), [1, 60, 1200])
        for spec in ('-1', '211', '2-1', '1,', 'a'):
            with self.assertRaises(ValueError):
                av_parity.scene_list(spec)
        for spec in ('0', '1', '1,1', '2,1', '1801', '1,', ' 1', '+1'):
            with self.assertRaises(ValueError):
                av_parity.checkpoint_list(spec)

    def test_environment_cannot_inherit_test_fixtures(self):
        from unittest.mock import patch
        with patch.dict('os.environ', {'PC_INPUT': 'bad', 'PC_POKE': 'bad', 'SCENE': '208',
                                      'MDFN_GPU': '1', 'HOME': '/kept', 'AUTOPLAY': '0',
                                      'TOKI_INPUT_SCRIPT': 'bad', 'STR_DUMP_FRAMES': 'bad',
                                      'STR_DUMP_BS': 'bad', 'TOKI_MOUSE_SCRIPT': 'bad'}):
            env = av_parity.clean_environment()
        self.assertNotIn('PC_POKE', env)
        self.assertNotIn('PC_INPUT', env)
        self.assertNotIn('SCENE', env)
        self.assertNotIn('MDFN_GPU', env)
        self.assertEqual(env['HOME'], '/kept')
        self.assertFalse(set(env) & {'AUTOPLAY', 'TOKI_INPUT_SCRIPT', 'STR_DUMP_FRAMES',
                                     'STR_DUMP_BS', 'TOKI_MOUSE_SCRIPT'})

    def test_fixture_pokes_pair_native_navigation_vblank_with_reference_frame(self):
        native, reference = av_parity.fixture_pokes(['1:class_flag:01'], 1381)
        self.assertEqual(native, 'v1:class_flag=01')
        self.assertEqual(reference, ['-poke', '1382:800EF872:01'])
        self.assertEqual(av_parity.fixture_pokes([], None), ('', []))
        for spec in ('0:class_flag:01', '1:class_flag:1', '1:class_flag:', 'x:class_flag:01',
                     '1:no_such_symbol_zz:01'):
            with self.assertRaises(ValueError):
                av_parity.fixture_pokes([spec], 1381)
        with self.assertRaisesRegex(ValueError, 'reference-anchor'):
            av_parity.fixture_pokes(['1:class_flag:01'], None)

    @unittest.skipIf(NO_REFEMU, NO_REFEMU)
    def test_reference_instrumentation_rejects_drift(self):
        with self.assertRaisesRegex(ValueError, 'drift'):
            _bpr.instrument('unrelated source')
        with self.assertRaisesRegex(ValueError, 'drift'):
            _bpr.replace_once('twice twice', 'twice', 'replacement')
        command = ': && /usr/bin/clang++ obj.o -o bin/regtest && :'
        self.assertEqual(_bpr.link_argv(command), ['/usr/bin/clang++', 'obj.o', '-o', 'bin/regtest'])
        with self.assertRaises(ValueError):
            _bpr.link_argv('rm -rf not-a-link-command')

    def test_artifact_budget_includes_repeats_and_maximum_points(self):
        small = av_parity.artifact_budget([18], ['native'], [1, 60], [])
        repeated = av_parity.artifact_budget([18], ['native'], [1, 60], [18])
        large = av_parity.artifact_budget(list(range(211)), ['native', 'mednafen'], list(range(1, 17)), [18])
        self.assertGreater(repeated, small)
        self.assertGreater(large, 10 * 1024 ** 3)

    @unittest.skipIf(NO_REFEMU, NO_REFEMU)
    def test_redirect_survives_pending_argument_and_return_loads(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        header = refemu.DIR / 'parity_regtest.h'
        body = re.search(r'static void Redirect\([^)]*\)\n\{(.*?)\n\}', header.read_text(), re.S).group(1)
        fixture = '''typedef unsigned int u32;
namespace CPU {
struct Registers { u32 a0, ra; };
struct State { Registers regs; int pending; } g_state;
void SetPC(u32) {
  if (g_state.pending == 0) g_state.regs.a0 = 999;
  else g_state.regs.ra = 999;
}
}
static void Redirect(u32 destination, u32 argument, u32 return_address) { BODY }
int main() {
 for (int reg = 0; reg < 2; reg++) {
  CPU::g_state.pending = reg;
  Redirect(0x80042A28, 208, 0x800426F0);
  if (CPU::g_state.regs.a0 != 208 || CPU::g_state.regs.ra != 0x800426F0) return 1;
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as work:
            for label, code, expected in [('fixed', body, 0), ('negative-control',
                    body.replace('  CPU::SetPC(destination);', '') + '\n  CPU::SetPC(destination);', 1)]:
                source, binary = Path(work) / (label + '.cpp'), Path(work) / label
                source.write_text(fixture.replace('BODY', code))
                run([compiler, '-std=c++17', str(source), '-o', str(binary)], timeout=20,
                    max_output=65536).require_ok()
                self.assertEqual(run([str(binary)], timeout=2).returncode, expected)

    def test_summary_retains_failures_and_unverified(self):
        rows = [{'captures': {'reference': {'status': 'OK'}, 'native': {'status': 'TIMEOUT'}},
                 'comparisons': {'native': {'status': 'UNMEASURABLE'}},
                 'determinism': {'native': {'status': 'DIFFERENT_OR_INCOMPLETE'}}}]
        summary = av_parity.summarize(rows)
        self.assertEqual(summary['captures']['TIMEOUT'], 1)
        self.assertEqual(summary['comparisons'], {'UNMEASURABLE': 1})


if __name__ == '__main__':
    unittest.main()
