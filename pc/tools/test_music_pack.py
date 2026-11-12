# SPDX-License-Identifier: GPL-3.0-or-later
"""Music packs (pc/hal/music_pack.c) and the SPU core's music split.

  python -m unittest discover -s pc/tools -p test_music_pack.py -v

SeqIdTests        the track id: the C and Python spellings agree on a
                  synthetic SEQ; one changed byte or a missing end changes /
                  refuses it; every SEQ on your disc parses to its end
                  (assets only).
HarnessTests      pc/tests/music_pack_harness.c drives the actual
                  music_pack.c against a simulated libsnd: the voice mask,
                  start, volume and main-volume following, pause holding the
                  position, the file's loop points, the song's own end
                  playing the file out, replay, stop, close, the fallback,
                  the profile switch, the dump; the decoders (WAV smpl loop,
                  and FLAC / Ogg Vorbis with LOOPSTART / LOOPEND tags when
                  ffmpeg can make them).  Negative controls: nine mutated
                  copies of music_pack.c, each must fail its named check.
SpuSplitTests     pc/tests/spu_split_harness.c on the core archive: the
                  split changes the output and nothing else (SPU RAM, ADSR,
                  ENDX, SPUSTAT identical); replacing a voice's output equals
                  never keying it; the dumped share is the voice's.  Negative
                  control: an archive whose shadow reverb writes the real
                  work area (SPU RAM differs).
GameTests         the built port and your assets: the title song (SCENE_NAV
                  18, enhanced, the game-thread mix) -- its id is the same in
                  two runs and is the disc's; the dump is the song; a pack
                  made of the dump reproduces the output within 1 LSB; a
                  silent pack silences it (the voices are really muted); VRAM,
                  RAM, the picture and the per-VBlank trace are identical with
                  and without a pack; MUSIC_PACK=0 and the parity profile
                  ignore the pack bit for bit.  title -> START (the driver's
                  fade) -> NAME_ENT with a tone pack: the file starts and
                  stops on the game's VBlank and its level follows the fade;
                  the trace is identical; the next song, not in the pack,
                  falls back.  Negative controls: the run without the pack
                  has no tone; a constant-level model does not fit the fade.
Nothing game-derived is written outside a temporary directory.
"""
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/tools'))
from pc_paths import NO_HISTORY, disc_input, have_commit  # noqa: E402
import music_tracks  # noqa: E402
import ps1_residue  # noqa: E402
import game_platform  # noqa: E402
import spu_core  # noqa: E402

PC_BIN = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
HAVE_GAME = PC_BIN.exists() and (ROOT / 'pc/assets/raw').exists()
CC = shlex.split(os.environ.get('CC', 'cc'))


def synthetic_seq():
    """A small SEQ in libsnd's dialect: program change, the loop markers
    (CC 99 = 20, CC 6 = 127, CC 99 = 30), a note on / off by running
    status, a tempo change, the end of track."""
    body = bytes([
        0x00, 0xC0, 0x05,
        0x00, 0xB0, 0x63, 0x14,
        0x00, 0x06, 0x7F,
        0x00, 0x90, 0x3C, 0x64,
        0x83, 0x60, 0x3C, 0x00,
        0x00, 0xFF, 0x51, 0x07, 0xA1, 0x20,
        0x00, 0xB0, 0x63, 0x1E,
        0x00, 0xFF, 0x2F])
    head = b'pQES' + bytes([0, 0, 0, 1, 0x01, 0xE0, 0x07, 0xA1, 0x20, 0x04, 0x02])
    return head + body + b'\x00' * 32


def write_wav(path, frames, rate=44100, smpl=None):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<hh', l, r) for l, r in frames))
    if smpl:
        start, end = smpl                   # end inclusive, as the smpl chunk has it
        chunk = struct.pack('<9I', 0, 0, 22675, 60, 0, 0, 0, 1, 0)
        chunk += struct.pack('<6I', 0, 0, start, end, 0, 0)
        data = bytearray(Path(path).read_bytes())
        data += b'smpl' + struct.pack('<I', len(chunk)) + chunk
        struct.pack_into('<I', data, 4, len(data) - 8)
        Path(path).write_bytes(bytes(data))


def ramp(n):
    """Left channel encodes the frame number (4 per step, period 8192, never 0)."""
    return [((i % 8192) * 4 - 16382, -((i % 8192) * 4 - 16382)) for i in range(n)]


class SeqIdTests(unittest.TestCase):
    def test_python_id_of_synthetic_seq(self):
        seq = synthetic_seq()
        n = music_tracks.seq_span(seq)
        self.assertEqual(seq[n - 2:n], b'\xff\x2f')
        tid = music_tracks.track_id(seq)
        self.assertRegex(tid, r'^seq-[0-9a-f]{8}$')
        t = music_tracks.seq_timing(seq)
        self.assertEqual(t['loop_count'], 127)
        self.assertIsNotNone(t['loop_start'])
        # negative controls: one changed byte, a missing end, no header
        changed = bytearray(seq)
        changed[20] ^= 1
        self.assertNotEqual(music_tracks.track_id(bytes(changed)), tid)
        self.assertIsNone(music_tracks.track_id(seq[:n - 1]))
        self.assertIsNone(music_tracks.track_id(b'xQES' + seq[4:]))
        # bytes after the end do not belong to the song
        self.assertEqual(music_tracks.track_id(seq[:n] + b'\x99' * 8), tid)

    @unittest.skipUnless((ROOT / 'pc/assets/raw/CDROM/VAB_SEQ').exists(), 'needs your pc/assets')
    def test_every_disc_seq_parses_to_its_end(self):
        folder = ROOT / 'pc/assets/raw/CDROM/VAB_SEQ'
        n = 0
        for path in sorted(folder.glob('*.SEQ')):
            data = path.read_bytes()
            offs = struct.unpack('<16I', data[:64])
            starts = sorted({o + 16 for j, o in enumerate(offs) if o or j == 0})
            for k, s in enumerate(starts):
                span = music_tracks.seq_span(data, s)
                nxt = starts[k + 1] if k + 1 < len(starts) else len(data)
                self.assertGreater(span, 0, f'{path.name} @{s:#x}')
                self.assertFalse(data[s + span:nxt].strip(b'\0'), f'{path.name} @{s:#x}: bytes after the end')
                n += 1
        self.assertGreater(n, 100)
        tracks = music_tracks.scan(str(ROOT / 'pc/assets/raw/CDROM'))
        self.assertEqual(len({t for t in tracks}), len(tracks))


class HarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='music-pack-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        cls.seq = cls.dir / 'song.seq'
        cls.seq.write_bytes(synthetic_seq())
        cls.id = music_tracks.track_id(synthetic_seq())
        pack = cls.dir / 'music/packs/t'
        pack.mkdir(parents=True)
        write_wav(pack / f'{cls.id}.wav', ramp(8000))
        (pack / f'{cls.id}.ini').write_text('# test\nloop_start = 1000\nloop_end = 3000\n')
        (cls.dir / 'music/packs/empty').mkdir()
        cls.binary = cls.build(ROOT / 'pc/hal/music_pack.c', 'harness')

    @classmethod
    def build(cls, source, name):
        out = cls.dir / name
        run([*CC, '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-no-pie',
             f'-DMUSIC_PACK_SOURCE="{source}"', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/music_pack_harness.c', 'pc/hal/music_decode.c', '-lm', '-ldl', '-o', str(out)],
            cwd=ROOT, timeout=120).require_ok()
        return out

    # music_pack.c before the state-machine fixes C4 / C5 (2026-11-03)
    BEFORE_C4_C5 = '84553342'

    def build_before(self, name):
        if not have_commit(self.BEFORE_C4_C5):
            self.skipTest(NO_HISTORY % self.BEFORE_C4_C5)
        path = self.dir / f'{name}.c'
        path.write_text(subprocess.run(['git', 'show', f'{self.BEFORE_C4_C5}:pc/hal/music_pack.c'], cwd=ROOT,
                                       capture_output=True, text=True, check=True).stdout)
        return self.build(path, name)

    def test_no_file_io_in_the_mixer(self):
        """C5: the pack's file is decoded ahead and the dump written by the
        game thread; music_pack_mix (the audio thread) touches no file.
        Negative control: music_pack.c before the fix."""
        d = self.dir / 'iodump'
        r = self.case('io', TOKI_MUSIC_PACK='t', TOKI_MUSIC_DUMP='1', TOKI_MUSIC_DIR=str(d))
        self.assertTrue(r.ok, r.stdout)
        self.assertIn('io_in_mix 0', r.stdout)
        with wave.open(str(d / 'dump' / f'{self.id}.wav')) as w:
            self.assertEqual(w.getnframes(), 400000)       # every frame reached the file
        (d / 'packs').symlink_to(self.dir / 'music/packs')
        old = self.build_before('before_c5')
        (d / 'dump' / f'{self.id}.wav').unlink()
        r = self.case('io', binary=old, TOKI_MUSIC_PACK='t', TOKI_MUSIC_DUMP='1', TOKI_MUSIC_DIR=str(d))
        self.assertFalse(r.ok)
        self.assertIn('CHECK io', r.stdout)

    def test_another_song_at_the_same_handle(self):
        """C4: a PLAY edge identifies the song by its bytes; negative
        control: the observer before the fix keeps the old id"""
        self.assertTrue(self.case('samehandle').ok)
        r = self.case('samehandle', binary=self.build_before('before_c4'))
        self.assertFalse(r.ok)
        self.assertIn('CHECK samehandle', r.stdout)

    def test_after_a_state_restore(self):
        """C4: after a restore the songs are identified again: another song
        stops the old file, the same song keeps it (a rewind does not restart
        the music).  Negative control: the hook doing nothing."""
        r = self.case('afterload', TOKI_MUSIC_PACK='t')
        self.assertTrue(r.ok, r.stdout)
        self.assertTrue(self.case('afterload-same', TOKI_MUSIC_PACK='t').ok)
        src = (ROOT / 'pc/hal/music_pack.c').read_text()
        self.assertEqual(src.count('    g_reverify = 1;\n'), 1)
        path = self.dir / 'mut_afterload.c'
        path.write_text(src.replace('    g_reverify = 1;\n', ''))
        r = self.case('afterload', binary=self.build(path, 'mut_afterload'), TOKI_MUSIC_PACK='t')
        self.assertFalse(r.ok)
        self.assertIn('CHECK afterload', r.stdout)

    def case(self, name, binary=None, arg=None, **env):
        e = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'MUSIC_'))}
        e.update(TOKI_MUSIC_DIR=str(self.dir / 'music'))
        e.update(env)
        return run([str(binary or self.binary), name, str(arg or self.seq)], cwd=self.dir, env=e,
                   timeout=60, max_output=64000)

    def test_c_id_matches_python(self):
        r = self.case('id').require_ok()
        tid, span = r.stdout.split()[:2]
        self.assertEqual(tid, self.id)
        self.assertEqual(int(span), music_tracks.seq_span(synthetic_seq()))

    def test_follows_the_game(self):
        log = self.dir / 'follow.jsonl'
        r = self.case('follow', TOKI_MUSIC_PACK='t', TOKI_MUSIC_LOG=str(log))
        self.assertTrue(r.ok, r.stdout)
        events = [json.loads(l)['ev'] for l in log.read_text().splitlines()]
        # the event log: every game call has its file-side answer
        want = ['open', 'file', 'start', 'vol', 'vol', 'pause', 'resume',
                'end', 'file_end', 'file', 'start', 'stop', 'file_stop', 'file', 'start',
                'close', 'file_stop']
        got = [e for e in events if e not in ('mvol', 'file_loop')]
        self.assertEqual(got, want)
        self.assertIn('file_loop', events)

    def test_fallback_without_a_file(self):
        self.assertTrue(self.case('fallback', TOKI_MUSIC_PACK='empty').ok)
        # negative control: with the file the same case is replaced
        r = self.case('fallback', TOKI_MUSIC_PACK='t')
        self.assertFalse(r.ok)
        self.assertIn('CHECK fallback', r.stdout)

    def test_profile_switch_off_ignores_the_pack(self):
        self.assertTrue(self.case('fallback', TOKI_MUSIC_PACK='t', MUSIC_PACK='0').ok)

    def test_dump_keeps_the_longest_take(self):
        d = self.dir / 'dumpdir'
        r = self.case('dump', TOKI_MUSIC_DUMP='1', TOKI_MUSIC_DIR=str(d))
        self.assertTrue(r.ok, r.stdout)
        path = d / 'dump' / f'{self.id}.wav'
        with wave.open(str(path)) as w:
            self.assertEqual((w.getnchannels(), w.getframerate(), w.getnframes()), (2, 44100, 3000))
            frames = struct.unpack('<6000h', w.readframes(3000))
        self.assertEqual(frames[2 * 999:2 * 1001], (999, -999, 0, 0))
        self.assertFalse(list(d.glob('dump/*.part')))
        heard = [json.loads(l) for l in (d / 'dump/heard.jsonl').read_text().splitlines()]
        self.assertTrue(any(e['ev'] == 'start' and e['id'] == self.id for e in heard))

    def test_settings_menu_page(self):
        out = self.case('menu', TOKI_MUSIC_PACK='', MUSIC_PACK='1').require_ok().stdout
        self.assertIn('page MUSIC PACKS items 3', out)
        self.assertIn('item page=0 type=5 label=MUSIC PACKS', out)
        self.assertIn('label=PACK key=audio.music_pack [OFF (ORIGINAL)] [empty] [t] note=', out)
        self.assertIn('label=DUMP SONGS key=audio.music_dump', out)
        # negative control: no packs folder -> only OFF, and the menu says so
        out = self.case('menu', TOKI_MUSIC_DIR=str(self.dir / 'nowhere'),
                        MUSIC_PACK='1').require_ok().stdout
        self.assertIn('[OFF (ORIGINAL)] note=NO PACKS FOUND', out)
        # parity (the switch off): the page says the pack is enhanced-only
        out = self.case('menu', MUSIC_PACK='0').require_ok().stdout
        self.assertIn('note=ENHANCED ONLY', out)

    def test_wav_smpl_loop(self):
        path = self.dir / 'smpl.wav'
        write_wav(path, ramp(5000), rate=22050, smpl=(100, 2999))
        out = self.case('decode', arg=path).require_ok().stdout
        self.assertIn('wav rate 22050 channels 2 frames 5000 decoded 5000 loop 100 3000', out)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'needs ffmpeg to make FLAC / Ogg files')
    def test_flac_and_ogg_with_loop_tags(self):
        src = self.dir / 'src.wav'
        write_wav(src, ramp(48000), rate=48000)
        ref = self.case('decode', arg=src).require_ok().stdout
        for ext, codec in (('flac', 'flac'), ('ogg', 'libvorbis')):
            path = self.dir / f'tagged.{ext}'
            run(['ffmpeg', '-v', 'error', '-y', '-i', str(src), '-c:a', codec,
                 '-metadata', 'LOOPSTART=1234', '-metadata', 'LOOPLENGTH=20000', str(path)],
                timeout=60).require_ok()
            out = self.case('decode', arg=path).require_ok().stdout
            self.assertIn(f'{ext} rate 48000 channels 2 frames 48000 decoded 48000 loop 1234 21234', out)
            if ext == 'flac':     # lossless: the same samples as the WAV
                self.assertEqual(out.split('sum')[1], ref.split('sum')[1])

    # (name, old text, new text, the check that must fail)
    MUTATIONS = [
        ('mask', '(owner & 0xFF) == g_cur', '(owner & 0xFF) == 32', 'CHECK mask'),
        ('volume', 'float gl = a->vol_l / rl, gr = a->vol_r / rr;', 'float gl = 1, gr = 1;',
         'CHECK volume'),
        ('mainvol', 'g_main_gain[0] = l / 32767.0f;', 'g_main_gain[0] = 1.0f;', 'CHECK mainvol'),
        ('pause', 'if (s->stopping || s->paused) return;', 'if (s->stopping) return;',
         'CHECK pause-hold'),
        ('loop', 'int64_t end = (s->may_loop && s->loop_allowed) ? s->loop_end : -1;',
         'int64_t end = -1;', 'CHECK loop'),
        ('end', 'if (g_stream) g_stream->may_loop = 0;', ';', 'CHECK end-plays-out'),
        ('stop', '                    event("stop", acc, a->id, NULL);\n                    song_stop(acc);',
         '                    event("stop", acc, a->id, NULL);', 'CHECK stop'),
        ('close', '                event("close", acc, a->id, NULL);\n                song_stop(acc);\n                a->open = 0;',
         '                event("close", acc, a->id, NULL);\n                a->open = 0;', 'CHECK close'),
        ('start-sync', 's->env = 1.0f;', 's->env = 0.0f;', 'CHECK start-pos'),
    ]

    def test_negative_controls(self):
        src = (ROOT / 'pc/hal/music_pack.c').read_text()
        for name, old, new, check in self.MUTATIONS:
            with self.subTest(name):
                self.assertEqual(src.count(old), 1, f'{name}: mutation target not found once')
                path = self.dir / f'mut_{name}.c'
                path.write_text(src.replace(old, new))
                binary = self.build(path, f'mut_{name}')
                r = self.case('follow', binary=binary, TOKI_MUSIC_PACK='t')
                self.assertFalse(r.ok, f'{name}: the mutation passed')
                self.assertIn(check, r.stdout, f'{name}: {r.stdout[-1500:]}')


class SpuSplitTests(unittest.TestCase):
    F = 22050

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='spu-split-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)

    def build(self, archive, name):
        out = self.dir / name
        run([*CC, '-std=gnu17', '-w', '-O1', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/spu_split_harness.c', 'pc/hal/spu_psx.c', str(archive),
             '-lstdc++', '-lm', '-o', str(out)], cwd=ROOT, timeout=120).require_ok()
        return out

    def results(self, binary):
        res = {}
        for mode in ('plain', 'zero', 'dump', 'replace', 'only0'):
            path = self.dir / f'{binary.name}-{mode}.bin'
            run([str(binary), mode, str(path)], timeout=60).require_ok()
            b = path.read_bytes()
            f = self.F * 4
            res[mode] = dict(out=struct.unpack(f'<{self.F * 2}h', b[:f]),
                             music=struct.unpack(f'<{self.F * 2}h', b[f:2 * f]),
                             ram=b[2 * f:2 * f + 0x80000], regs=b[2 * f + 0x80000:])
        return res

    def test_split_changes_only_the_output(self):
        r = self.results(self.build(spu_core.archive(), 'split'))
        plain = r['plain']
        self.assertGreater(max(abs(v) for v in plain['out']), 1000)
        for mode in ('zero', 'dump', 'replace'):
            self.assertEqual(r[mode]['ram'], plain['ram'], f'{mode}: SPU RAM changed')
            self.assertEqual(r[mode]['regs'], plain['regs'], f'{mode}: device registers changed')
        self.assertEqual(r['zero']['out'], plain['out'])
        self.assertEqual(r['dump']['out'], plain['out'])
        self.assertEqual(r['replace']['out'], r['only0']['out'])
        self.assertNotEqual(r['replace']['out'], plain['out'])
        diff = [m - (p - o) for m, p, o in zip(r['dump']['music'], plain['out'], r['only0']['out'])]
        self.assertLessEqual(max(abs(d) for d in diff), 2)
        self.assertGreater(max(abs(v) for v in r['dump']['music']), 1000)
        self.assertEqual(set(r['plain']['music']), {0})

    def test_negative_control_shadow_reverb_writes_the_real_work_area(self):
        src = self.dir / 'core'
        spu_core.copy(src)
        cpp = src / 'upstream/spu.cpp'
        text = cpp.read_text()
        old = ('std::memcpy((s_pc_split.reverb_ram ? s_pc_split.reverb_ram : s_ram.data()) '
               '+ real_address, &data, sizeof(data));')
        self.assertEqual(text.count(old), 1)
        cpp.write_text(text.replace(old, 'std::memcpy(s_ram.data() + real_address, &data, sizeof(data));'))
        r = self.results(self.build(spu_core.build(src), 'broken'))
        self.assertNotEqual(r['replace']['ram'], r['plain']['ram'])


def run_port(work, frames, env, scene=None, script=None):
    work.mkdir(parents=True, exist_ok=True)
    (work / 'pc').mkdir(exist_ok=True)
    for name in ('pc/assets', 'disks'):
        if not os.path.lexists(work / name):
            os.symlink(disc_input(name), work / name)
    e = {k: v for k, v in os.environ.items()
         if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'MUSIC_'))}
    e.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
             PC_TRACE=str(work / 't.trace'), PC_TRACE_FRAMES=str(frames),
             PC_PARITY_DIR=str(work / 'parity'), PC_PARITY_FRAMES='300,900,1500,1800',
             PC_PARITY_RAM='1', PC_FRAME_HASH=str(work / 'frames.txt'),
             TOKI_MUSIC_LOG=str(work / 'music.jsonl'), SCENE_NAV=str(scene))
    e.update(env)
    r = run([str(PC_BIN), str(ROOT / 'pc/assets')], cwd=work, env=e, timeout=900,
            log_path=str(work / 'log.txt'))
    if not r.ok:
        raise RuntimeError(f'{work.name}: {r.reason or r.returncode}')
    return work


def pcm(path):
    data = Path(path).read_bytes()
    return struct.unpack(f'<{len(data) // 2}h', data)


def events(path):
    return [json.loads(l) for l in Path(path).read_text().splitlines()]


ARTIFACTS = ['parity/state.jsonl', 'frames.txt', 't.trace'] + [
    f'parity/frame_{n:06d}.{k}' for n in (300, 900, 1500, 1800) for k in ('vram', 'ram', 'ppm')]


@unittest.skipUnless(HAVE_GAME, 'needs the built port and your pc/assets')
class GameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='music-game-')
        cls.addClassCleanup(cls.tmp.cleanup)
        d = cls.dir = Path(cls.tmp.name)
        cls.music = d / 'music'
        enh = dict(TOKI_PROFILE='enhanced', TOKI_MUSIC_DIR=str(cls.music))
        cls.dump = run_port(d / 'dump', 2700, dict(enh, TOKI_MUSIC_DUMP='1'), scene=18)
        cls.plain = run_port(d / 'plain', 2700, enh, scene=18)
        heard = [e for e in events(cls.dump / 'music.jsonl') if e['ev'] == 'start']
        cls.title = heard[0]['id']
        for mode in ('copy', 'silence', 'tone'):
            music_tracks.main(['synth-pack', '--dump', str(cls.music / 'dump'),
                               '--out', str(cls.music / 'packs' / mode), '--mode', mode,
                               '--only', cls.title])
        cls.copy = run_port(d / 'copy', 2700, dict(enh, TOKI_MUSIC_PACK='copy'), scene=18)
        cls.silence = run_port(d / 'silence', 2700, dict(enh, TOKI_MUSIC_PACK='silence'), scene=18)
        cls.off = run_port(d / 'off', 2700, dict(enh, TOKI_MUSIC_PACK='copy', MUSIC_PACK='0'), scene=18)
        par = dict(TOKI_MUSIC_DIR=str(cls.music))
        cls.parity = run_port(d / 'parity', 2700, par, scene=18)
        cls.parity_pack = run_port(d / 'parity_pack', 2700, dict(par, TOKI_MUSIC_PACK='copy'), scene=18)

    def same(self, a, b, name):
        x, y = (a / name).read_bytes(), (b / name).read_bytes()
        if name.endswith('.ram'):
            # the game's state: host-stack residue (ps1_residue.py) masked;
            # it follows the host's call depth, which a pack changes
            x, y = ps1_residue.mask(x), ps1_residue.mask(y)
        return x == y

    def song_start(self, run_dir):
        """The first audible frame of the song in the capture window."""
        a = pcm(run_dir / 'parity/audio.s16le')
        return next(i for i in range(0, len(a), 2) if a[i] or a[i + 1]) // 2

    def test_track_id_is_stable_and_the_discs(self):
        key = lambda e: (e['ev'], e['vbl'], e['id'])
        a = [key(e) for e in events(self.dump / 'music.jsonl') if e['ev'] in ('open', 'start', 'close')]
        b = [key(e) for e in events(self.plain / 'music.jsonl') if e['ev'] in ('open', 'start', 'close')]
        self.assertTrue(a)
        self.assertEqual(a, b)
        tracks = music_tracks.scan(str(ROOT / 'pc/assets/raw/CDROM'))
        self.assertIn(self.title, tracks)
        # parity starts the same song (later): same id
        p = [e['id'] for e in events(self.parity / 'music.jsonl') if e['ev'] == 'start']
        self.assertEqual(p[0], self.title)

    def test_dump_is_the_song(self):
        with wave.open(str(self.music / 'dump' / f'{self.title}.wav')) as w:
            n = w.getnframes()
            dump = struct.unpack(f'<{n * 2}h', w.readframes(n))
        out = pcm(self.plain / 'parity/audio.s16le')
        i = next(k for k in range(0, len(dump), 2) if dump[k] or dump[k + 1])
        j = self.song_start(self.plain) * 2
        m = min(len(dump) - i, len(out) - j)
        self.assertGreater(m, 44100 * 2 * 20)
        self.assertLessEqual(max(abs(dump[i + k] - out[j + k]) for k in range(m)), 2)

    def test_copy_pack_reproduces_the_original(self):
        a, b = pcm(self.plain / 'parity/audio.s16le'), pcm(self.copy / 'parity/audio.s16le')
        with wave.open(str(self.music / 'dump' / f'{self.title}.wav')) as w:
            n = w.getnframes()
        s = self.song_start(self.plain)
        # the dump starts at the song's start event, a little before its first note
        end = min(len(a), (s + n - 44100) * 2)
        self.assertLessEqual(max(abs(a[k] - b[k]) for k in range(end)), 2)
        self.assertNotEqual(a, b)          # it is the file playing, not the voices

    def test_silent_pack_mutes_the_song(self):
        a, b = pcm(self.plain / 'parity/audio.s16le'), pcm(self.silence / 'parity/audio.s16le')
        rms = math.sqrt(sum(v * v for v in a) / len(a))
        self.assertGreater(rms, 500)       # negative control: the song is there without the pack
        self.assertEqual(max(abs(v) for v in b), 0)

    def test_game_state_identical_with_and_without_a_pack(self):
        for other in (self.copy, self.silence, self.dump, self.off):
            for name in ARTIFACTS:
                with self.subTest(run=other.name, artifact=name):
                    self.assertTrue(self.same(self.plain, other, name))
        # negative control: the comparison sees the libsnd state the game
        # polls -- the RAM checkpoints hold the score record, playing
        ram = (self.plain / 'parity/frame_000900.ram').read_bytes()
        handle = struct.unpack_from('<I', ram, 0x131380)[0] & 0x1FFFFF
        flags = struct.unpack_from('<I', ram, handle + 144)[0]
        self.assertEqual(flags & 1, 1)
        self.assertFalse(self.same(self.plain, self.parity, 'frames.txt'))

    def test_switch_off_and_parity_ignore_the_pack(self):
        self.assertTrue(self.same(self.plain, self.off, 'parity/audio.s16le'))
        self.assertTrue(self.same(self.parity, self.parity_pack, 'parity/audio.s16le'))
        for name in ARTIFACTS:
            self.assertTrue(self.same(self.parity, self.parity_pack, name), name)

    def test_fade_and_close_follow_the_game(self):
        script = self.dir / 'title_fade.txt'
        script.write_text('hold 2\n\nphase title_fade\n'
                          'wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000\n'
                          '+120 start\n'
                          'wait album_prompt now_step=0x13 now_sub_step=2 now_sub_sub_step=0x41 within 3000\n'
                          '+300 circle\n')
        runs = {}
        for name, extra in (('nopack', []), ('tone', ['--set', 'TOKI_MUSIC_PACK=tone'])):
            out = self.dir / f'fade-{name}'
            cmd = [sys.executable, str(ROOT / 'pc/tools/playthrough.py'), str(script),
                   '--out', str(out), '--profile', 'enhanced', '--max-frames', '3000',
                   '--set', f'TOKI_MUSIC_DIR={self.music}',
                   '--set', f'TOKI_MUSIC_LOG={out}.jsonl', '--set', f'PC_AUDIO_DUMP={out}.raw',
                   '--set', f'PC_FRAME_HASH={out}.frames', *extra]
            r = run(cmd, cwd=ROOT, timeout=900)
            self.assertTrue(r.ok, r.stdout[-2000:])
            runs[name] = out
        for suffix in ('.frames',):
            self.assertEqual(Path(str(runs['nopack']) + suffix).read_bytes(),
                             Path(str(runs['tone']) + suffix).read_bytes())
        self.assertEqual((runs['nopack'] / 'title_fade.pc.trace').read_bytes(),
                         (runs['tone'] / 'title_fade.pc.trace').read_bytes())
        ev = events(str(runs['tone']) + '.jsonl')
        starts = [e for e in ev if e['ev'] == 'start']
        self.assertEqual(starts[0]['id'], self.title)
        self.assertTrue(starts[0]['file'])
        self.assertTrue(any(e['ev'] == 'file' and e['vbl'] == starts[0]['vbl'] for e in ev))
        close = next(e for e in ev if e['ev'] == 'close')
        self.assertTrue(any(e['ev'] == 'file_stop' and e['vbl'] == close['vbl'] for e in ev))
        self.assertGreater(len(starts), 1)
        self.assertNotEqual(starts[1]['id'], self.title)
        self.assertFalse(starts[1]['file'])            # not in the pack: the original
        vols = [(e['vbl'], e['vol'][0]) for e in ev if e['ev'] == 'vol' and e['id'] == self.title]
        self.assertGreater(len(vols), 20)
        start_vol = starts[0]['vol'][0]

        def tone(raw, v):
            with open(raw, 'rb') as fh:
                fh.seek(int(v * 44100 * 1001 / 60000) * 4)
                data = fh.read(2205 * 4)
            left = struct.unpack(f'<{len(data) // 2}h', data)[::2]
            re_ = sum(x * math.cos(2 * math.pi * 1000 * i / 44100) for i, x in enumerate(left))
            im = sum(x * math.sin(2 * math.pi * 1000 * i / 44100) for i, x in enumerate(left))
            return math.hypot(re_, im) * 2 / len(left)

        def vol_at(v):
            vs = [vo for vv, vo in vols if vv <= v]
            return vs[-1] if vs else start_vol

        fade = [v for v, _ in vols[::8]]
        measured = [tone(str(runs['tone']) + '.raw', v) for v in fade]
        model = [6000 * vol_at(v) / start_vol for v in fade]
        err = max(abs(m - x) for m, x in zip(measured, model))
        self.assertLess(err, 700, list(zip(fade, measured, model)))
        # negative controls: the run without the pack has no tone; a constant
        # level does not fit the fade
        self.assertLess(max(tone(str(runs['nopack']) + '.raw', v) for v in fade), 700)
        self.assertGreater(max(abs(m - 6000) for m in measured), 3000)
        after = tone(str(runs['tone']) + '.raw', close['vbl'] + 10)
        self.assertLess(after, 50)


if __name__ == '__main__':
    unittest.main()
