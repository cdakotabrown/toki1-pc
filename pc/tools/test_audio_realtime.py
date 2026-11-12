# SPDX-License-Identifier: GPL-3.0-or-later
"""The real-time audio device: what a player hears (2026-11-02 report from
real Windows 11: "the audio is skipping in odd ways ... almost like it's
missing a channel", music and sound effects broken from the first screen,
both profiles).

Root cause: the SPU core latches KON/KOFF until its next sample batch and a
second write replaces the first; the device thread mixed 1,024 frames per
callback while the game thread forwarded each VBlank's writes, so two
VBlanks' key-ons met between two callbacks and the first one's notes were
never played (and key-offs were lost, leaving notes on).  Fix: the voice and
key registers go through a queue the device thread replays on the emulated
time line (pc/hal/spu.c, "The real-time register queue").

  CadenceTests  pc/tests/spu_cadence_harness.c: the real audio_callback and
                spu_mmio_sync under simulated host cadences -- steady or
                bursty game VBlanks x fixed 1,024-frame or odd-sized paired
                device requests (WASAPI behind SDL's resampler) -- 120 notes
                must sound exactly once.  Negative control:
                TOKI_SPU_LATENCY_MS=0 (direct writes) loses notes.
                The `io` case: no stdio call inside the device callback with
                PC_AUDIO_HASH / PC_AUDIO_DUMP on; negative control: the same
                calls through the game-thread entry are seen by the spy.
  GameTests     the game in real time (SDL disk driver, no turbo,
                pc/tools/audio_realtime.py): title song to the name entry
                in enhanced and parity: no key write lost, no dropout, both
                channels, the device format logged; the deterministic mix of
                the same script is unchanged by the queue (XA drops and the
                envelope against the deterministic mix are reported by
                audio_realtime.py, not asserted: the disk driver's clock
                slows under load).  Negative
                control: TOKI_SPU_LATENCY_MS=0 loses key-ons in real time.
                SoundFont mode in real time (a generated SF2): nothing lost,
                every music key-on a SoundFont note, no unknown tag; the
                48 kHz device (output_rate): obtained 48000, nothing lost.
                PC_TEST_PLATFORM=windows runs the Windows exe under Wine.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor

from safe_process import run
import audio_realtime as ar
import game_platform
import make_test_sf2
import spu_core

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/test-audio-realtime' / game_platform.PLATFORM
HAVE_GAME = game_platform.game_available() and (ROOT / 'pc/assets/raw').exists()

# new_game.txt's opening (test_music_overlap.NAME_ENTRY): the title song,
# the album (card select), the name entry's song.
NAME_ENTRY = """hold 2
phase name_entry
wait title_menu now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000
+30 start
wait album_prompt now_step=0x13 now_sub_step=2 now_sub_sub_step=0x41 within 3000
+60 circle
wait album_pick now_step=0x13 now_sub_step=2 now_sub_sub_step=0x45 within 600
+30 left+up*80
+82 right*10
+12 down*29
+31 circle
wait sysfile_msg now_step=0x13 now_sub_step=2 now_sub_sub_step=0x47 within 600
+30 circle
wait sysfile_prompt now_step=0x13 now_sub_step=2 now_sub_sub_step=0x49 within 600
+30 circle
wait surname_entry now_step=0x14 now_sub_step=1 now_sub_sub_step=1 within 1200
end 240
"""


class CadenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='spu-cadence-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'spu-cadence'
        source = Path(os.environ.get('SPU_SOURCE', ROOT / 'pc/hal/spu.c')).resolve(strict=True)
        archive = spu_core.archive().resolve(strict=True)
        sdl = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2'], timeout=10).require_ok().stdout)
        cc = shlex.split(os.environ.get('CC', 'cc'))
        run([*cc, '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-ffunction-sections', '-fdata-sections',
             '-DSPU_SOURCE="' + str(source) + '"', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/spu_cadence_harness.c', 'pc/hal/cd_audio.c', 'pc/hal/spu_psx.c', str(archive),
             '-Wl,--gc-sections', '-Wl,--unresolved-symbols=ignore-in-object-files',
             '-lstdc++', '-lm', '-ldl', *sdl, '-no-pie', '-o', str(cls.binary)],
            cwd=ROOT, timeout=120).require_ok()

    def case(self, game, device, latency=None, extra=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
        if latency is not None:
            env['TOKI_SPU_LATENCY_MS'] = str(latency)
        env.update(extra or {})
        r = run([str(self.binary), game, device], cwd=ROOT, env=env, timeout=60, max_output=16000)
        return r.returncode, r.stdout

    def test_every_note_sounds_under_every_cadence(self):
        for game in ('steady', 'bursty'):
            for device in ('fixed', 'odd'):
                with self.subTest(game=game, device=device):
                    rc, out = self.case(game, device)
                    self.assertEqual(rc, 0, out)
                    self.assertIn('notes 120 onsets 120 kon_lost 0 koff_lost 0 stuck 0', out)

    def test_negative_control_direct_writes_lose_notes(self):
        for game in ('steady', 'bursty'):
            for device in ('fixed', 'odd'):
                with self.subTest(game=game, device=device):
                    rc, out = self.case(game, device, latency=0)
                    self.assertEqual(rc, 1, out)
                    self.assertNotIn('onsets 120 kon_lost 0 koff_lost 0 stuck 0', out)

    def test_a_flush_merges_keys(self):
        """an SPU upload applies the queue at once: the key-ons merge, a
        key-off cancels its pending key-on (negative control: the same
        writes straight to the core keep only the last key-on)"""
        rc, out = self.case('flush', 'merged')
        self.assertEqual(rc, 0, out)
        self.assertIn('pending KON 0x000C', out)
        rc, out = self.case('flush', 'plain')
        self.assertEqual(rc, 1, out)
        self.assertIn('pending KON 0x0008', out)

    def mutated(self, name, old, new):
        src = (ROOT / 'pc/hal/spu.c').read_text()
        self.assertEqual(src.count(old), 1, name)
        path = Path(self.directory.name) / f'{name}.c'
        path.write_text(src.replace(old, new))
        binary = Path(self.directory.name) / name
        sdl = shlex.split(run(['pkg-config', '--cflags', '--libs', 'sdl2'], timeout=10).require_ok().stdout)
        cc = shlex.split(os.environ.get('CC', 'cc'))
        run([*cc, '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-ffunction-sections', '-fdata-sections',
             '-DSPU_SOURCE="' + str(path) + '"', '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/spu_cadence_harness.c', 'pc/hal/cd_audio.c', 'pc/hal/spu_psx.c',
             str(spu_core.archive().resolve()),
             '-Wl,--gc-sections', '-Wl,--unresolved-symbols=ignore-in-object-files',
             '-lstdc++', '-lm', '-ldl', *sdl, '-no-pie', '-o', str(binary)], cwd=ROOT, timeout=120).require_ok()
        return binary

    def run_case(self, binary, a, b):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
        r = run([str(binary), a, b], cwd=ROOT, env=env, timeout=60, max_output=16000)
        return r.returncode, r.stdout

    def test_a_restore_drops_the_queue(self):
        """C1: after a quickload / rewind (spu_hal_after_restore) the old
        time line's queued key-on never reaches the restored SPU; negative
        control: without the hook the clock jump applies it"""
        rc, out = self.case('restore', 'hook')
        self.assertEqual(rc, 0, out)
        self.assertIn('pending KON 0x0000', out)
        rc, out = self.case('restore', 'none')
        self.assertEqual(rc, 1, out)
        self.assertIn('pending KON 0x0002', out)

    def test_fast_forward_bypasses_the_queue_and_trims_xa(self):
        """C6: while fast-forward is held the writes go straight to the core,
        and on the release the XA FIFO keeps only its margin; negative
        controls: each half removed"""
        rc, out = self.case('ffqueue', '-')
        self.assertEqual(rc, 0, out)
        self.assertIn('queued during ff 0 ', out)
        b = self.mutated('noffqueue', ' && !(hal_ff_active && hal_ff_active());', ';')
        rc, out = self.run_case(b, 'ffqueue', '-')
        self.assertEqual(rc, 1, out)
        self.assertNotIn('queued during ff 0 ', out)
        b = self.mutated('notrim', '        if (realtime && ff_was && !ff) cd_audio_trim(cd_audio_margin_frames() + 10u);\n', '')
        rc, out = self.run_case(b, 'ffqueue', '-')
        self.assertEqual(rc, 1, out)
        self.assertIn('queued during ff 0 ', out)

    def test_an_upload_keeps_a_short_note(self):
        """C7: a bank upload no longer flushes the queue, so a note keyed and
        released a VBlank apart both still queued keeps its 16.7 ms; negative
        control: the flush restored cancels it"""
        rc, out = self.case('upload', '-')
        self.assertEqual(rc, 0, out)
        self.assertIn('cancelled 0 ', out)
        b = self.mutated('uploadflush', '    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);\n    /* The queue is NOT flushed here',
                         '    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);\n    rt_flush_locked();\n    /* The queue is NOT flushed here')
        rc, out = self.run_case(b, 'upload', '-')
        self.assertEqual(rc, 1, out)
        self.assertIn('cancelled 1 ', out)

    def test_a_reset_write_comes_after_the_queue(self):
        """C12: spu_reset_write applies the queued writes first; negative
        control: without that the queued write lands after the reset"""
        rc, out = self.case('resetwrite', '-')
        self.assertEqual(rc, 0, out)
        self.assertIn('voice 0 volume 0x0000', out)
        b = self.mutated('noresetflush', '    rt_flush_locked();\n    spu_psx_write_reg(0x1f801c00u + offset, value);',
                         '    spu_psx_write_reg(0x1f801c00u + offset, value);')
        rc, out = self.run_case(b, 'resetwrite', '-')
        self.assertEqual(rc, 1, out)
        self.assertIn('voice 0 volume 0x1234', out)

    def test_no_file_io_in_the_device_callback(self):
        with tempfile.TemporaryDirectory(prefix='spu-io-') as d:
            files = {'PC_AUDIO_HASH': f'{d}/hash.txt', 'PC_AUDIO_DUMP': f'{d}/out.raw'}
            rc, out = self.case('io', 'device', extra=files)
            self.assertEqual(rc, 0, out)
            self.assertIn('io_in_callback 0', out)
            calls = int(out.split('callbacks ')[1].split()[0])
            lines = Path(files['PC_AUDIO_HASH']).read_text().splitlines()
            self.assertEqual(len(lines), calls, 'the game thread wrote every hash line')
            self.assertEqual(Path(files['PC_AUDIO_DUMP']).stat().st_size, calls * 1024 * 4)
            # negative control: the same files written from inside the call
            rc, out = self.case('io', 'direct', extra=files)
            self.assertEqual(rc, 1, out)
            self.assertNotIn('io_in_callback 0', out)


@unittest.skipUnless(HAVE_GAME, 'needs the built port and your pc/assets')
class GameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        script = OUT / 'name_entry.txt'
        script.write_text(NAME_ENTRY)
        # SoundFont mode and the 48 kHz device (pc/hal/soundfont.c,
        # audio_resample.c): a generated SoundFont, no game data
        music = OUT / 'music'
        (music / 'soundfonts').mkdir(parents=True, exist_ok=True)
        (music / 'soundfonts' / 'sine.sf2').write_bytes(make_test_sf2.build())
        sf = [f'TOKI_MUSIC_DIR={music}', 'TOKI_SOUNDFONT=sine.sf2', f'TOKI_SF2_TRACE={OUT / "rt_soundfont.sf2"}']
        jobs = {
            'rt_soundfont': dict(profile='enhanced', settings=sf),
            'rt_48k': dict(profile='enhanced', settings=['TOKI_AUDIO_RATE=48000']),
            'rt_enhanced': dict(profile='enhanced'),
            'rt_parity': dict(profile='parity'),
            'rt_enhanced_direct': dict(profile='enhanced', settings=['TOKI_SPU_LATENCY_MS=0']),
            'det_enhanced': dict(profile='enhanced', deterministic=True),
            'det_enhanced_direct': dict(profile='enhanced', deterministic=True,
                                        settings=['TOKI_SPU_LATENCY_MS=0']),
        }
        with ThreadPoolExecutor(len(jobs)) as ex:
            futs = {k: ex.submit(ar.capture, script, OUT / k, **v) for k, v in jobs.items()}
            cls.r = {k: f.result() for k, f in futs.items()}

    def health(self, name):
        h = ar.health(self.r[name]['log'])
        self.assertIsNotNone(h, f'{name}: no [AUDIO] health line ({self.r[name]["dir"]})')
        return h

    def test_device_format_is_logged_and_as_asked(self):
        d = ar.device(self.r['rt_enhanced']['log'])
        self.assertIsNotNone(d)
        self.assertEqual(d['driver'], 'disk')
        self.assertEqual(d['have'], (44100, 2, 1024))

    def test_no_key_write_lost_in_real_time(self):
        for name in ('rt_enhanced', 'rt_parity'):
            with self.subTest(run=name):
                h = self.health(name)
                self.assertGreater(h['callbacks'], 1000)
                self.assertGreater(h['queued'], 1000, 'voice writes go through the queue')
                self.assertGreater(h['kon_writes'], 100)
                self.assertEqual((h['kon_lost'], h['koff_lost'], h['kon_koff']), (0, 0, 0))

    def test_soundfont_in_real_time(self):
        """SoundFont mode in real time: no key write lost, every music key-on
        became a SoundFont note when the core took it (the queue carries the
        snapshot's tag), no unknown tag, no dropout"""
        h = self.health('rt_soundfont')
        self.assertEqual((h['kon_lost'], h['koff_lost'], h['kon_koff']), (0, 0, 0))
        self.assertGreater(h['queued'], 1000)
        log = self.r['rt_soundfont']['log']
        self.assertIn('[SF2] loaded', log)
        rows = [{k: int(v) for k, v in zip(r[1::2], r[2::2]) if k in ('keys', 'on', 'unknown')} for r in
                (l.split() for l in (OUT / 'rt_soundfont.sf2').read_text().splitlines())]
        self.assertGreater(rows[-1]['keys'], 100)
        self.assertEqual(rows[-1]['unknown'], 0)
        # a key-on reaches the font when the device applies its queued KON:
        # never more than the key-ons of the last second are still pending
        # (latency 50 ms + adaptive <= 150 ms + a loaded host), none is lost
        for i, r in enumerate(rows):
            self.assertLessEqual(r['on'], r['keys'])
            self.assertLessEqual(r['keys'] - r['on'], r['keys'] - rows[max(0, i - 60)]['keys'], r)
        a = ar.analyze(self.r['rt_soundfont']['raw'])
        self.assertGreater(a['seconds'], 30)
        self.assertEqual(a['gaps'], [])
        self.assertEqual(a['repeats'], [])

    def test_48k_device(self):
        """[audio] output_rate = 48000: the device runs at 48 kHz (the
        resampler feeds it), nothing lost"""
        d = ar.device(self.r['rt_48k']['log'])
        self.assertIsNotNone(d)
        self.assertEqual(d['have'][0], 48000)
        self.assertEqual(self.r['rt_enhanced'] and ar.device(self.r['rt_enhanced']['log'])['have'][0], 44100)
        self.assertIn('[SPU] output rate 48000 Hz', self.r['rt_48k']['log'])
        h = self.health('rt_48k')
        self.assertEqual((h['kon_lost'], h['koff_lost'], h['kon_koff']), (0, 0, 0))
        raw = self.r['rt_48k']['raw']
        self.assertGreater(raw.stat().st_size / 4 / 48000, 30)

    def test_negative_control_direct_writes_lose_key_ons(self):
        h = self.health('rt_enhanced_direct')
        self.assertEqual(h['queued'], 0)
        self.assertGreater(h['kon_lost'] + h['koff_lost'], 0)

    def test_no_dropouts_and_both_channels(self):
        for name in ('rt_enhanced', 'rt_parity'):
            with self.subTest(run=name):
                raw = self.r[name]['raw']
                self.assertTrue(raw.exists(), f'{name}: no capture')
                a = ar.analyze(raw)
                self.assertGreater(a['seconds'], 30)
                self.assertEqual(a['gaps'], [])
                self.assertEqual(a['repeats'], [])
                self.assertEqual(a['stereo'], [])
        # Not asserted: XA sectors dropped / FIFO gaps.  The disk driver's
        # clock is SDL_Delay(23 ms) plus the mixing time, so on a loaded
        # host (the battery runs this beside the matrix and playthroughs)
        # it falls more than 0.5 s behind the game and the real-time XA
        # FIFO drops sectors -- a property of the capture driver, not of a
        # hardware device.  The XA path is unchanged here and covered by
        # test_cd_audio (RealTimeMarginTests); the health line reports it.

    def test_deterministic_mix_is_untouched(self):
        """the queue is real-time only: the game-thread mix is the same
        with it switched off"""
        a = self.r['det_enhanced']['raw'].read_bytes()
        b = self.r['det_enhanced_direct']['raw'].read_bytes()
        self.assertTrue(a)
        self.assertEqual(a, b)


if __name__ == '__main__':
    unittest.main()
