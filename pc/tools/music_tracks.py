#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""music_tracks.py -- the game's songs, their track ids, and music packs.

The PC port's music packs (pc/hal/music_pack.c; docs/pc-port/PLAYER-GUIDE.md,
"Music packs") replace a song by its TRACK ID: "seq-" + FNV-1a 32 of the
song's SEQ bytes from its 'pQES' header to its end-of-track event.  The same
id comes out of the SEQ files on the disc (this tool) and out of the game's
RAM when it plays the song (the port), whichever table, address or overlay
plays it.

Commands:
  scan     the songs on the disc: every SEQ in the sound tables
           (CDROM/VAB_SEQ/*.SEQ: 16 offsets, reverb depths, SEQ data at
           offset + 16), with length, tempo, loop markers.  --json for
           machines.
  collect  run the port headless (scenes via SCENE_NAV, or playthrough
           scripts) with the event log on, and optionally the dump, to learn
           where each song is heard.
  table    scan + any heard logs (TOKI_MUSIC_LOG / <music dir>/dump/
           heard.jsonl) + any dumped WAVs -> a track list (markdown, TSV or
           JSON).  `--list-music` is `table` to stdout.
  synth-pack  build a test pack from dumped WAVs (`tone`: the dump with a
           1 kHz tone added; `pitch`: the dump an octave up; `silence`;
           `copy`: the dump itself) -- for checking the hook, never for play.

Nothing this tool writes is game data to commit: tables, logs, dumps and
packs go under the music directory or --out (docs/pc-port/PLAYER-GUIDE.md).
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import disc_input  # noqa: E402
DEFAULT_DISC = os.path.join(ROOT, 'pc/assets/raw/CDROM')


# ---- the SEQ format (libsnd, as pc/hal/music_pack.c parses it) -----------

def seq_span(data, start=0):
    """Bytes of the SEQ at data[start:] ('pQES' .. FF 2F), or 0."""
    p = data
    if p[start:start + 4] != b'pQES' or len(p) - start < 16:
        return 0
    pos, status, end = start + 15, 0, len(p)
    while pos < end:
        for k in range(4):
            if pos >= end:
                return 0
            b = p[pos]
            pos += 1
            if not b & 0x80:
                break
        else:
            return 0
        if pos >= end:
            return 0
        b = p[pos]
        if b & 0x80:
            status = b
            pos += 1
        elif not status:
            return 0
        hi = status & 0xF0
        if status == 0xFF:
            if pos >= end:
                return 0
            t = p[pos]
            pos += 1
            if t == 0x2F:
                return pos - start
            if t != 0x51:
                return 0
            pos += 3
        elif hi in (0xC0, 0xD0):
            pos += 1
        elif 0x80 <= hi <= 0xE0:
            pos += 2
        else:
            return 0
    return 0


def track_id(data, start=0):
    n = seq_span(data, start)
    if not n:
        return None
    h = 2166136261
    for b in data[start:start + n]:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return 'seq-%08x' % h


def seq_timing(data, start=0):
    """Length and loop markers in seconds (libsnd's reading of the SEQ):
    CC 99 = 20 marks the loop start, CC 6 right after it the count (127 =
    for ever), CC 99 = 30 the loop end."""
    p = data
    res = (p[start + 8] << 8) | p[start + 9]
    tempo = (p[start + 10] << 16) | (p[start + 11] << 8) | p[start + 12]
    pos, status, t, end = start + 15, 0, 0.0, start + seq_span(data, start)
    loop_start = loop_end = loop_count = None
    nrpn = None
    first_tempo = tempo
    while pos < end:
        delta = 0
        for _ in range(4):
            b = p[pos]
            pos += 1
            delta = (delta << 7) | (b & 0x7F)
            if not b & 0x80:
                break
        if res and tempo:
            t += delta * tempo / res / 1e6
        b = p[pos]
        if b & 0x80:
            status = b
            pos += 1
        hi = status & 0xF0
        if status == 0xFF:
            typ = p[pos]
            pos += 1
            if typ == 0x2F:
                break
            tempo = (p[pos] << 16) | (p[pos + 1] << 8) | p[pos + 2]
            pos += 3
        elif hi in (0xC0, 0xD0):
            pos += 1
        else:
            if hi == 0xB0:
                ctl, val = p[pos], p[pos + 1]
                if ctl == 99:
                    nrpn = val
                    if val == 20 and loop_start is None:
                        loop_start = t
                    elif val == 30 and loop_end is None:
                        loop_end = t
                elif ctl == 6 and nrpn == 20 and loop_count is None:
                    loop_count = val
            pos += 2
    return dict(resolution=res, bpm=round(60e6 / first_tempo, 2) if first_tempo else None,
                seconds=round(t, 2),
                loop_start=None if loop_start is None else round(loop_start, 2),
                loop_end=None if loop_end is None else round(loop_end, 2),
                loop_count=loop_count)


def scan(disc):
    """Every song in the disc's sound tables."""
    tracks = {}
    folder = os.path.join(disc, 'VAB_SEQ')
    for name in sorted(os.listdir(folder)):
        if not name.upper().endswith('.SEQ'):
            continue
        with open(os.path.join(folder, name), 'rb') as fh:
            data = fh.read()
        if len(data) < 80:
            continue
        offs = struct.unpack('<16I', data[:64])
        reverb = data[64:80]
        seen = set()
        for j, off in enumerate(offs):
            if off == 0 and j:
                continue          # SD_SEQTableSet: 0 repeats the first song
            start = off + 16
            if start in seen or start + 16 > len(data):
                continue
            seen.add(start)
            tid = track_id(data, start)
            if not tid:
                continue
            info = tracks.setdefault(tid, dict(id=tid, sources=[], bytes=seq_span(data, start)))
            info['sources'].append(dict(table=name[:-4], index=j, reverb=reverb[j]))
            info.update(seq_timing(data, start))
    return tracks


# ---- heard logs and dumps -------------------------------------------------

def read_heard(paths):
    heard = {}
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, errors='replace') as fh:
            lines = fh.read().splitlines()
        for line in lines:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get('ev') != 'start' or not e.get('id', '').startswith('seq-'):
                continue
            h = heard.setdefault(e['id'], dict(places=set(), first=None, plays=0))
            h['plays'] += 1
            place = 'step %s%s' % (e.get('step'), '' if e.get('overlay') in (None, '-')
                                   else ' (%s)' % e['overlay'])
            h['places'].add(place)
            if h['first'] is None:
                h['first'] = os.path.basename(os.path.dirname(path)) + ' vbl %s' % e.get('vbl')
    return heard


def dump_info(dump_dir, tid):
    path = os.path.join(dump_dir, tid + '.wav') if dump_dir else None
    if not path or not os.path.exists(path):
        return None
    with wave.open(path) as w:
        return round(w.getnframes() / w.getframerate(), 1)


def build_table(disc, heard_paths, dump_dir):
    tracks = scan(disc) if disc and os.path.isdir(os.path.join(disc, 'VAB_SEQ')) else {}
    heard = read_heard(heard_paths)
    for tid in heard:
        tracks.setdefault(tid, dict(id=tid, sources=[], bytes=None))
    rows = []
    for tid in sorted(tracks, key=lambda k: (tracks[k]['sources'][0]['table']
                                            if tracks[k]['sources'] else '~', k)):
        t = tracks[tid]
        h = heard.get(tid)
        rows.append(dict(
            id=tid,
            names=['%s#%d' % (s['table'], s['index']) for s in t['sources']],
            bytes=t.get('bytes'), seconds=t.get('seconds'), bpm=t.get('bpm'),
            loop_start=t.get('loop_start'), loop_end=t.get('loop_end'),
            loop_count=t.get('loop_count'),
            heard=sorted(h['places']) if h else [], plays=h['plays'] if h else 0,
            dump_seconds=dump_info(dump_dir, tid)))
    return rows


def fmt_loop(r):
    if r['loop_start'] is None:
        return 'none'
    cnt = 'for ever' if r['loop_count'] in (None, 127) else 'x%d' % r['loop_count']
    return '%s-%s s %s' % (r['loop_start'], r['loop_end'], cnt)


def write_table(rows, out, fmt):
    if fmt == 'json':
        text = json.dumps(rows, indent=1) + '\n'
    elif fmt == 'tsv':
        lines = ['id\tdisc\tseconds\tloop\theard\tdump_s']
        for r in rows:
            lines.append('\t'.join([r['id'], ','.join(r['names']), str(r['seconds']), fmt_loop(r),
                                    '; '.join(r['heard']), str(r['dump_seconds'] or '')]))
        text = '\n'.join(lines) + '\n'
    else:
        lines = ['# Tokimeki Memorial: the songs and their track ids', '',
                 'Track id = file name in a music pack (`<id>.ogg` / `.flac` / `.wav`).',
                 '"disc" is the sound table on the disc and the song\'s index in it;',
                 '"length" runs to the end of the sequence (to the loop end for a song',
                 'that loops for ever); "heard" is where a logged run started it.', '',
                 '| track id | disc | length (s) | loop (sequence time) | heard | dump (s) |',
                 '| --- | --- | ---: | --- | --- | ---: |']
        for r in rows:
            lines.append('| %s | %s | %s | %s | %s | %s |' % (
                r['id'], ', '.join(r['names']) or '?', r['seconds'] if r['seconds'] is not None else '?',
                fmt_loop(r) if r['bytes'] else '?', '; '.join(r['heard']) or '-',
                r['dump_seconds'] or '-'))
        text = '\n'.join(lines) + '\n'
    if out in (None, '-'):
        sys.stdout.write(text)
    else:
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, 'w') as fh:
            fh.write(text)
        print('wrote %s (%d tracks)' % (out, len(rows)))


# ---- collect: run the port -------------------------------------------------

def run_port(binary, work, frames, extra_env, scene=None, script=None, timeout=1800):
    os.makedirs(os.path.join(work, 'pc'), exist_ok=True)
    for name in ('pc/assets', 'disks'):
        if not os.path.lexists(os.path.join(work, name)):
            os.symlink(os.path.join(ROOT, name) if name != 'disks' else str(disc_input(name)),
                       os.path.join(work, name))
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               PC_TRACE=os.path.join(work, 'run.trace'), PC_TRACE_FRAMES=str(frames),
               PC_PARITY_DIR=os.path.join(work, 'parity'),
               # a SCENE_NAV run anchors the game-thread mix at the scene and mixes
               # (and dumps) for the 1,800 VBlanks of the capture window
               PC_PARITY_FRAMES='1800' if scene is not None else '1',
               TOKI_MUSIC_LOG=os.path.join(work, 'heard.jsonl'))
    if scene is not None:
        env['SCENE_NAV'] = str(scene)
    env.update(extra_env)
    with open(os.path.join(work, 'run.log'), 'w') as fh:
        r = subprocess.run([binary, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env,
                           stdout=fh, stderr=subprocess.STDOUT, timeout=timeout)
    return r.returncode


def cmd_collect(a):
    binary = os.path.abspath(a.binary)
    out = os.path.abspath(a.out)
    extra = dict(kv.split('=', 1) for kv in a.set)
    if a.profile:
        extra['TOKI_PROFILE'] = a.profile
    if a.dump:
        extra['TOKI_MUSIC_DUMP'] = '1'
        extra.setdefault('TOKI_MUSIC_DIR', os.path.join(out, 'music'))
    logs = []
    for scene in [s for s in a.scenes.split(',') if s]:
        work = os.path.join(out, 'scene%s' % scene)
        rc = run_port(binary, work, a.frames, extra, scene=scene)
        logs.append(os.path.join(work, 'heard.jsonl'))
        starts = 0
        if os.path.exists(logs[-1]):
            with open(logs[-1]) as fh:
                starts = sum(1 for line in fh if '"ev":"start"' in line)
        print('scene %s: exit %d, %d song starts' % (scene, rc, starts))
    if a.playthrough:
        for script in a.playthrough:
            name = os.path.splitext(os.path.basename(script))[0]
            cmd = [sys.executable, os.path.join(ROOT, 'pc/tools/playthrough.py'), script,
                   '--bin', binary, '--out', os.path.join(out, 'pt-' + name)]
            if a.profile:
                cmd += ['--profile', a.profile]
            log = os.path.join(out, 'pt-' + name, 'heard.jsonl')
            for k, v in dict(extra, TOKI_MUSIC_LOG=log).items():
                cmd += ['--set', '%s=%s' % (k, v)]
            os.makedirs(os.path.dirname(log), exist_ok=True)
            rc = subprocess.run(cmd).returncode
            logs.append(log)
            print('playthrough %s: exit %d' % (name, rc))
    print('heard logs:', ' '.join(logs))


def cmd_table(a):
    heard = list(a.heard)
    dump_dir = a.dump_dir
    if a.music_dir:
        heard.append(os.path.join(a.music_dir, 'dump', 'heard.jsonl'))
        dump_dir = dump_dir or os.path.join(a.music_dir, 'dump')
    rows = build_table(a.disc, heard, dump_dir)
    fmt = a.format or (os.path.splitext(a.out)[1].lstrip('.') if a.out and a.out != '-' else 'md')
    write_table(rows, a.out, {'md': 'md', 'json': 'json', 'tsv': 'tsv'}.get(fmt, 'md'))


def cmd_scan(a):
    tracks = scan(a.disc)
    if a.json:
        print(json.dumps(tracks, indent=1))
        return
    for tid, t in sorted(tracks.items(), key=lambda kv: kv[1]['sources'][0]['table']):
        print('%s  %-24s %6.1f s  bytes %5d  loop %s' % (
            tid, ','.join('%s#%d' % (s['table'], s['index']) for s in t['sources']),
            t['seconds'], t['bytes'], fmt_loop(t)))
    print('%d songs' % len(tracks))


# ---- synth-pack: a test pack from dumps ------------------------------------

def read_wav(path):
    with wave.open(path) as w:
        assert w.getsampwidth() == 2
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        frames = w.readframes(n)
    samples = struct.unpack('<%dh' % (len(frames) // 2), frames)
    return ch, rate, samples


def write_wav(path, ch, rate, samples):
    with wave.open(path, 'wb') as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))


def synth(samples, ch, rate, mode):
    clamp = lambda v: -32768 if v < -32768 else 32767 if v > 32767 else int(v)
    if mode == 'tone':
        out = []
        for i, s in enumerate(samples):
            t = (i // ch) / rate
            out.append(clamp(s * 0.8 + 6000 * math.sin(2 * math.pi * 1000 * t)))
        return out, rate
    if mode == 'pitch':        # an octave up: every second frame
        frames = [samples[i:i + ch] for i in range(0, len(samples), ch)][::2]
        return [v for f in frames for v in f], rate
    if mode == 'silence':
        return [0] * len(samples), rate
    if mode == 'copy':         # the dump itself: must sound like the original
        return list(samples), rate
    raise ValueError(mode)


def cmd_synth(a):
    os.makedirs(a.out, exist_ok=True)
    n = 0
    for name in sorted(os.listdir(a.dump)):
        if not name.endswith('.wav') or not name.startswith('seq-'):
            continue
        if a.only and name[:-4] not in a.only:
            continue
        ch, rate, samples = read_wav(os.path.join(a.dump, name))
        if a.seconds:
            samples = samples[:int(a.seconds * rate) * ch]
        out, rate = synth(samples, ch, rate, a.mode)
        write_wav(os.path.join(a.out, name), ch, rate, out)
        n += 1
    print('wrote %d files to %s (%s)' % (n, a.out, a.mode))


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv[:1] == ['--list-music']:
        argv = ['table', '--out', '-'] + argv[1:]
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan', help='the songs on the disc')
    s.add_argument('--disc', default=DEFAULT_DISC, help='the extracted CDROM folder')
    s.add_argument('--json', action='store_true')
    s.set_defaults(fn=cmd_scan)
    s = sub.add_parser('collect', help='run the port and log where songs are heard')
    s.add_argument('--binary', default=os.path.join(ROOT, 'pc/build/tokimeki-pc'))
    s.add_argument('--out', required=True)
    s.add_argument('--scenes', default='', help='SCENE_NAV ids, comma-separated')
    s.add_argument('--frames', type=int, default=3000)
    s.add_argument('--playthrough', action='append', help='a pc/tools/playthrough script')
    s.add_argument('--profile', default=None)
    s.add_argument('--dump', action='store_true', help='also dump the songs (TOKI_MUSIC_DUMP)')
    s.add_argument('--set', action='append', default=[], help='NAME=VALUE for the port')
    s.set_defaults(fn=cmd_collect)
    s = sub.add_parser('table', help='the track list')
    s.add_argument('--disc', default=DEFAULT_DISC)
    s.add_argument('--heard', action='append', default=[], help='a heard.jsonl event log')
    s.add_argument('--music-dir', help='a music directory (its dump/heard.jsonl and dumps)')
    s.add_argument('--dump-dir')
    s.add_argument('--out', default='-', help='file (.md / .tsv / .json) or - for stdout')
    s.add_argument('--format', choices=('md', 'tsv', 'json'))
    s.set_defaults(fn=cmd_table)
    s = sub.add_parser('synth-pack', help='a TEST pack from dumped WAVs')
    s.add_argument('--dump', required=True)
    s.add_argument('--out', required=True, help='the pack folder')
    s.add_argument('--mode', choices=('tone', 'pitch', 'silence', 'copy'), default='tone')
    s.add_argument('--seconds', type=float, default=0)
    s.add_argument('--only', action='append', default=[], help='track ids')
    s.set_defaults(fn=cmd_synth)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == '__main__':
    main()
