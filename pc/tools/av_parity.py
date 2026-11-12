#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded all-scene native/retail framebuffer and PCM comparison.

This is a comprehensive no-input *scenario sweep*, not a full playthrough.
Both machines boot first, then execute retail goto_step at the first title
entry. Checkpoints count hardware VBlanks after navigation. Differences,
unsupported scanout, missing references, silence and divergent state remain
failures/unverified evidence; they are never promoted to a parity pass.

Build the derived reference with the local reference-emulator tooling
(build_parity_reference.py in local/refemu/, pc/tools/refemu.py; not
distributed).
Generated frames, audio, BIOS links and third-party code must stay ignored.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sys
import threading
import time

from parity_metrics import compare, digest, records
from safe_process import run
import refemu

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP, disc_input  # noqa: E402
STOP = threading.Event()


def scene_list(spec):
    scenes = set()
    for part in spec.split(','):
        if not re.fullmatch(r'\d+(?:-\d+)?', part):
            raise ValueError('scenes must be comma-separated IDs/ranges in 0..210')
        start, _, end = part.partition('-')
        low, high = int(start), int(end or start)
        if not 0 <= low <= high <= 210:
            raise ValueError('scene range outside 0..210')
        scenes.update(range(low, high + 1))
    return sorted(scenes)


def checkpoint_list(spec):
    if not re.fullmatch(r'[0-9]+(?:,[0-9]+)*', spec):
        raise ValueError('checkpoints must be strictly increasing positive integers')
    points = [int(value) for value in spec.split(',')]
    if not 1 <= len(points) <= 16 or not all(0 < point <= 1800 for point in points):
        raise ValueError('1..16 checkpoints in 1..1800 required')
    if points != sorted(set(points)) or points[-1] < 2:
        raise ValueError('checkpoints must strictly increase and finish after VBlank 1')
    return points


def clean_environment():
    prefixes = ('PC_', 'TOKI_', 'AUTOPLAY', 'SCENE', 'SCREENSHOT_', 'VERBOSE_FRAMES',
                'STR_DUMP_', 'GP0_TRACE', 'GPU_TRACE', 'MDFN_GPU', 'REFEMU_')
    # the reference's own NBN trace switch, under any prefix
    return {key: value for key, value in os.environ.items()
            if not key.startswith(prefixes) and not key.endswith('_NBN_TRACE')}


def symbol_address(name, path=DECOMP / 'config/symbol_names.json'):
    """Retail address of a landed global name (config/symbol_names.json)."""
    symbols = json.loads(Path(path).read_text())['symbols']
    found = [int(addr, 16) for addr, entry in symbols.items() if entry.get('name') == name]
    if len(found) != 1:
        raise ValueError(f'fixture symbol {name!r}: {len(found)} addresses in symbol_names.json')
    return found[0]


def fixture_pokes(specs, anchor, lookup=symbol_address):
    """--fixture-poke N:SYMBOL:HEX -> (native PC_POKE, reference -poke args).

    A TEST FIXTURE, not natural parity evidence: it writes RAM in both
    machines N VBlanks after navigation, after checkpoint N's capture.  The
    native side is pc/hal/trace.c `vN:SYMBOL=HEX`; the reference is
    regtest `-poke <anchor+N>:ADDR:HEX`, applied by its frame hook after
    ParityReference::OnFrame.  The reference anchor must be given (it is
    printed as `[PARITY] anchor=` and checked after the run)."""
    native, reference = [], []
    for spec in specs:
        match = re.fullmatch(r'([1-9][0-9]{0,3}):([A-Za-z_][A-Za-z0-9_]*):((?:[0-9a-fA-F]{2}){1,64})', spec)
        if not match:
            raise ValueError(f'--fixture-poke {spec!r}: want N:SYMBOL:HEXBYTES, N >= 1')
        if anchor is None:
            raise ValueError('--fixture-poke needs --reference-anchor (the reference [PARITY] anchor=)')
        frame, name, data = int(match.group(1)), match.group(2), match.group(3).lower()
        native.append(f'v{frame}:{name}={data}')
        reference += ['-poke', f'{anchor + frame}:{lookup(name):08X}:{data}']
    return ','.join(native), reference


def artifact_budget(scenes, renderers, checkpoints, repeat_scenes):
    runs = len(scenes) + len(set(scenes) & set(repeat_scenes))
    image_bytes = len(checkpoints) * 1024 * 512 * 2
    common = image_bytes + 8 * 1024 ** 2 + 8 * 1024 ** 2 + 32768
    native = common + len(checkpoints) * (320 * 240 * 3 + 64) + 18 * 1024 ** 2
    return runs * (common + len(renderers) * native) + 1024 ** 3


def private_inputs(directory):
    (directory / 'pc').mkdir()
    (directory / 'pc/assets').symlink_to(ROOT / 'pc/assets', target_is_directory=True)
    (directory / 'disks').symlink_to(disc_input('disks'), target_is_directory=True)


def capture(backend, scene, directory, settings):
    directory.mkdir(parents=True)
    env = clean_environment()
    points = ','.join(map(str, settings['checkpoints']))
    if backend == 'reference':
        env.update(TOKI_PARITY_DIR=str(directory), TOKI_PARITY_FRAMES=points,
                   TOKI_PARITY_SCENE=str(scene), LD_LIBRARY_PATH=settings['reference_lib'])
        cmd = [settings['reference'], '-frames', str(settings['checkpoints'][-1] + 6000),
               '-console', '-cpu', 'Interpreter', *settings.get('reference_pokes', []),
               '--', str(ROOT / settings['disc'])]
        state_name, audio_name = 'frames.jsonl', 'audio.wav'
    else:
        private_inputs(directory)
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
                   PC_PARITY_DIR=str(directory), PC_PARITY_FRAMES=points,
                   PC_TRACE=str(directory / 'trace.txt'), PC_TRACE_CLOCK='vblank',
                   PC_TRACE_FRAMES=str(settings['checkpoints'][-1]), SCENE_NAV=str(scene),
                   MDFN_GPU='1' if backend == 'mednafen' else '0')
        if settings.get('native_pokes'):
            env['PC_POKE'] = settings['native_pokes']
        cmd = [str(ROOT / 'pc/build/tokimeki-pc'), str(ROOT / 'pc/assets')]
        state_name, audio_name = 'state.jsonl', 'audio.s16le'
    start = time.monotonic()
    result = run(cmd, cwd=directory, env=env, timeout=settings['timeout'], stop=STOP,
                 log_path=directory / 'run.log',
                 watch_files=[(directory / state_name, 32768, 'STATE_LIMIT'),
                              (directory / audio_name, 8 * 1024 * 1024, 'AUDIO_LIMIT'),
                              (directory / 'trace.txt', 16 * 1024 * 1024, 'TRACE_LIMIT')])
    summary = {'status': 'OK' if result.ok else result.reason or 'EXIT',
               'returncode': result.returncode, 'seconds': round(time.monotonic() - start, 3)}
    # Preserve a recovered crash as a failure even when the watchdog exits 0.
    log = (directory / 'run.log').read_text(errors='replace')
    summary['recovered_crashes'] = log.count('[CRASH] sig=')
    if summary['recovered_crashes']:
        summary['status'] = 'RECOVERED_CRASH'
    if backend == 'reference' and settings.get('reference_anchor') is not None:
        # A fixture poke is placed at anchor+N: a different anchor means it
        # landed on another frame, so the capture does not count.
        seen = re.search(r'\[PARITY\] anchor=(\d+)', log)
        summary['anchor'] = int(seen.group(1)) if seen else None
        if summary['anchor'] != settings['reference_anchor'] and summary['status'] == 'OK':
            summary['status'] = 'ANCHOR_MISMATCH'
    elif settings.get('native_pokes') and summary['status'] == 'OK':
        if 'PC_POKE applied at navigation VBlank' not in log:
            summary['status'] = 'FIXTURE_NOT_APPLIED'
    if summary['status'] == 'OK':
        try:
            rows = records(directory / state_name, settings['checkpoints'])
            summary['end'] = [rows[-1][key] for key in ('now_step', 'now_sub_step', 'now_sub_sub_step')]
            summary['game_frame'] = rows[-1]['frame_counter']
        except (OSError, ValueError, KeyError) as exc:
            summary.update(status='INCOMPLETE', reason=str(exc))
    artifacts = {}
    owned = {state_name, audio_name}
    for point in settings['checkpoints']:
        owned.update((f'frame-{point}.vram', f'frame_{point:06d}.vram', f'frame_{point:06d}.ppm'))
    for path in sorted(directory.iterdir()):
        if path.is_file() and path.name in owned:
            artifacts[path.name] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
    summary['artifacts'] = artifacts
    return summary


def execute_scene(scene, out, settings):
    base = out / f'scene-{scene:03d}'
    captures = {}
    for backend in ['reference', *settings['renderers']]:
        if STOP.is_set():
            break
        try:
            captures[backend] = capture(backend, scene, base / backend, settings)
        except Exception as exc:
            captures[backend] = {'status': 'ERROR', 'reason': f'{type(exc).__name__}: {exc}'}
    comparisons = {}
    for backend in settings['renderers']:
        if any(captures.get(key, {}).get('status') != 'OK' for key in ('reference', backend)):
            comparisons[backend] = {'status': 'UNMEASURABLE', 'full_visual_audio_parity_proven': False}
            continue
        try:
            comparisons[backend] = compare(base / backend, base / 'reference', settings['checkpoints'])
        except (OSError, ValueError, KeyError) as exc:
            comparisons[backend] = {'status': 'UNMEASURABLE', 'reason': str(exc),
                                    'full_visual_audio_parity_proven': False}
    determinism = {}
    if scene in settings['repeat_scenes']:
        for backend in ['reference', *settings['renderers']]:
            if STOP.is_set():
                break
            repeated = capture(backend, scene, base / f'{backend}-repeat', settings)
            first = captures.get(backend, {})
            # Logs and host wall time deliberately excluded; compare all PCM,
            # VRAM, images and checkpoint-state bytes exactly.
            same = first.get('status') == repeated['status'] == 'OK' and (
                first.get('artifacts') == repeated.get('artifacts'))
            components = {}
            for group, suffixes in {'images': ('.ppm',), 'vram': ('.vram',),
                                    'pcm': ('.wav', '.s16le'), 'state': ('.jsonl',)}.items():
                before = {k: v for k, v in first.get('artifacts', {}).items() if k.endswith(suffixes)}
                after = {k: v for k, v in repeated.get('artifacts', {}).items() if k.endswith(suffixes)}
                components[group] = ('EXACT_REPEAT' if before and before == after else
                                     'NOT_CAPTURED' if not before and not after else 'DIFFERENT_OR_INCOMPLETE')
            determinism[backend] = {'status': 'EXACT_REPEAT' if same else 'DIFFERENT_OR_INCOMPLETE',
                                    'components': components, 'repeat': repeated}
    return {'scene': scene, 'captures': captures, 'comparisons': comparisons, 'determinism': determinism}


def summarize(rows):
    capture_counts, comparisons, visual, audio, deterministic = (Counter() for _ in range(5))
    state_divergences = 0
    for row in rows:
        capture_counts.update(item['status'] for item in row['captures'].values())
        deterministic.update(item['status'] for item in row['determinism'].values())
        for verdict in row['comparisons'].values():
            comparisons[verdict['status']] += 1
            if 'audio' in verdict:
                audio[verdict['audio']['status']] += 1
            for frame in verdict.get('frames', []):
                visual[frame['visual']['status']] += 1
                state_divergences += bool(frame['state_differences'])
    return {'captures': dict(capture_counts), 'comparisons': dict(comparisons),
            'visual_checkpoints': dict(visual), 'audio': dict(audio),
            'state_diverged_checkpoints': state_divergences, 'determinism': dict(deterministic)}


def write_report(path, report):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--reference-lib', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--scenes', default='0-210')
    parser.add_argument('--checkpoints', default='1,60,300,600,1200')
    parser.add_argument('--renderers', default='native,mednafen')
    parser.add_argument('--repeat-scenes', default='0,18,64,67,208')
    parser.add_argument('--jobs', type=int, default=2, choices=(1, 2))
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--fixture-poke', action='append', default=[], metavar='N:SYMBOL:HEX',
                        help='TEST FIXTURE: write HEX to SYMBOL in both machines N VBlanks '
                             'after navigation (after checkpoint N); needs --reference-anchor')
    parser.add_argument('--reference-anchor', type=int,
                        help='the reference [PARITY] anchor= host frame (scene 64: 1381)')
    args = parser.parse_args()
    try:
        scenes, checkpoints = scene_list(args.scenes), checkpoint_list(args.checkpoints)
        repeats = scene_list(args.repeat_scenes)
        native_pokes, reference_pokes = fixture_pokes(args.fixture_poke, args.reference_anchor)
    except ValueError as exc:
        parser.error(str(exc))
    if args.fixture_poke and len(scenes) != 1:
        parser.error('--fixture-poke is a single-scene fixture (anchors differ per scene)')
    renderers = args.renderers.split(',')
    if not renderers or len(set(renderers)) != len(renderers) or set(renderers) - {'native', 'mednafen'}:
        parser.error('--renderers must be native, mednafen, or native,mednafen')
    if not 0 < args.timeout <= 180:
        parser.error('--timeout must be in (0,180]')
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error('output directory must be new or empty; never reuse stale captures')
    filesystem = out
    while not filesystem.exists():
        filesystem = filesystem.parent
    required_bytes = artifact_budget(scenes, renderers, checkpoints, repeats)
    if shutil.disk_usage(filesystem).free < required_bytes:
        parser.error(f'output filesystem needs {required_bytes} free bytes for bounded artifacts')
    if not args.reference.is_file() or not (ROOT / 'pc/assets/raw').is_dir():
        parser.error('reference executable and local pc/assets/raw are required')
    out.mkdir(parents=True, exist_ok=True)
    settings = {'checkpoints': checkpoints, 'renderers': renderers, 'repeat_scenes': repeats,
                'reference': str(args.reference.resolve()), 'reference_lib': str(args.reference_lib.resolve()),
                'timeout': args.timeout, 'native_pokes': native_pokes,
                'reference_pokes': reference_pokes,
                'reference_anchor': args.reference_anchor if args.fixture_poke else None,
                'disc': 'disks/Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue'}
    report = {'schema': 1, 'complete': False, 'full_visual_audio_parity_proven': False,
              'scope': '211 possible retail goto_step entry scenarios, no input; not a full playthrough',
              'clock': 'post-navigation hardware VBlank', 'requested_scenes': scenes,
              'checkpoints': checkpoints, 'renderers': renderers, 'jobs': args.jobs,
              'timeout_seconds': args.timeout, 'native_sha256': digest(ROOT / 'pc/build/tokimeki-pc'),
              'reference_sha256': digest(args.reference), 'rows': [],
              'reference_display_crop_mode': 'Borders',
              'audio_window': 'completed post-navigation VBlank 1 through final checkpoint',
              'reference_gpu_status': 'passive derived getter; no emulated MMIO read',
              'test_fixture_pokes': {'native': native_pokes, 'reference': reference_pokes,
                                     'note': 'state-mutating test fixture, not natural parity evidence'}
              if args.fixture_poke else None,
              'artifact_budget_bytes': required_bytes,
              'tooling_sha256': {**{name: digest(ROOT / name) for name in (
                  'pc/tools/av_parity.py', 'pc/tools/parity_metrics.py', 'pc/hal/parity.c',
                  'pc/hal/spu.c', 'pc/hal/trace.c')},
                  **{f'refemu/{name}': digest(refemu.DIR / name) for name in (
                      'parity_regtest.h', 'build_parity_reference.py')
                     if (refemu.DIR / name).is_file()}},
              'environment_fixture_keys_removed': sorted(key for key in os.environ
                  if key not in clean_environment()),
              'limitations': ['SDL/window and display-mask parity not evaluated',
                              'FMV separate audio mixer not captured',
                              'No interactive story, serve input, voice-selection or gameplay coverage claim',
                              'Different game state makes image/audio comparison diagnostic, not lockstep proof',
                              'Phase-aligned audio diagnostics do not change exact PCM verdict']}
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: STOP.set())
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(execute_scene, scene, out, settings): scene for scene in scenes}
        for future in as_completed(futures):
            try:
                row = future.result()
            except Exception as exc:
                row = {'scene': futures[future], 'captures': {}, 'comparisons': {}, 'determinism': {},
                       'error': f'{type(exc).__name__}: {exc}'}
            report['rows'].append(row)
            report['rows'].sort(key=lambda entry: entry['scene'])
            report['summary'] = summarize(report['rows'])
            report['seconds'] = round(time.monotonic() - start, 3)
            write_report(out / 'report.json', report)
            print(f"{len(report['rows'])}/{len(scenes)} scene={row['scene']} "
                  + ','.join(f'{k}:{v["status"]}' for k, v in row['comparisons'].items()), flush=True)
    report['complete'] = len(report['rows']) == len(scenes) and not STOP.is_set()
    report['native_binary_unchanged'] = report['native_sha256'] == digest(ROOT / 'pc/build/tokimeki-pc')
    report['reference_binary_unchanged'] = report['reference_sha256'] == digest(args.reference)
    report['seconds'] = round(time.monotonic() - start, 3)
    write_report(out / 'report.json', report)
    print(json.dumps(report['summary'], indent=2))
    # A completed sweep of mismatches is not a successful parity verdict.
    return 0 if report['complete'] and report['full_visual_audio_parity_proven'] else 1


if __name__ == '__main__':
    sys.exit(main())
