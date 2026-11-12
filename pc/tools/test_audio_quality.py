# SPDX-License-Identifier: GPL-3.0-or-later
"""Enhanced sound quality: the SPU's interpolation, the floating-point reverb,
the SoundFont injection point of the SPU core, and the 48 kHz device
resampler (pc/hal/spu_quality.c, pc/hal/audio_resample.c, the core patch in
pc/3rd/SPU-CORE.md "Local patches").

  python -m unittest discover -s pc/tools -p test_audio_quality.py -v

CoreTests       pc/tests/spu_quality_harness.c on the core archive.  With
                every setting at its console value the patched core is the
                unpatched one: pc/tests/spu_split_harness.c gives the same
                bytes (output, SPU RAM, registers) on this archive and on the
                core as it was before the patch (pristine + patches 0001-0002).
                Cubic and sinc change the output only (SPU RAM, ADSR, ENDX,
                SPUSTAT identical), deterministically; a 4.1 kHz tone keeps
                more of its level than through the console's Gaussian (the
                "muffling"); a voice played 2.5x above its rate aliases less
                through sinc than through cubic.  The floating-point reverb
                changes the output only.  Injected frames reach the output
                with the main volume (dry) and the rest's reverb (wet) and
                never the real reverb unit; the split off ignores them.
                Negative controls: an archive whose injection feeds the real
                reverb unit (SPU RAM differs), one whose interpolation switch
                is ignored (cubic == Gaussian).
ResampleTests   pc/tests/audio_resample_harness.c: 44.1 -> 48 kHz in odd
                device-sized requests: tones up to 18 kHz keep their level
                (0.2 dB) and frequency, everything else at least 70 dB down,
                a 20 kHz tone's image is gone; it pulls exactly the input it
                needs.  Negative control: linear interpolation leaves images
                the same check sees.
GameTests       the built port and your assets, the title song (SCENE_NAV
                18, the game-thread mix): enhanced with cubic, sinc, hq
                differs from the console sound and nothing else -- VRAM, RAM,
                the picture, the per-VBlank trace and the frame hashes are
                identical -- and repeats bit for bit; the parity profile and
                SPU_QUALITY=0 ignore the settings bit for bit.
                PC_TEST_PLATFORM=windows runs the Windows exe under Wine.
Nothing game-derived is written outside build/.
"""
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/tools'))
from pc_paths import disc_input  # noqa: E402
import game_platform  # noqa: E402
import ps1_residue  # noqa: E402
import spu_core  # noqa: E402

CC = shlex.split(os.environ.get('CC', 'cc'))
# the last core before the quality patch (music packs, 2026-11-02): the
# pristine upstream files plus the first two local patches
PRE_PATCH = 2
PC_BIN = game_platform.GAME
HAVE_GAME = PC_BIN.exists() and (ROOT / 'pc/assets/raw').exists()
OUT = ROOT / 'build/test-audio-quality' / game_platform.PLATFORM
F = 22050


def build_core(dest, mutate=None, rev=None):
    """A copy of the fetched core (with only the first `rev` local patches,
    or all of them), optionally with one source replacement, built to
    dest/spu_psx.a."""
    if rev is None:
        spu_core.copy(dest)
    else:
        spu_core.patched(dest, rev)
    cpp = dest / 'upstream/spu.cpp'
    if mutate:
        old, new = mutate
        text = cpp.read_text()
        assert text.count(old) == 1, old
        cpp.write_text(text.replace(old, new))
    return spu_core.build(dest)


class CoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='spu-quality-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        cls.harness = cls.build(cls, spu_core.archive(), 'quality')

    def build(self, archive, name, source='pc/tests/spu_quality_harness.c'):
        out = self.dir / name
        # (gc-sections: spu_psx.c's newer wrappers stay out of the split
        # harness, so it links against the pre-patch core too)
        run([*CC, '-std=gnu17', '-w', '-O1', '-ffunction-sections', '-fdata-sections',
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', source, 'pc/hal/spu_psx.c', str(archive),
             '-Wl,--gc-sections', '-lstdc++', '-lm', '-o', str(out)], cwd=ROOT, timeout=180).require_ok()
        return out

    def result(self, binary, cfg, arg):
        path = self.dir / f'{binary.name}-{cfg}-{arg}.bin'
        run([str(binary), cfg, arg, str(path)], timeout=60).require_ok()
        b = path.read_bytes()
        return dict(out=np.frombuffer(b[:F * 4], dtype='<i2').reshape(-1, 2).astype(np.float64),
                    raw=b[:F * 4], ram=b[F * 4:F * 4 + 0x80000], regs=b[F * 4 + 0x80000:])

    @staticmethod
    def level(x, hz):
        """amplitude of the `hz` component of the left channel (second half)"""
        s = x[len(x) // 2:, 0]
        t = np.arange(len(s)) / 44100.0
        return 2 * abs(np.dot(s, np.exp(-2j * np.pi * hz * t))) / len(s)

    def test_off_is_the_unpatched_core(self):
        old = build_core(self.dir / 'pre', rev=PRE_PATCH)
        a = self.build(old, 'split-pre', 'pc/tests/spu_split_harness.c')
        b = self.build(spu_core.archive(), 'split-now', 'pc/tests/spu_split_harness.c')
        for mode in ('plain', 'zero', 'dump', 'replace', 'only0'):
            with self.subTest(mode=mode):
                pa, pb = self.dir / f'pre-{mode}', self.dir / f'now-{mode}'
                run([str(a), mode, str(pa)], timeout=60).require_ok()
                run([str(b), mode, str(pb)], timeout=60).require_ok()
                self.assertEqual(pa.read_bytes(), pb.read_bytes())

    def test_interpolators_change_the_output_only(self):
        r = {m: self.result(self.harness, 'tone', m) for m in ('gaussian', 'cubic', 'sinc')}
        for m in ('cubic', 'sinc'):
            self.assertEqual(r[m]['ram'], r['gaussian']['ram'], m)
            self.assertEqual(r[m]['regs'], r['gaussian']['regs'], m)
            self.assertNotEqual(r[m]['raw'], r['gaussian']['raw'], m)
            again = self.result(self.harness, 'tone', m)
            self.assertEqual(again['raw'], r[m]['raw'], f'{m} is deterministic')

    def test_less_muffled_than_the_console(self):
        hz = 44100 / 4 * 0x0C00 / 0x1000
        lv = {m: self.level(self.result(self.harness, 'tone', m)['out'], hz) for m in ('gaussian', 'cubic', 'sinc')}
        db = {m: 20 * np.log10(lv[m] / lv['gaussian']) for m in lv}
        # the console's Gaussian takes several dB off a tone at a quarter of
        # the sample's rate; the others keep it
        self.assertGreater(db['cubic'], 1.5, lv)
        self.assertGreater(db['sinc'], 1.5, lv)

    def test_sinc_band_limits_a_high_pitch(self):
        f0 = 44100 / 8 * 2.5
        alias = 3 * f0 - 44100          # the third harmonic folded back
        db = {m: 20 * np.log10(self.level(self.result(self.harness, 'high', m)['out'], abs(alias)) /
                               self.level(self.result(self.harness, 'high', m)['out'], f0))
              for m in ('cubic', 'sinc')}
        self.assertLess(db['sinc'], db['cubic'] - 12, db)

    def test_sinc_needs_its_tables(self):
        self.assertEqual(self.result(self.harness, 'prepare', '-')['raw'],
                         self.result(self.harness, 'tone', 'cubic')['raw'])

    def test_hq_reverb_changes_the_output_only(self):
        a, b = self.result(self.harness, 'reverb', '0'), self.result(self.harness, 'reverb', '1')
        self.assertEqual(a['ram'], b['ram'])
        self.assertEqual(a['regs'], b['regs'])
        self.assertNotEqual(a['raw'], b['raw'])
        self.assertEqual(self.result(self.harness, 'reverb', '1')['raw'], b['raw'])
        # same reverb: the two tails stay close (the float unit is not a different effect)
        tail = slice(F // 2, F)
        ea = np.sqrt(np.mean(a['out'][tail] ** 2))
        eb = np.sqrt(np.mean(b['out'][tail] ** 2))
        self.assertGreater(ea, 50)
        self.assertLess(abs(20 * np.log10(eb / ea)), 1.5, (ea, eb))

    def test_injection(self):
        none = self.result(self.harness, 'inject', 'none')
        dry = self.result(self.harness, 'inject', 'dry')
        wet = self.result(self.harness, 'inject', 'wet')
        nosplit = self.result(self.harness, 'inject', 'nosplit')
        plain = self.result(self.harness, 'inject', 'plain')
        i = np.arange(F)
        v = np.trunc(4000.0 * np.sin(2 * np.pi * 441.0 * i / 44100.0))
        # dry: the frames, through the main volume (0x3FFF), added to the rest
        d = dry['out'][:, 0] - none['out'][:, 0]
        self.assertLessEqual(float(np.max(np.abs(d - v * 0x7FFE / 32768))), 2.0)
        for r in (dry, wet):
            self.assertEqual(r['ram'], none['ram'], 'the real reverb unit never sees the frames')
            self.assertEqual(r['regs'], none['regs'])
        self.assertNotEqual(wet['raw'], none['raw'])
        self.assertNotEqual(wet['raw'], dry['raw'])
        self.assertEqual(nosplit['raw'], plain['raw'], 'with the split off the frames are ignored')

    def test_negative_control_injection_into_the_real_unit(self):
        a = build_core(self.dir / 'nc-inject', mutate=(
            '          rest_reverb_in_left += in[2];\n',
            '          rest_reverb_in_left += in[2];\n          reverb_in_left += in[2];\n'))
        h = self.build(a, 'h-nc-inject')
        none, wet = self.result(h, 'inject', 'none'), self.result(h, 'inject', 'wet')
        self.assertNotEqual(wet['ram'], none['ram'])

    def test_negative_control_switch_ignored(self):
        a = build_core(self.dir / 'nc-switch', mutate=(
            '  if (s_pc_quality.interp != 0) [[unlikely]]\n    return PcInterpolate(*this);\n', ''))
        h = self.build(a, 'h-nc-switch')
        self.assertEqual(self.result(h, 'tone', 'cubic')['raw'], self.result(h, 'tone', 'gaussian')['raw'])


class ResampleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='resample-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        cls.bin = cls.dir / 'rs'
        run([*CC, '-std=gnu17', '-w', '-O2', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/audio_resample_harness.c', 'pc/hal/audio_resample.c', '-lm', '-o', str(cls.bin)],
            cwd=ROOT, timeout=120).require_ok()

    def tone(self, hz, linear=False):
        path = self.dir / f'{hz}-{linear}.raw'
        r = run([str(self.bin), str(hz), str(path)] + (['linear'] if linear else []), timeout=60).require_ok()
        x = np.fromfile(path, dtype='<i2').reshape(-1, 2).astype(np.float64)
        return x, int(r.stdout.split()[1])

    @staticmethod
    def analyse(x, hz):
        s = x[4800:, 0]                     # skip the start (history of silence)
        win = np.blackman(len(s))
        spec = np.abs(np.fft.rfft(s * win)) / (np.sum(win) / 2)
        freqs = np.fft.rfftfreq(len(s), 1 / 48000.0)
        peak = int(np.argmax(spec))
        near = np.abs(freqs - hz) < 30
        level = float(np.sqrt(np.sum(spec[near] ** 2) / 2.85)) if hz < 24000 else 0.0
        rest = float(np.max(spec[~near])) if np.any(~near) else 0.0
        return freqs[peak], level, rest

    def test_passband_level_frequency_and_spurs(self):
        for hz in (1000, 6000, 10000, 15000, 18000):
            with self.subTest(hz=hz):
                x, pulled = self.tone(hz)
                f, level, rest = self.analyse(x, hz)
                self.assertLess(abs(f - hz), 3)
                self.assertLess(abs(20 * np.log10(np.max(np.abs(x[4800:, 0])) / 16384)), 0.2)
                self.assertLess(20 * np.log10(rest / 16384 + 1e-12), -70)
                self.assertTrue(np.array_equal(x[:, 1], -x[:, 0]) or
                                np.max(np.abs(x[:, 1] + x[:, 0])) <= 1)
                # exactly the input the 96,000 frames need: 2 s at 44.1 kHz + the 32-frame lookahead
                self.assertLessEqual(abs(pulled - (88200 + 32)), 2)

    def test_image_of_a_high_tone_is_gone(self):
        x, _ = self.tone(20000)
        _, _, rest = self.analyse(x, 20000)
        self.assertLess(20 * np.log10(rest / 16384 + 1e-12), -70)

    def test_negative_control_linear_interpolation_leaves_images(self):
        x, _ = self.tone(15000, linear=True)
        _, _, rest = self.analyse(x, 15000)
        self.assertGreater(20 * np.log10(rest / 16384 + 1e-12), -50)


def run_port(work, frames, env):
    work.mkdir(parents=True, exist_ok=True)
    (work / 'pc').mkdir(exist_ok=True)
    for name in ('pc/assets', 'disks'):
        if not os.path.lexists(work / name):
            os.symlink(disc_input(name), work / name)
    e = {k: v for k, v in os.environ.items()
         if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'MUSIC_', 'SPU_'))}
    e.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
             PC_TRACE=str(work / 't.trace'), PC_TRACE_FRAMES=str(frames),
             PC_PARITY_DIR=str(work / 'parity'), PC_PARITY_FRAMES='300,900,1500,1800',
             PC_PARITY_RAM='1', PC_FRAME_HASH=str(work / 'frames.txt'), SCENE_NAV='18')
    e.update(env)
    r = run([str(PC_BIN), str(ROOT / 'pc/assets')], cwd=work, env=e, timeout=900,
            log_path=str(work / 'log.txt'))
    if not r.ok:
        raise RuntimeError(f'{work.name}: {r.reason or r.returncode}')
    return work


ARTIFACTS = ['parity/state.jsonl', 'frames.txt', 't.trace'] + [
    f'parity/frame_{n:06d}.{k}' for n in (300, 900, 1500, 1800) for k in ('vram', 'ram', 'ppm')]


def same(a, b, name):
    x, y = (a / name).read_bytes(), (b / name).read_bytes()
    if name.endswith('.ram'):
        x, y = ps1_residue.mask(x), ps1_residue.mask(y)
    return x == y


@unittest.skipUnless(HAVE_GAME, 'needs the built port and your pc/assets')
class GameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from concurrent.futures import ThreadPoolExecutor
        if OUT.exists():
            shutil.rmtree(OUT)
        enh = dict(TOKI_PROFILE='enhanced')
        jobs = {
            'plain': enh,
            'cubic': dict(enh, TOKI_SPU_INTERP='cubic'),
            'cubic2': dict(enh, TOKI_SPU_INTERP='cubic'),
            'sinc': dict(enh, TOKI_SPU_INTERP='sinc'),
            'hq': dict(enh, TOKI_SPU_REVERB='hq'),
            'off': dict(enh, TOKI_SPU_INTERP='sinc', TOKI_SPU_REVERB='hq', SPU_QUALITY='0'),
            'parity': {},
            'parity_set': dict(TOKI_SPU_INTERP='sinc', TOKI_SPU_REVERB='hq'),
        }
        with ThreadPoolExecutor(len(jobs)) as ex:
            futs = {k: ex.submit(run_port, OUT / k, 2700, v) for k, v in jobs.items()}
            cls.r = {k: f.result() for k, f in futs.items()}

    def audio(self, name):
        return (self.r[name] / 'parity/audio.s16le').read_bytes()

    def test_settings_change_the_sound_only(self):
        for name in ('cubic', 'sinc', 'hq'):
            with self.subTest(run=name):
                self.assertNotEqual(self.audio(name), self.audio('plain'))
                for art in ARTIFACTS:
                    self.assertTrue(same(self.r['plain'], self.r[name], art), art)
                log = (self.r[name] / 'log.txt').read_text(errors='replace')
                self.assertIn('[SPU] sound quality:', log)

    def test_deterministic(self):
        self.assertEqual(self.audio('cubic'), self.audio('cubic2'))

    def test_parity_and_the_switch_ignore_the_settings(self):
        self.assertEqual(self.audio('off'), self.audio('plain'))
        self.assertEqual(self.audio('parity_set'), self.audio('parity'))
        for art in ARTIFACTS:
            self.assertTrue(same(self.r['parity'], self.r['parity_set'], art), art)
        self.assertNotIn('[SPU] sound quality:', (self.r['parity_set'] / 'log.txt').read_text(errors='replace'))
        # negative control: the comparison sees the profiles differ
        self.assertNotEqual(self.audio('parity'), self.audio('plain'))


if __name__ == '__main__':
    unittest.main()
