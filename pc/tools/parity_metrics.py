# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict framebuffer/PCM measurements, never a smoke-test-as-parity verdict.

BGR555 uses the native HAL's shift3 expansion; RGB24 retains all eight bits.
Different viewport sizes are a mismatch, not resized/cropped into a pass.
PCM equality is exact;
bounded phase alignment is a separately labeled diagnostic, not a waiver.
Silence/black matches cannot demonstrate content fidelity.
"""
import hashlib
import json
from pathlib import Path
import wave

import numpy as np

STATE_FIELDS = ('now_step', 'now_sub_step', 'now_sub_sub_step', 'frame_counter')
MAX_AUDIO_BYTES = 8 * 1024 * 1024


def digest(path):
    result = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def records(path, expected):
    if Path(path).stat().st_size > 32768:
        raise ValueError('oversized capture manifest')
    rows = [json.loads(line) for line in Path(path).read_text().splitlines()]
    if [row['frame'] for row in rows] != list(expected):
        raise ValueError('missing/duplicate/out-of-order checkpoint')
    return rows


def _display_flag(row, name, bit, invert=False):
    """Keep legacy unknowns explicit; reject contradictory device metadata."""
    declared = None
    if name in row:
        if not isinstance(row[name], int) or row[name] not in (0, 1):
            raise ValueError(f'invalid {name} flag')
        declared = bool(row[name])
    if 'gpu_status' not in row:
        return declared
    if not isinstance(row['gpu_status'], int):
        raise ValueError('invalid GPU status')
    observed = bool(row['gpu_status'] & (1 << bit)) != invert
    if declared is not None and declared != observed:
        raise ValueError(f'contradictory {name} flag')
    return observed


def viewport(directory, row):
    """Read stored scanout source, not arbitrary texture regions or SDL UI."""
    name = row['vram']
    if Path(name).name != name:
        raise ValueError('unsafe image path')
    path = Path(directory) / name
    if path.stat().st_size != 1024 * 512 * 2:
        raise ValueError('VRAM must contain exactly 1024x512 little-endian words')
    x, y, width, height = row.get('viewport', row.get('crop'))
    if not (0 < width <= 1024 and 0 < height <= 512):
        raise ValueError('invalid viewport dimensions')
    vram = np.fromfile(path, dtype='<u2').reshape(512, 1024)
    enabled = _display_flag(row, 'display_enabled', 23, invert=True)
    rgb24 = _display_flag(row, 'display_rgb24', 21)
    if enabled is False:
        return np.zeros((height, width, 3), dtype=np.uint8)
    if rgb24:
        # CRTC source X wraps modulo1024: X=0 alone cannot prove a zero
        # programmed origin/skip. Decode only proven unskipped, unwrapped
        # rows; shifted RGB24 needs additional scanout-phase metadata.
        base = row.get('display_start_x')
        if row.get('image_source') == 'gpu_scanout' and 'display_rgb24' in row:
            display = row.get('display')
            base = (display[0] if isinstance(display, list) and len(display) == 4
                    and display[:2] == [x, y] else None)
        if (base is None or base != x or x < 0 or x * 2 + width * 3 > 2048 or
                y < 0 or y + height > 512):
            raise ValueError('RGB24 origin/wrap needs unskipped programmed-X metadata')
        packed = vram.view(np.uint8).reshape(512, 2048)
        start = x * 2
        return packed[y:y + height, start:start + width * 3].reshape(height, width, 3).copy()
    # PS1 scanout wraps at VRAM edges; native snapshots explicitly pad
    # out-of-bounds pixels. Reject that mismatch rather than invent pixels.
    if x < 0 or y < 0 or x + width > 1024 or y + height > 512:
        raise ValueError('viewport wraps outside VRAM; unsupported comparison')
    words = vram[y:y + height, x:x + width] & 0x7fff
    rgb = np.stack([words & 31, (words >> 5) & 31, (words >> 10) & 31], axis=-1).astype(np.uint8)
    return rgb << 3


def visual(native_dir, native, reference_dir, reference):
    left = viewport(native_dir, native)
    right = viewport(reference_dir, reference)
    same_shape = left.shape == right.shape
    height, width = min(left.shape[0], right.shape[0]), min(left.shape[1], right.shape[1])
    a, b = left[:height, :width], right[:height, :width]
    changed = np.any(a != b, axis=2)
    nonblack_a, nonblack_b = np.any(left != 0, axis=2), np.any(right != 0, axis=2)
    native_enabled = _display_flag(native, 'display_enabled', 23, invert=True)
    reference_enabled = _display_flag(reference, 'display_enabled', 23, invert=True)
    native_mode = _display_flag(native, 'display_rgb24', 21)
    reference_mode = _display_flag(reference, 'display_rgb24', 21)
    mask_known = native_enabled is not None and reference_enabled is not None
    mode_known = native_mode is not None and reference_mode is not None
    if not same_shape:
        status = 'DIMENSION_MISMATCH'
    elif mask_known and native_enabled != reference_enabled:
        status = 'DISPLAY_MASK_MISMATCH'
    elif mode_known and native_mode != reference_mode:
        status = 'DISPLAY_MODE_MISMATCH'
    elif changed.any():
        status = 'PIXELS_DIFFER'
    else:
        status = 'FRAMEBUFFER_MATCH' if nonblack_a.any() else 'BLACK_ONLY_UNVERIFIED'
    return {
        'status': status,
        'native_size': [left.shape[1], left.shape[0]],
        'reference_size': [right.shape[1], right.shape[0]],
        'native_nonblack_fraction': float(nonblack_a.mean()),
        'reference_nonblack_fraction': float(nonblack_b.mean()),
        'common_top_left_diagnostic': {
            'size': [width, height],
            'changed_pixels': int(changed.sum()),
            'changed_fraction': float(changed.mean()),
            'mae_8bit': float(np.abs(a.astype(np.int16) - b.astype(np.int16)).mean()),
        },
        'color_comparison': 'native shift3 for BGR555; exact packed RGB8 for RGB24',
        'native_format': 'rgb24' if native_mode else 'bgr555',
        'reference_format': 'rgb24' if reference_mode else 'bgr555',
        'native_display_disabled': None if native_enabled is None else not native_enabled,
        'reference_display_disabled': None if reference_enabled is None else not reference_enabled,
        'display_mask_parity_evaluated': mask_known,
        'display_mask_equal': native_enabled == reference_enabled if mask_known else None,
        'display_pixel_mode_parity_evaluated': mode_known,
        'display_pixel_mode_equal': native_mode == reference_mode if mode_known else None,
    }


def pcm(directory, row):
    directory = Path(directory)
    if (directory / 'audio.s16le').exists():
        path = directory / 'audio.s16le'
        count = row['audio_bytes']
        if (count != row['audio_samples'] * 4 or path.stat().st_size < count or
                path.stat().st_size > MAX_AUDIO_BYTES or count > MAX_AUDIO_BYTES):
            raise ValueError('truncated/oversized/inconsistent native PCM')
        data = path.read_bytes()[:count]
        rate = row['audio_rate']
    else:
        path = directory / 'audio.wav'
        if path.stat().st_size > MAX_AUDIO_BYTES:
            raise ValueError('oversized reference WAV')
        with wave.open(str(path)) as stream:
            if stream.getnchannels() != 2 or stream.getsampwidth() != 2 or stream.getcomptype() != 'NONE':
                raise ValueError('expected stereo 16-bit PCM WAV')
            declared_frames = stream.getnframes()
            if declared_frames * 4 > MAX_AUDIO_BYTES:
                raise ValueError('oversized declared WAV sample count')
            data = stream.readframes(declared_frames)
            if len(data) != declared_frames * 4:
                raise ValueError('truncated WAV payload')
            rate = stream.getframerate()
    if len(data) % 4 or not data:
        raise ValueError('empty/misaligned PCM')
    if rate != 44100:
        raise ValueError('sample rates differ from declared 44100 Hz')
    return np.frombuffer(data, dtype='<i2').reshape(-1, 2)


def audio_arrays(left, right, max_lag=4410):
    """Exact raw comparison plus +/-100ms phase diagnostic at 44.1kHz."""
    if left.ndim != 2 or right.ndim != 2 or left.shape[1] != 2 or right.shape[1] != 2:
        raise ValueError('expected nonempty stereo PCM')
    if not len(left) or not len(right):
        raise ValueError('empty PCM')
    a, b = left.astype(np.float64), right.astype(np.float64)
    rms_a, rms_b = float(np.sqrt(np.mean(a * a))), float(np.sqrt(np.mean(b * b)))
    count = min(len(a), len(b))
    exact = left.shape == right.shape and np.array_equal(left, right)
    if rms_a == 0 and rms_b == 0:
        status = 'SILENCE_ONLY_UNVERIFIED'
    elif rms_a == 0 or rms_b == 0:
        status = 'MISSING_AUDIO'
    else:
        status = 'PCM_MATCH' if exact else 'PCM_DIFFERENT'
    result = {
        'status': status, 'native_samples': len(a), 'reference_samples': len(b),
        'sample_count_difference': len(a) - len(b), 'exact_pcm_equal': bool(exact),
        'native_rms': rms_a, 'reference_rms': rms_b,
        'native_peak': int(np.abs(a).max()), 'reference_peak': int(np.abs(b).max()),
        'raw_changed_sample_fraction': float(np.mean(left[:count] != right[:count])),
    }
    if rms_a == 0 or rms_b == 0:
        return result
    # Choose the most energetic reference channel; downmixing can cancel
    # anti-phase stereo. Full stereo is still used for the aligned error.
    channel = int(np.argmax(np.mean(b * b, axis=0)))
    aa, bb = a[:, channel], b[:, channel]
    nfft = 1 << (len(aa) + len(bb) - 2).bit_length()
    correlation = np.fft.irfft(np.fft.rfft(aa, nfft) * np.conj(np.fft.rfft(bb, nfft)), nfft)
    limit = min(max_lag, len(aa) - 1, len(bb) - 1)
    lags = np.arange(-limit, limit + 1)
    lag = int(lags[np.argmax(correlation[lags % nfft])])
    start_a, start_b = max(lag, 0), max(-lag, 0)
    count = min(len(a) - start_a, len(b) - start_b)
    aa, bb = a[start_a:start_a + count], b[start_b:start_b + count]
    denominator = np.sqrt(np.sum(aa * aa) * np.sum(bb * bb))
    result['phase_diagnostic'] = {
        'search_limit_samples': max_lag, 'lag_samples': lag,
        'overlap_samples': count,
        'stereo_correlation': float(np.sum(aa * bb) / denominator) if denominator else None,
        'rmse': float(np.sqrt(np.mean((aa - bb) ** 2))),
        'changes_verdict': False,
    }
    return result


def window_audio(native_dir, native, reference_dir, reference):
    if reference.get('audio_start_frame') != 1 or not reference.get('audio_start_tick'):
        raise ValueError('reference PCM window is not boundary-anchored')
    # Explicitly compare [completed VBlank 1, completed VBlank N]. Native
    # captures also retain [navigation, VBlank 1]; exclude that documented
    # interval, NOT an inferred phase/count correction or content trimming.
    prefix = native['ticks_per_vblank'] * native['audio_rate'] // native['clock_hz']
    start_tick, end_tick = reference['audio_start_tick'], reference['tick']
    if end_tick <= start_tick or native['frame'] < 2:
        raise ValueError('empty reference audio window')
    expected_ticks = (native['frame'] - 1) * native['ticks_per_vblank']
    actual_ticks = end_tick - start_tick
    result = audio_arrays(pcm(native_dir, native)[prefix:], pcm(reference_dir, reference))
    result['window'] = {
        'start_postnav_vblank': 1, 'end_postnav_vblank': native['frame'],
        'native_excluded_prefix_samples': prefix,
        'reference_start_tick': start_tick, 'reference_end_tick': end_tick,
        'reference_elapsed_ticks': actual_ticks, 'native_nominal_ticks': expected_ticks,
        'aligned_vblank_boundaries': abs(actual_ticks - expected_ticks) <= 768,
        'max_tick_quantization_slack': 768,
    }
    if not result['window']['aligned_vblank_boundaries']:
        result['measured_status'] = result['status']
        result['status'] = 'WINDOW_UNVERIFIED'
    return result


def compare(native_dir, reference_dir, checkpoints):
    native = records(Path(native_dir) / 'state.jsonl', checkpoints)
    reference = records(Path(reference_dir) / 'frames.jsonl', checkpoints)
    frames = []
    for left, right in zip(native, reference):
        differences = {field: {'native': left[field], 'reference': right[field]}
                       for field in STATE_FIELDS if left[field] != right[field]}
        try:
            image = visual(native_dir, left, reference_dir, right)
        except ValueError as exc:
            image = {'status': 'UNMEASURABLE', 'reason': str(exc)}
        frames.append({'frame': left['frame'], 'state_differences': differences, 'visual': image})
    sound = window_audio(native_dir, native[-1], reference_dir, reference[-1])
    sound['compared_state_fields_match_at_all_checkpoints'] = not any(
        frame['state_differences'] for frame in frames)
    sound['compared_state_fields'] = list(STATE_FIELDS)
    flags = []
    if any(frame['state_differences'] for frame in frames):
        flags.append('STATE_DIVERGED')
    if any(frame['visual']['status'] != 'FRAMEBUFFER_MATCH' for frame in frames):
        flags.append('FRAMEBUFFER_NOT_PROVEN')
    if sound['status'] != 'PCM_MATCH':
        flags.append('AUDIO_NOT_PROVEN')
    return {
        'status': 'DIFFERENT_OR_UNVERIFIED' if flags else 'FRAMEBUFFER_PCM_MATCH',
        'flags': flags, 'frames': frames, 'audio': sound,
        'full_visual_audio_parity_proven': False,
        'scope': 'post-navigation no-input checkpoints; display masks/modes only with explicit metadata; no FMV-mixer or interactive claim',
    }
