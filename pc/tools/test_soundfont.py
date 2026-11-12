# SPDX-License-Identifier: GPL-3.0-or-later
"""SoundFont mode (pc/hal/soundfont.c): the game's sequenced music through
TinySoundFont from the game's own note events (enhanced; docs/pc-port/
ENHANCED-MODE.md "SoundFont mode").

  python -m unittest discover -s pc/tools -p test_soundfont.py -v

The port ships no SoundFont: every test makes its own with
pc/tools/make_test_sf2.py (computed sine / square / noise, public domain).

Sf2Tests        the generated SF2 loads in TinySoundFont as the port loads it
                (129 presets); a corrupt file does not.
MapTests        <name>.map: the song's section wins over the VAB's, `off`,
                `drums`, bank:program, per-key remaps, transpose, gain_db,
                velocity, [global]; a VAB without a section has no entry.
HeuristicTests  synthetic programs and ADPCM samples: a drum kit (one-key
                zones, a sample each), a multi-sampled instrument (one-key
                zones sharing a few samples: not a kit), one-shot noise, and
                looped strings / flute / reed / brass / bass, a decaying piano;
                drum keys for a hi-hat and a kick.
CadenceTests    pc/tests/soundfont_harness.c: the real spu.c, soundfont.c and
                music_pack.c, 120 notes keyed through the register bridge
                under the deterministic mixer, steady and bursty game VBlanks
                x fixed and odd device requests, and fast-forward (VBlanks at
                4x): every note is started once by the SoundFont (its 441 Hz
                sine is heard, the SPU's DC sample is not), none is lost or
                left sounding, no tag is unknown.  Negative controls: a
                spu.c whose queue drops the tag (the notes never reach the
                SoundFont), TOKI_SPU_LATENCY_MS=0 (key-ons lost in the core
                and so in the SoundFont).
GameTests       the built port and your assets, the title song (SCENE_NAV 18,
                enhanced, the game-thread mix): a silent SoundFont silences
                the song (the voices are really left out; without one RMS >
                500), a sine SoundFont plays it; VRAM, RAM, the picture, the
                per-VBlank trace and the frame hashes are identical with and
                without a SoundFont; MUSIC_SOUNDFONT=0 and the parity profile
                ignore it bit for bit; a map with every program `off` gives
                the original output exactly, per VAB or in the song's own
                section (negative control: another song's section); every music key-on became a
                SoundFont note and none was left sounding, the heuristic's
                choices are in <name>.auto.map.  Title -> START (the driver's
                fade) -> NAME_ENT with the sine SoundFont: the song's level
                follows the fade as the original does, the notes stop at the
                close and the next song plays from the next VAB; the trace
                is identical.  Fast-forward held (TOKI_FF_HOLD): the same
                notes, in step.  PC_TEST_PLATFORM=windows: the Windows exe
                under Wine.
"""
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
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
import make_test_sf2  # noqa: E402
import ps1_residue  # noqa: E402
import spu_core  # noqa: E402

CC = shlex.split(os.environ.get('CC', 'cc'))
PC_BIN = game_platform.GAME
HAVE_GAME = PC_BIN.exists() and (ROOT / 'pc/assets/raw').exists()
OUT = ROOT / 'build/test-soundfont' / game_platform.PLATFORM


def write_sf2(path, **kw):
    Path(path).write_bytes(make_test_sf2.build(**kw))
    return Path(path)


def build_unit(out):
    run([*CC, '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
         'pc/tests/soundfont_unit_harness.c', 'pc/hal/spu_psx.c', str(spu_core.archive()), '-lstdc++', '-lm',
         '-o', str(out)], cwd=ROOT, timeout=180).require_ok()
    return out


class Sf2Tests(unittest.TestCase):
    def test_generated_sf2_loads(self):
        with tempfile.TemporaryDirectory(prefix='sf2-') as d:
            h = build_unit(Path(d) / 'unit')
            for wave in ('sine', 'square', 'silent'):
                sf = write_sf2(Path(d) / f'{wave}.sf2', wave=wave)
                r = run([str(h), 'probe', str(sf)], timeout=30)
                self.assertTrue(r.ok, r.stdout)
                self.assertIn('presets 129', r.stdout)
            bad = Path(d) / 'bad.sf2'
            bad.write_bytes(b'RIFF\0\0\0\0sfbk')
            self.assertFalse(run([str(h), 'probe', str(bad)], timeout=30).ok)


class MapTests(unittest.TestCase):
    MAP = """# a comment
[global]
gain_db = -6
velocity = 90
[vab-00000001]
prog 3 = 48            ; strings
prog 3 transpose = -12
prog 3 gain_db = 6
prog 3 velocity = 64
prog 4 = off
prog 9 = drums
prog 9 key 60 = 38
[seq-0000000a]
prog 3 = 0:73
"""

    def test_lookups(self):
        with tempfile.TemporaryDirectory(prefix='sfmap-') as d:
            h = build_unit(Path(d) / 'unit')
            m = Path(d) / 'x.map'
            m.write_text(self.MAP)
            out = run([str(h), 'map', str(m)], timeout=30).require_ok().stdout
        lines = out.splitlines()
        self.assertEqual(lines[0], 'entries 8 gain 0.501 velocity 90')
        # (scope order: the song's section first)
        self.assertIn('- vab-00000001 3 -1 -> 0 0 48 0.000 vab-00000001', lines)
        self.assertIn('seq-0000000a vab-00000001 3 -1 -> 0 0 73 0.000 seq-0000000a', lines)
        self.assertIn('seq-0000000b vab-00000001 3 -1 -> 0 0 48 0.000 vab-00000001', lines)
        self.assertIn('- vab-00000001 9 -1 -> 0 128 0 0.000 vab-00000001', lines)
        self.assertIn('- vab-00000001 9 60 -> 4 0 0 38.000 vab-00000001', lines)
        self.assertIn('- vab-00000001 9 61 -> none', lines)
        self.assertIn('- vab-00000001 4 -1 -> 1 0 0 0.000 vab-00000001', lines)      # off
        self.assertIn('- vab-00000001 3 -1 -> 2 0 0 -12.000 vab-00000001', lines)    # transpose
        self.assertIn('- vab-00000001 3 -1 -> 3 0 0 1.995 vab-00000001', lines)      # gain_db 6
        self.assertIn('- vab-00000001 3 -1 -> 5 0 0 64.000 vab-00000001', lines)     # velocity
        self.assertIn('- vab-00000002 3 -1 -> none', lines)


class HeuristicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='sfheur-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.h = build_unit(Path(cls.tmp.name) / 'unit')

    def classify(self, case, what='classify'):
        return run([str(self.h), what, case], timeout=60).require_ok().stdout.strip()

    def test_programs(self):
        want = {'kit': '128:0 drum 1', 'multisample': '0:12 drum 0', 'snare': '128:0 drum 1',
                'strings': '0:48 drum 0', 'flute': '0:73 drum 0', 'reed': '0:71 drum 0',
                'brass': '0:61 drum 0', 'bass': '0:33 drum 0', 'piano': '0:0 drum 0'}
        for case, prefix in want.items():
            with self.subTest(case=case):
                self.assertTrue(self.classify(case).startswith(prefix), self.classify(case))

    def test_drum_keys(self):
        self.assertTrue(self.classify('hihat', 'drum').startswith('42 closed hi-hat'))
        self.assertTrue(self.classify('kick', 'drum').startswith('36 kick'))


class CadenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='sfcad-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        cls.sf2 = write_sf2(cls.dir / 'short.sf2', release=0.005)
        cls.binary = cls.build(cls, ROOT / 'pc/hal/spu.c', 'sfh')

    def build(self, source, name):
        out = self.dir / name
        sdl = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2'], timeout=10).require_ok().stdout)
        run([*CC, '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-ffunction-sections', '-fdata-sections',
             '-DSPU_SOURCE="' + str(source) + '"', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/soundfont_harness.c', 'pc/hal/soundfont.c', 'pc/hal/music_pack.c',
             'pc/hal/music_decode.c', 'pc/hal/audio_resample.c', 'pc/hal/cd_audio.c', 'pc/hal/spu_psx.c',
             str(spu_core.archive()), '-Wl,--gc-sections', '-Wl,--unresolved-symbols=ignore-in-object-files',
             '-lstdc++', '-lm', '-ldl', *sdl, '-no-pie', '-o', str(out)], cwd=ROOT, timeout=240).require_ok()
        return out

    def case(self, game, device, binary=None, env=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_', 'MUSIC_'))}
        e['TOKI_MUSIC_EVENTS'] = '0'
        e.update(env or {})
        r = run([str(binary or self.binary), game, device, str(self.sf2)], cwd=ROOT, env=e, timeout=120,
                max_output=64000)
        return r.returncode, r.stdout

    def test_every_note_from_the_soundfont(self):
        for game in ('det', 'steady', 'bursty'):
            for device in ('fixed', 'odd'):
                with self.subTest(game=game, device=device):
                    rc, out = self.case(game, device)
                    self.assertEqual(rc, 0, out)
                    self.assertIn('notes 120 onsets 120', out)
                    self.assertIn('sf_on 120 sf_keys 120 unknown 0 kon_lost 0 koff_lost 0', out)

    def test_fast_forward_stays_in_step(self):
        for device in ('fixed', 'odd'):
            with self.subTest(device=device):
                rc, out = self.case('ff', device)
                self.assertEqual(rc, 0, out)
                self.assertIn('sf_on 120 sf_keys 120 unknown 0 kon_lost 0', out)
                self.assertIn('sounding 0', out)

    def test_negative_control_tag_dropped_by_the_queue(self):
        src = self.dir / 'spu_notag.c'
        text = (ROOT / 'pc/hal/spu.c').read_text()
        self.assertEqual(text.count('    w->tag = tag;\n'), 1)
        src.write_text(text.replace('    w->tag = tag;\n', '    w->tag = 0;\n'))
        b = self.build(src, 'sfh-notag')
        rc, out = self.case('steady', 'fixed', binary=b)
        self.assertEqual(rc, 1, out)
        self.assertIn('sf_on 0 ', out)
        rc, out = self.case('det', 'fixed', binary=b)        # the direct path does not use the queue
        self.assertEqual(rc, 0, out)

    def test_negative_control_direct_writes_lose_notes(self):
        rc, out = self.case('steady', 'fixed', env={'TOKI_SPU_LATENCY_MS': '0'})
        self.assertEqual(rc, 1, out)
        self.assertNotIn('onsets 120 ', out)


# ------------------------------------------------------------------ the game
def run_port(work, frames, env, scene=18):
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
             PC_PARITY_RAM='1', PC_FRAME_HASH=str(work / 'frames.txt'),
             TOKI_MUSIC_LOG=str(work / 'music.jsonl'), TOKI_SF2_TRACE=str(work / 'sf2.trace'),
             SCENE_NAV=str(scene))
    e.update(env)
    r = run([str(PC_BIN), str(ROOT / 'pc/assets')], cwd=work, env=e, timeout=900,
            log_path=str(work / 'log.txt'))
    if not r.ok:
        raise RuntimeError(f'{work.name}: {r.reason or r.returncode}')
    return work


ARTIFACTS = ['parity/state.jsonl', 'frames.txt', 't.trace'] + [
    f'parity/frame_{n:06d}.{k}' for n in (300, 900, 1500, 1800) for k in ('vram', 'ram', 'ppm')]
TRACE = re.compile(r'^(\d+) owned (\w+) notes (\w+) fresh (\w+) core (\w+) active (\d+) on (\d+) off (\d+) '
                   r'keys (\d+) ended (\d+) taken (\d+) unknown (\d+) frames (\d+)')


def sf_trace(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        m = TRACE.match(line)
        if m:
            g = m.groups()
            rows.append(dict(vbl=int(g[0]), owned=int(g[1], 16), notes=int(g[2], 16), fresh=int(g[3], 16),
                             core=int(g[4], 16), active=int(g[5]), on=int(g[6]), off=int(g[7]), keys=int(g[8]),
                             ended=int(g[9]), taken=int(g[10]), unknown=int(g[11]), frames=int(g[12])))
    return rows


def pcm(path):
    return np.fromfile(path, dtype='<i2').astype(np.float64)


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
        # its own folder: the parallel runner (pc/tools/run_unittests.py) runs
        # this class and the fade class at once, and both lived under OUT
        base = cls.base = OUT / 'game'
        if base.exists():
            shutil.rmtree(base)
        music = cls.music = base / 'music'
        sfdir = music / 'soundfonts'
        sfdir.mkdir(parents=True)
        write_sf2(sfdir / 'sine.sf2')
        write_sf2(sfdir / 'silent.sf2', wave='silent')
        write_sf2(sfdir / 'silentoff.sf2', wave='silent')
        enh = dict(TOKI_PROFILE='enhanced', TOKI_MUSIC_DIR=str(music))
        # the heuristic's VAB ids come from a first run; `off` for every program
        cls.sine = run_port(base / 'sine', 2700, dict(enh, TOKI_SOUNDFONT='sine.sf2'))
        auto = (sfdir / 'sine.auto.map').read_text()
        offmap = []
        for sec, prog in re.findall(r'\[(vab-[0-9a-f]+)\]\nprog (\d+) =', auto):
            offmap += [f'[{sec}]', f'prog {prog} = off']
        (sfdir / 'silentoff.map').write_text('\n'.join(offmap) + '\n')
        # the same per song: the title song's section (its first notes are keyed
        # before the music observer sees the song); and a wrong song id
        title = next(json.loads(l)['id'] for l in (cls.sine / 'music.jsonl').read_text().splitlines()
                     if json.loads(l)['ev'] == 'start')
        progs = sorted({int(p) for p in re.findall(r'\nprog (\d+) =', auto)})
        for name, song in (('songoff', title), ('songwrong', 'seq-00000000')):
            write_sf2(sfdir / f'{name}.sf2', wave='silent')
            (sfdir / f'{name}.map').write_text(f'[{song}]\n' + ''.join(f'prog {p} = off\n' for p in progs))
        jobs = {
            'plain': enh,
            'silent': dict(enh, TOKI_SOUNDFONT='silent.sf2'),
            'silentoff': dict(enh, TOKI_SOUNDFONT='silentoff.sf2'),
            'songoff': dict(enh, TOKI_SOUNDFONT='songoff.sf2'),
            'songwrong': dict(enh, TOKI_SOUNDFONT='songwrong.sf2'),
            'switch_off': dict(enh, TOKI_SOUNDFONT='sine.sf2', MUSIC_SOUNDFONT='0'),
            'parity': dict(TOKI_MUSIC_DIR=str(music)),
            'parity_sf': dict(TOKI_MUSIC_DIR=str(music), TOKI_SOUNDFONT='sine.sf2'),
            'ff': dict(enh, TOKI_SOUNDFONT='sine.sf2', TOKI_FF_HOLD='1'),
        }
        with ThreadPoolExecutor(len(jobs)) as ex:
            futs = {k: ex.submit(run_port, base / k, 2700, v) for k, v in jobs.items()}
            cls.r = {k: f.result() for k, f in futs.items()}
        cls.r['sine'] = cls.sine
        cls.auto = auto

    def audio(self, name):
        return (self.r[name] / 'parity/audio.s16le').read_bytes()

    def test_silent_soundfont_silences_the_song(self):
        a, b = pcm(self.r['plain'] / 'parity/audio.s16le'), pcm(self.r['silent'] / 'parity/audio.s16le')
        self.assertGreater(math.sqrt(np.mean(a * a)), 500)     # negative control: the song is there
        self.assertEqual(float(np.max(np.abs(b))), 0.0)

    def test_sine_soundfont_plays_the_song(self):
        a = pcm(self.r['sine'] / 'parity/audio.s16le')
        self.assertGreater(math.sqrt(np.mean(a * a)), 100)
        self.assertNotEqual(self.audio('sine'), self.audio('plain'))

    def test_game_state_identical(self):
        for other in ('sine', 'silent', 'silentoff', 'switch_off'):
            for art in ARTIFACTS:
                with self.subTest(run=other, artifact=art):
                    self.assertTrue(same(self.r['plain'], self.r[other], art))
        self.assertFalse(same(self.r['plain'], self.r['parity'], 'frames.txt'))

    def test_switch_and_parity_ignore_the_soundfont(self):
        self.assertEqual(self.audio('switch_off'), self.audio('plain'))
        self.assertEqual(self.audio('parity_sf'), self.audio('parity'))
        for art in ARTIFACTS:
            self.assertTrue(same(self.r['parity'], self.r['parity_sf'], art), art)
        self.assertNotIn('[SF2]', (self.r['parity_sf'] / 'log.txt').read_text(errors='replace'))

    def test_map_off_gives_the_original(self):
        self.assertTrue(self.auto.strip())
        self.assertEqual(self.audio('silentoff'), self.audio('plain'))
        # per song, from its first note on; negative control: another song's id
        self.assertEqual(self.audio('songoff'), self.audio('plain'))
        self.assertEqual(float(np.max(np.abs(pcm(self.r['songwrong'] / 'parity/audio.s16le')))), 0.0)

    def test_every_music_key_on_is_a_note(self):
        for name in ('sine', 'ff'):
            with self.subTest(run=name):
                rows = sf_trace(self.r[name] / 'sf2.trace')
                last = rows[-1]
                self.assertGreater(last['keys'], 100)
                self.assertEqual(last['on'], last['keys'])
                self.assertEqual(last['unknown'], 0)
                # while audio is mixed: no note sounds on a voice the core does not play
                # (a note keyed since the last mix starts in the next one)
                mixed = [row for prev, row in zip(rows, rows[1:]) if row['frames'] > prev['frames']]
                self.assertGreater(len(mixed), 1000)
                for row in mixed:
                    self.assertEqual(row['notes'] & ~row['fresh'] & ~row['core'], 0, row)
        self.assertIn('[vab-', self.auto)
        self.assertIn('(heuristic)', self.auto)

    def test_fast_forward_same_notes(self):
        a, b = sf_trace(self.r['sine'] / 'sf2.trace'), sf_trace(self.r['ff'] / 'sf2.trace')
        self.assertEqual(a[-1]['keys'], b[-1]['keys'])
        self.assertEqual(a[-1]['on'], b[-1]['on'])


@unittest.skipUnless(HAVE_GAME, 'needs the built port and your pc/assets')
class FadeTests(unittest.TestCase):
    """title -> START (the driver's fade) -> NAME_ENT, playthrough harness"""
    SCRIPT = ('hold 2\n\nphase title_fade\n'
              'wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000\n'
              '+120 start\n'
              'wait album_prompt now_step=0x13 now_sub_step=2 now_sub_sub_step=0x41 within 3000\n'
              '+300 circle\n')

    @classmethod
    def setUpClass(cls):
        from concurrent.futures import ThreadPoolExecutor
        d = cls.dir = OUT / 'fade'
        if d.exists():
            shutil.rmtree(d)
        sfdir = d / 'music/soundfonts'
        sfdir.mkdir(parents=True)
        write_sf2(sfdir / 'sine.sf2')
        script = d / 'title_fade.txt'
        script.write_text(cls.SCRIPT)

        def play(name, extra):
            out = d / name
            cmd = [sys.executable, str(ROOT / 'pc/tools/playthrough.py'), str(script), '--out', str(out),
                   '--profile', 'enhanced', '--max-frames', '3000',
                   '--set', f'TOKI_MUSIC_DIR={d / "music"}', '--set', f'TOKI_MUSIC_LOG={out}.jsonl',
                   '--set', f'PC_AUDIO_DUMP={out}.raw', '--set', f'PC_FRAME_HASH={out}.frames',
                   '--set', f'TOKI_SF2_TRACE={out}.sf2', *extra]
            r = run(cmd, cwd=ROOT, timeout=1200)
            if not r.ok:
                raise RuntimeError(r.stdout[-2000:])
            return out
        with ThreadPoolExecutor(2) as ex:
            a = ex.submit(play, 'orig', [])
            b = ex.submit(play, 'sf', ['--set', 'TOKI_SOUNDFONT=sine.sf2'])
            cls.orig, cls.sf = a.result(), b.result()

    def test_same_game(self):
        self.assertEqual(Path(f'{self.orig}.frames').read_bytes(), Path(f'{self.sf}.frames').read_bytes())
        self.assertEqual((self.orig / 'title_fade.pc.trace').read_bytes(),
                         (self.sf / 'title_fade.pc.trace').read_bytes())

    def test_level_follows_the_fade_and_the_close_stops_the_notes(self):
        ev = [json.loads(l) for l in Path(f'{self.sf}.jsonl').read_text().splitlines()]
        starts = [e for e in ev if e['ev'] == 'start']
        close = next(e for e in ev if e['ev'] == 'close')
        vols = [(e['vbl'], e['vol'][0]) for e in ev if e['ev'] == 'vol' and e['id'] == starts[0]['id']]
        self.assertGreater(len(vols), 20)
        self.assertTrue(starts[0]['soundfont'])
        rows = {r['vbl']: r for r in sf_trace(f'{self.sf}.sf2')}
        # the close released every note: nothing of the old song sounds when the next one starts
        self.assertGreater(len(starts), 1)
        self.assertGreater(starts[1]['vbl'], close['vbl'])
        before = rows.get(starts[1]['vbl'] - 1)
        self.assertIsNotNone(before)
        self.assertEqual(before['notes'], 0)
        self.assertEqual(before['active'], 0)
        # the next song is played by the SoundFont too
        self.assertTrue(starts[1]['soundfont'])
        self.assertGreater(rows[max(rows)]['on'], rows[close['vbl']]['on'])

        # RMS per 0.25 s through the fade: the SoundFont's level against the original's
        def env(raw):
            a = pcm(raw).reshape(-1, 2)[:, 0]
            v0, v1 = vols[0][0], vols[-1][0]
            out = []
            for v in range(v0, v1, 15):
                i = int(v * 44100 * 1001 / 60000)
                seg = a[i:i + 11025]
                out.append(math.sqrt(np.mean(seg * seg)) if len(seg) else 0.0)
            return np.array(out)
        eo, es = env(f'{self.orig}.raw'), env(f'{self.sf}.raw')
        self.assertGreater(len(eo), 8)
        # both fall with the driver's volume: strongly correlated, ending far below their start
        self.assertGreater(float(np.corrcoef(eo, es)[0, 1]), 0.8, (eo, es))
        self.assertLess(es[-1], es[0] * 0.3, es)


if __name__ == '__main__':
    unittest.main()
