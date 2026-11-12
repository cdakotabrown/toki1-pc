# SPDX-License-Identifier: GPL-3.0-or-later
"""Songs never overlap at a scene change or inside a scene, in both profiles
(docs/pc-port/ENHANCED-MODE.md, "Music at scene changes"; the 2026-11-02
report of "two songs at once" in the sports festival's spoon relay).

The port's song state is read from two logs every run can write:
  - TOKI_MUSIC_LOG (pc/hal/music_pack.c): open / start / stop / end /
    close per sequence access number, as JSON lines;
  - PC_DEBUG=sound (pc/hal/spu.c audio_stats): every change of the sound
    driver's song state (gSDInfo 0x100 playing, gSDSerialInfo bit 0 fading,
    seq_vol_l) and every sound-bank upload into SPU RAM.

Checks:
  - at most one sequence is playing at any VBlank;
  - no sound bank is uploaded while a song is still audible in a fade-out
    (the upload's SD_VABClose would cut it, and the next song starts on its
    tails: the enhanced profile's instant loads did this before
    CD_WAIT_MUSIC_FADE);
  - the sports festival (TAIIKU, scene 114) to the spoon relay plays the same
    songs, in the same order, with the same hand-overs in enhanced and parity
    (the relay's sequencer record matches the console's frame for frame:
    RENDER-PARITY-HANDOFF.md, 2026-11-02 "TAIIKU music");
  - the player log carries one [MUSIC] line per song event and none for the
    per-frame fade steps; TOKI_MUSIC_EVENTS=0 removes them, and observing
    changes nothing (parity's audio and trace are identical either way).

Negative controls: CD_WAIT_MUSIC_FADE=0 (enhanced) uploads the name entry's
bank over the title song mid-fade; synthetic logs with two songs playing at
once and with a mid-fade upload are flagged; TOKI_MUSIC_EVENTS=0 writes no
[MUSIC] line.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PC_BIN = ROOT / 'pc/build/tokimeki-pc'
HAVE_GAME = PC_BIN.exists() and (ROOT / 'pc/assets/raw').exists()
OUT = ROOT / 'build/test-music-overlap'

# TAIIKU entered from the title (SCENE_NAV=114, goto_step as the game does),
# the event menu, page 1's first event (the spoon relay: taiiku_main step 12,
# the SPSETU panel), up to the race's song.  No card: the menu takes the pad
# (up moves the hand a line; three ups from the opening position reach line
# 0, the hand's y 0x38).
TAIIKU = """hold 2
trace taiiku_state:4 cursor_cw_y:2
phase taiiku
wait entered now_step=0x72 within 3000
wait taiiku_menu now_step=0x72 taiiku_state=8 within 3000 mash cross/60
+60 up
+20 up
+20 up
wait taiiku_picked now_step=0x72 taiiku_state!=8 within 600 mash circle/30
check spoon_relay taiiku_state=12
wait relay now_step=0x72 now_sub_step=2 within 3000 mash circle/10
end 600
"""

# new_game.txt's opening: the title song, START, the album (blank card), to
# the name entry's own song (a scene change that loads a new sound bank
# while the title song fades out).
NAME_ENTRY = """hold 2
trace cursor_cw_x:2 cursor_cw_y:2 now_icon:1
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

SONG = re.compile(r'\[AUDIO\] vbl (\d+) song: info 0x([0-9A-F]+) serial (\d+) cur (-?\d+) no (-?\d+) '
                  r'req (-?\d+) vol (-?\d+), (\d+) voices sounding')
UPLOAD = re.compile(r'\[AUDIO\] vbl (\d+) upload 0x([0-9A-F]+)\+(\d+): (\d+) voices sounding')
MUSIC = re.compile(r'^\[MUSIC\] vbl (\d+) (\S+) acc (\d+) (\S+) step (-?\d+)/(-?\d+) (\S+)', re.M)


# ------------------------------------------------------------------ analysis
def songs_playing_at_once(events):
    """[(vbl, ids)] wherever more than one access number is playing"""
    playing, out = {}, []
    for e in events:
        if e['ev'] in ('start', 'resume'):
            playing[e['acc']] = e['id']
            if len(playing) > 1:
                out.append((e['vbl'], sorted(playing.values())))
        elif e['ev'] in ('stop', 'end', 'close', 'pause'):
            playing.pop(e['acc'], None)
    return out


def uploads_over_a_fade(log_text):
    """[(vbl, vol)]: a sound-bank upload while the driver's song is playing,
    fading (serial bit 0) and still audible (seq_vol_l >= 0)"""
    state = dict(info=0, serial=0, vol=-1)
    out = []
    for line in log_text.splitlines():
        m = SONG.search(line)
        if m:
            state = dict(info=int(m.group(2), 16), serial=int(m.group(3)), vol=int(m.group(7)))
            continue
        m = UPLOAD.search(line)
        if m and state['info'] & 0x100 and state['serial'] & 1 and state['vol'] >= 0:
            out.append((int(m.group(1)), state['vol']))
    return out


def song_sequence(events):
    """the songs as the game played them: (event, id, step, sub)"""
    return [(e['ev'], e['id'], e['step'], e['sub']) for e in events
            if e['ev'] in ('open', 'start', 'stop', 'end', 'close')]


# ------------------------------------------------------------------ runs
def run(name, script, profile, extra=(), music_log=True):
    d = OUT / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    (d / 'script.txt').write_text(script)
    cmd = [sys.executable, str(ROOT / 'pc/tools/playthrough.py'), str(d / 'script.txt'),
           '--out', str(d / 'run'), '--profile', profile, '--max-frames', '12000',
           '--set', 'PC_DEBUG=sound', '--set', f'PC_AUDIO_HASH={d}/audio.hash']
    if music_log:
        cmd += ['--set', f'TOKI_MUSIC_LOG={d}/music.jsonl']
    for kv in extra:
        cmd += ['--set', kv]
    with open(d / 'playthrough.log', 'w') as fh:
        r = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=ROOT)
    logs = sorted((d / 'run').glob('*.pc.log'))
    text = ''.join(p.read_text(errors='replace') for p in logs)
    mj = d / 'music.jsonl'
    events = [json.loads(l) for l in mj.read_text().splitlines()] if mj.exists() else []
    trace = sorted((d / 'run').glob('*.pc.trace'))
    return dict(rc=r.returncode, log=text, events=events, dir=d,
                audio=(d / 'audio.hash').read_bytes() if (d / 'audio.hash').exists() else b'',
                trace=trace[0].read_bytes() if trace else b'')


class AnalysisTests(unittest.TestCase):
    """the checks themselves, on synthetic logs (negative controls)"""

    def test_two_songs_flagged(self):
        ev = [dict(ev='start', acc=0, id='a', vbl=1), dict(ev='start', acc=1, id='b', vbl=5),
              dict(ev='close', acc=0, id='a', vbl=9)]
        self.assertEqual(songs_playing_at_once(ev), [(5, ['a', 'b'])])
        ev = [dict(ev='start', acc=0, id='a', vbl=1), dict(ev='close', acc=0, id='a', vbl=4),
              dict(ev='start', acc=0, id='b', vbl=4)]
        self.assertEqual(songs_playing_at_once(ev), [])

    def test_upload_over_a_fade_flagged(self):
        log = ('[AUDIO] vbl 10 song: info 0x000100 serial 1 cur 0 no 0 req -1 vol 23, 6 voices sounding\n'
               '[AUDIO] vbl 12 upload 0x11010+145728: 6 voices sounding, 0 from inside it\n')
        self.assertEqual(uploads_over_a_fade(log), [(12, 23)])
        done = ('[AUDIO] vbl 10 song: info 0x000000 serial 0 cur 0 no -1 req -1 vol -1, 1 voices sounding\n'
                '[AUDIO] vbl 12 upload 0x11010+145728: 1 voices sounding, 0 from inside it\n')
        self.assertEqual(uploads_over_a_fade(done), [])

    def test_player_log_line_parses(self):
        line = ('[MUSIC] vbl 2978 start acc 0 seq-24c9ffcc step 114/0 TAIIKU '
                '"addr":"0x8003676C","bytes":6874,"vab":2,"vol":[85,85],"pack":"","file":false\n')
        m = MUSIC.search(line)
        self.assertEqual((m.group(2), m.group(4), m.group(5), m.group(7)),
                         ('start', 'seq-24c9ffcc', '114', 'TAIIKU'))


@unittest.skipUnless(HAVE_GAME, 'needs the built port and your pc/assets')
class GameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        jobs = {
            'taiiku_enh': (TAIIKU, 'enhanced', ['SCENE_NAV=114']),
            'taiiku_par': (TAIIKU, 'parity', ['SCENE_NAV=114']),
            # nothing observes the music at all: no event log, no [MUSIC] lines
            'taiiku_par_quiet': (TAIIKU, 'parity', ['SCENE_NAV=114', 'TOKI_MUSIC_EVENTS=0'], False),
            'name_enh': (NAME_ENTRY, 'enhanced', []),
            'name_par': (NAME_ENTRY, 'parity', []),
            'name_enh_nofade': (NAME_ENTRY, 'enhanced', ['CD_WAIT_MUSIC_FADE=0']),
        }
        with ThreadPoolExecutor(len(jobs)) as ex:
            futs = {k: ex.submit(run, k, *v) for k, v in jobs.items()}
            cls.r = {k: f.result() for k, f in futs.items()}

    def ok(self, name, events=True):
        r = self.r[name]
        self.assertEqual(r['rc'], 0, f'{name}: playthrough failed ({r["dir"]})')
        if events:
            self.assertTrue(r['events'], f'{name}: no music events')
        return r

    def test_one_song_at_a_time(self):
        for name in ('taiiku_enh', 'taiiku_par', 'name_enh', 'name_par'):
            with self.subTest(run=name):
                self.assertEqual(songs_playing_at_once(self.ok(name)['events']), [])

    def test_no_bank_upload_over_a_fading_song(self):
        for name in ('taiiku_enh', 'taiiku_par', 'name_enh', 'name_par'):
            with self.subTest(run=name):
                self.assertEqual(uploads_over_a_fade(self.ok(name)['log']), [])

    def test_negative_control_no_fade_wait_cuts_the_title_song(self):
        cuts = uploads_over_a_fade(self.ok('name_enh_nofade')['log'])
        self.assertTrue(cuts, 'CD_WAIT_MUSIC_FADE=0 should upload the bank mid-fade')

    def test_taiiku_songs_match_parity(self):
        enh, par = self.ok('taiiku_enh')['events'], self.ok('taiiku_par')['events']
        seq = [s for s in song_sequence(enh) if s[2] == 114]
        self.assertEqual(seq, [s for s in song_sequence(par) if s[2] == 114])
        starts = [s[1] for s in seq if s[0] == 'start']
        # UNDOKAI#2 (the festival's menu) then UNDOKAI#1 (the spoon relay)
        self.assertEqual(starts, ['seq-24c9ffcc', 'seq-6259157c'])

    def test_player_log_has_song_events_only(self):
        lines = MUSIC.findall(self.ok('taiiku_par')['log'])
        evs = {l[1] for l in lines}
        self.assertIn('start', evs)
        self.assertNotIn('vol', evs)
        self.assertIn(('start', 'seq-6259157c', '114', '2'),
                      {(l[1], l[3], l[4], l[5]) for l in lines})

    def test_negative_control_events_off_and_observing_changes_nothing(self):
        quiet, loud = self.ok('taiiku_par_quiet', events=False), self.ok('taiiku_par')
        self.assertEqual(MUSIC.findall(quiet['log']), [])
        self.assertTrue(quiet['audio'])
        self.assertEqual(quiet['audio'], loud['audio'])
        self.assertEqual(quiet['trace'], loud['trace'])


if __name__ == '__main__':
    unittest.main()
