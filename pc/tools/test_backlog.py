# SPDX-License-Identifier: GPL-3.0-or-later
"""The enhanced profile's dialogue backlog and voice replay
(pc/hal/backlog_capture.c, backlog.c, backlog_view.c, voice_replay.c).

Two layers.

Harness (pc/tests/backlog_harness.c, no game data): a synthetic text engine,
font and XA file drive the real capture, list and decoder --
  - every glyph bitmap maps back to its canonical Shift-JIS code although
    the font's index spans overlap (as the game's do);
  - a scripted conversation captures: the voiced page once (the blinking
    arrow does not repeat it) with the line it started, without the menu
    above the message window; the page the player confirmed through (no
    arrow) at its reset, without a line that started before it; an arrow
    narration page; not a narration page that was never waited on; not the
    same page shown again;
  - parity captures nothing;
  - the list keeps the newest 200 pages; the view's input;
  - the replay decoder gives exactly the PCM the CD controller's own path
    (cd_audio_xa_sector, filter on) hands the SPU for the same sectors.
Negative controls: each MUTATIONS entry rebuilds the harness from a copy of
a source with one mechanism broken and requires its mode to fail.

Game (the built port and your pc/assets; skipped without them): Shiori's
phone call of date_save.txt on the enhanced profile --
  - the backlog captures the call's pages: valid Shift-JIS codes, the
    player's name (the one the script types) as speaker of the player's
    lines, the girl's lines with their voice from CDROM/XA_G, and the
    replay of a captured line decodes to the controller's PCM over the
    real disc asset;
  - frame (VRAM, scanout, PS1 RAM) and game-audio hashes are identical
    with the backlog off, on, and on while it is opened, scrolled, a voice
    is replayed in it and outside it (Ctrl+R) and it is closed; the
    presenter only inserts its own `backlog` windows (and `toast` notes); the mixed output
    differs from the run without the replay exactly by the replayed line;
  - parity has no backlog;
  - negative control: a PC-side assist that does press for the player
    (TOKI_TEXT_AUTO) changes the hashes, so the comparison sees a game that
    noticed.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

from safe_process import run
import game_platform  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BINARY = game_platform.GAME   # PC_TEST_PLATFORM=windows: the .exe under Wine
ASSETS = ROOT / 'pc/assets'
SOURCES = ('pc/hal/backlog_capture.c', 'pc/hal/backlog.c', 'pc/hal/voice_replay.c',
           'pc/hal/cd_audio.c', 'pc/hal/profile.c', 'pc/hal/menu.c')

# (name, file, old, new, mode that must then fail)
MUTATIONS = (
    ('overlapping font spans resolved first-come', 'pc/hal/backlog_capture.c',
     'if (font_range(k + 1, &nstart, &nend, &nbase) && nbase < last) last = nbase;',
     '(void)nstart; (void)nend; (void)nbase;', 'font'),
    ('a page confirmed through is lost', 'pc/hal/backlog_capture.c',
     'if (g_cand == 1 && reset && g_page.speaker >= 0) take();', '', 'capture'),
    ('any page taken down is captured', 'pc/hal/backlog_capture.c',
     'if (g_cand == 1 && reset && g_page.speaker >= 0) take();',
     'if (g_cand == 1 && reset) take();', 'capture'),
    ('a line from before the page is attached', 'pc/hal/backlog_capture.c',
     '        g_stream.frame >= g_base_frame) {', '        1) {', 'capture'),
    ('the menu above the window is kept', 'pc/hal/backlog_capture.c',
     '    if (p->speaker < 0) return;\n    top', '    return;\n    top', 'capture'),
    ('the same page twice', 'pc/hal/backlog_capture.c',
     'if (sig == g_last_sig && g_page.screen == g_last_sig_screen) return;', '', 'capture'),
    ('parity captures', 'pc/hal/profile.c',
     '{ "TOKI_BACKLOG",       0, 1,', '{ "TOKI_BACKLOG",       1, 1,', 'parity'),
    ('no 200-page cap', 'pc/hal/backlog.c',
     'if (g_count == BACKLOG_MAX_PAGES) {', 'if (g_count == BACKLOG_MAX_PAGES + 1) {', 'list'),
    ('the replay takes every channel', 'pc/hal/voice_replay.c',
     'raw[0] != v->file || raw[1] != v->channel) continue;', 'raw[0] != v->file) continue;',
     'decode'),
    ('the replay keeps a stale decoder', 'pc/hal/voice_replay.c',
     '    cd_xa_decoder_reset(&dec);\n', '    memset(&dec, 0x11, sizeof dec);\n', 'decode'),
)

CAPTURE_WANT = [
    # the girl, voiced: name / bracket / line, the line's whole request window
    'page 1 screen=450101 speaker=0 voice=1 lba=1099..1259 req=1083..1275 chan=3 '
    'segs=-121,50:85418542/-86,50:81758441/-72,66:844284438144',
    # the player, confirmed through: captured at the reset, no voice
    'page 2 screen=450101 speaker=0 voice=0 segs=-121,50:82a082a2/-86,50:81758441',
    # narration that waited on the arrow
    'page 3 screen=450101 speaker=-1 voice=0 segs=-128,50:86418642',
]


def build(folder, mutation=None):
    folder = Path(folder)
    paths = {s: ROOT / s for s in SOURCES}
    if mutation:
        _, path, old, new, _ = mutation
        text = (ROOT / path).read_text()
        assert text.count(old) == 1, f'mutation anchor not unique in {path}: {old!r}'
        copy = folder / Path(path).name
        copy.write_text(text.replace(old, new))
        paths[path] = copy
    binary = folder / 'backlog_harness'
    run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-w', '-O1', '-fno-pie', '-no-pie',
         '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', '-include', 'pc/include/game/types.h',
         str(ROOT / 'pc/tests/backlog_harness.c'), *[str(paths[s]) for s in SOURCES],
         '-o', str(binary)], cwd=ROOT, timeout=120).require_ok()
    return binary


def harness(binary, mode, *args, profile='enhanced'):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOKI_', 'PC_'))}
    env['TOKI_CONFIG'] = '/dev/null'
    if profile:
        env['TOKI_PROFILE'] = profile
    r = run([str(binary), mode, *args], cwd=ROOT, env=env, timeout=120)
    out = [l for l in r.stdout.splitlines() if not l.startswith('[')]
    return r.returncode, out


def mode_ok(binary, mode):
    """-> (passes, detail) for one harness mode."""
    if mode == 'font':
        rc, out = harness(binary, 'font')
        return rc == 0 and out[-1:] == ['font ok 7585'], out[-3:]
    if mode == 'capture':
        rc, out = harness(binary, 'capture')
        return rc == 0 and out == CAPTURE_WANT, out
    if mode == 'parity':
        rc, out = harness(binary, 'capture', profile=None)
        return rc == 0 and out == [], out
    if mode == 'list':
        rc, out = harness(binary, 'list')
        want = ['count=200 first=6 last=205', 'open=1 sel=199', 'up3=196', 'pgdn=199',
                'home=0', 'pgup=0', 'playing=0', 'open=0']
        return rc == 0 and out == want, out
    if mode == 'decode':
        rc, out = harness(binary, 'decode')
        return rc == 0 and out[-1:] == ['identical'], out
    raise ValueError(mode)


class BacklogHarness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = build(cls.temp.name)

    def check(self, mode):
        ok, detail = mode_ok(self.binary, mode)
        self.assertTrue(ok, detail)

    def test_font_codes_are_canonical(self):
        self.check('font')

    def test_capture_rules(self):
        self.check('capture')

    def test_parity_captures_nothing(self):
        self.check('parity')

    def test_list_cap_and_view_input(self):
        self.check('list')

    def test_replay_decodes_the_controllers_pcm(self):
        self.check('decode')

    def test_negative_controls(self):
        for mutation in MUTATIONS:
            with self.subTest(mutation[0]), tempfile.TemporaryDirectory() as folder:
                binary = build(folder, mutation)
                ok, detail = mode_ok(binary, mutation[4])
                self.assertFalse(ok, f'{mutation[0]}: {mutation[4]} still passes: {detail}')


# ---------------------------------------------------------------- the game

def call_script(path):
    """date_save.txt up to the end of Shiori's phone call (the calendar)."""
    lines = []
    for line in (ROOT / 'pc/tools/playthrough/date_save.txt').read_text().splitlines():
        lines.append(line)
        if line.startswith('wait calendar '):
            break
    lines.append('end 60')
    Path(path).write_text('\n'.join(lines) + '\n')


def hashes(path):
    return Path(path).read_text().splitlines() if Path(path).exists() else []


def present(path):
    rows = []
    for line in hashes(path):
        p = line.split()
        rows.append((p[1], p[3] if len(p) > 3 else ''))
    return rows


@unittest.skipUnless(BINARY.is_file() and (ASSETS / 'raw/PSX.EXE').is_file(),
                     'needs the built port and pc/assets')
class BacklogInGame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        cls.script = cls.dir / 'call.txt'
        call_script(cls.script)
        (cls.dir / 'h').mkdir()
        cls.harness = build(cls.dir / 'h')
        cls.off = cls.play('off', {'TOKI_BACKLOG': '0'})
        cls.on = cls.play('on', {})
        cls.pages = [json.loads(l) for l in hashes(cls.on['log'])]
        voiced = [p for p in cls.pages if 'voice' in p]
        cls.tick = voiced[-1]['vblank'] + 40 if voiced else 0
        keys = (f'{cls.tick} key Ctrl+B Up Up Return Down Return Ctrl+P Escape\n'
                f'{cls.tick + 60} key Ctrl+R\n'
                f'{cls.tick + 70} key Ctrl+B End Escape\n')
        cls.used = cls.play('used', {}, keys)
        # negative control: a PC-side assist that does press for the player
        cls.pressed = cls.play('pressed', {'TOKI_BACKLOG': '0', 'TOKI_TEXT_AUTO': '1',
                                           'TOKI_TEXT_AUTO_DELAY': '1'})
        cls.parity = cls.play('parity', {}, f'{cls.tick} key Ctrl+B Return Escape\n',
                              profile=None)

    @classmethod
    def play(cls, name, settings, keys=None, profile='enhanced'):
        out = cls.dir / name
        out.mkdir()
        files = dict(frames=out / 'frames.txt', audio=out / 'audio.txt',
                     present=out / 'present.txt', dump=out / 'audio.raw',
                     log=out / 'pages.jsonl', cfg=out / 'cfg')
        files['cfg'].mkdir()
        (out / 'saves').mkdir()
        sets = dict(PC_FRAME_HASH=files['frames'], PC_FRAME_HASH_RAM='game',
                    PC_AUDIO_HASH=files['audio'], PC_PRESENT_HASH=files['present'],
                    PC_AUDIO_DUMP=files['dump'], TOKI_BACKLOG_LOG=files['log'],
                    TOKI_CONFIG_DIR=files['cfg'],
                    # the fixed layout: game code keeps stack addresses in RAM
                    TOKI_SAVE_DIR=out / 'saves', **settings)
        if keys:
            (out / 'keys.txt').write_text(keys)
            sets['TOKI_INPUT_SCRIPT'] = out / 'keys.txt'
            sets['PC_PRESENT_WINDOW_SHOT'] = out / 'backlog.png'
        cmd = ['.venv/bin/python3' if (ROOT / '.venv/bin/python3').exists() else 'python3',
               'pc/tools/playthrough.py', str(cls.script), '--out', str(out / 'run'),
               '--bin', str(BINARY)]
        if profile:
            cmd += ['--profile', profile]
        for k, v in sets.items():
            cmd += ['--set', f'{k}={v}']
        r = run(cmd, cwd=ROOT, timeout=600, max_output=16 * 1024**2)
        log = (out / 'run/date.pc.log')
        files['out'] = log.read_text(errors='replace') if log.exists() else ''
        files['result'] = r
        report = out / 'run/report.json'
        files['report'] = json.loads(report.read_text()) if report.exists() else {}
        shot = os.environ.get('TEST_BACKLOG_SHOTS')
        if shot and (out / 'backlog.png').exists():
            Path(shot).mkdir(parents=True, exist_ok=True)
            shutil.copy(out / 'backlog.png', Path(shot) / f'{name}.png')
        return files

    def test_runs_pass(self):
        for name in ('off', 'on', 'used', 'parity'):
            r = getattr(self, name)['result']
            self.assertTrue(r.ok, f'{name}: {r.stdout[-1500:]}')

    def test_pages_are_captured_with_text_speaker_and_voice(self):
        pages = self.pages
        self.assertGreaterEqual(len(pages), 5, pages)
        player = '82a082a2'                  # the surname date_save.txt types: a i
        for p in pages:
            for s in p['segs']:
                self.assertNotIn('0000', [s['sjis'][i:i + 4] for i in range(0, len(s['sjis']), 4)], p)
                bytes.fromhex(s['sjis']).decode('cp932')     # valid Shift-JIS
        named = [p for p in pages if p['speaker'] >= 0]
        self.assertGreaterEqual(len(named), 5)
        mine = [p for p in named if p['segs'][p['speaker']]['sjis'] == player]
        girls = [p for p in named if p['segs'][p['speaker']]['sjis'] != player]
        self.assertGreaterEqual(len(mine), 2)
        self.assertGreaterEqual(len(girls), 2)
        self.assertFalse(any('voice' in p for p in mine), 'the player is not voiced')
        voiced = [p for p in girls if 'voice' in p]
        self.assertGreaterEqual(len(voiced), 2)
        for p in voiced:
            v = p['voice']
            self.assertTrue(v['path'].startswith('CDROM/XA_G/') and v['path'].endswith('.XA'), v)
            self.assertNotEqual(v['path'], 'CDROM/XA_G/BGM378.XA')
            self.assertTrue(v['req'][0] <= v['lba'][0] <= v['lba'][1] <= v['req'][1], v)
            self.assertEqual(v['lba'][0], v['req'][0] + 16, v)   # SD_XACmd's window
        self.assertEqual(len({p['voice']['lba'][0] for p in voiced}), len(voiced))
        # a captured line, replayed from the disc asset, is the controller's PCM
        v = voiced[0]['voice']
        rc, out = harness(self.harness, 'decode-asset', v['path'], str(v['file_lba']),
                          str(v['req'][0]), str(v['req'][1]), str(v['file']), str(v['channel']))
        self.assertEqual((rc, out[-1:]), (0, ['identical']), out)

    def test_backlog_off_captures_nothing(self):
        self.assertEqual(hashes(self.off['log']), [])
        self.assertNotIn('[BACKLOG] page', self.off['out'])

    def test_capture_changes_nothing(self):
        self.assertGreater(len(hashes(self.off['frames'])), 1000)
        self.assertIn(' ram=', hashes(self.off['frames'])[0])
        self.assertEqual(hashes(self.on['frames']), hashes(self.off['frames']))
        self.assertEqual(hashes(self.on['audio']), hashes(self.off['audio']))
        self.assertEqual(self.on['dump'].read_bytes(), self.off['dump'].read_bytes())
        self.assertEqual(present(self.on['present']), present(self.off['present']))

    def test_using_the_backlog_changes_nothing_the_game_sees(self):
        out = self.used['out']
        self.assertIn('[BACKLOG] open', out)
        self.assertIn('[BACKLOG] replay page', out)
        self.assertIn('[BACKLOG] voice replay:', out)
        self.assertEqual(out.count('[BACKLOG] closed'), 2)
        self.assertEqual(hashes(self.used['frames']), hashes(self.off['frames']))
        self.assertEqual(hashes(self.used['audio']), hashes(self.off['audio']))
        # the presenter inserted its own windows, every other one is the game's
        frames = present(self.used['present'])
        inserted = [h for h, tag in frames if tag == 'backlog']
        self.assertGreaterEqual(len(inserted), 5)
        game = [(h, tag) for h, tag in frames if tag != 'backlog']
        ref = present(self.on['present'])
        self.assertEqual(len(game), len(ref))
        # the same windows, except where the presenter drew a note (a toast)
        differ = [i for i, ((h, tag), (r, _)) in enumerate(zip(game, ref)) if h != r]
        self.assertTrue(all(game[i][1] == 'toast' for i in differ), differ[:20])
        self.assertLess(len(differ), 400)
        self.assertEqual(self.used['report'].get('pc'), self.on['report'].get('pc'))

    def test_replay_is_mixed_on_top_of_the_game(self):
        """The output differs from the run without the replay by the replayed
        line itself, scaled by one gain (to within the mixer's rounding)."""
        a = self.on['dump'].read_bytes()
        b = self.used['dump'].read_bytes()
        self.assertEqual(len(a), len(b))
        first = next(i for i in range(0, len(a), 4) if a[i:i + 4] != b[i:i + 4])
        line = [l for l in self.used['out'].splitlines() if '[BACKLOG] replay page' in l][-1]
        serial = int(line.split()[-1])
        page = next(p for p in self.pages if p['serial'] == serial)
        v = page['voice']
        pcm = self.dir / 'replay.raw'
        rc, out = harness(self.harness, 'decode-asset', v['path'], str(v['file_lba']),
                          str(v['req'][0]), str(v['req'][1]), str(v['file']), str(v['channel']),
                          str(pcm))
        self.assertEqual(rc, 0, out)
        p = struct.unpack(f'<{len(pcm.read_bytes()) // 2}h', pcm.read_bytes())

        def fit(start, n):
            n = min(n, len(p), (len(a) - start) // 2)
            x = struct.unpack(f'<{n}h', a[start:start + 2 * n])
            y = struct.unpack(f'<{n}h', b[start:start + 2 * n])
            d = [yy - xx for xx, yy in zip(x, y)]
            den = sum(pp * pp for pp in p[:n]) or 1
            gain = sum(dd * pp for dd, pp in zip(d, p)) / den
            return sum(1 for dd, pp in zip(d, p) if abs(dd - pp * gain) > 1.5), gain, n
        # the replay starts at a mixer call; its first samples may round to 0
        start = min(range(max(0, first - 4 * 1500), first + 4, 4), key=lambda c: fit(c, 6000)[0])
        off, gain, n = fit(start, len(p))
        self.assertEqual(n, len(p))
        self.assertGreater(gain, 0.001)
        self.assertLess(off, n // 1000 + 1, f'gain {gain}: {off} of {n} samples off')
        # and nothing before the replay or after it ended
        self.assertEqual(a[:start], b[:start])
        self.assertEqual(a[start + 2 * len(p):], b[start + 2 * len(p):])

    def test_parity_has_no_backlog(self):
        self.assertNotIn('[BACKLOG]', self.parity['out'])
        self.assertEqual(hashes(self.parity['log']), [])
        self.assertNotIn('backlog', {tag for _, tag in present(self.parity['present'])})

    def test_negative_control_a_press_is_seen(self):
        self.assertNotEqual(hashes(self.pressed['frames']), hashes(self.off['frames']))


if __name__ == '__main__':
    unittest.main()
