#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert PS1 VAB (VH+VB) soundbanks to SoundFont 2 (SF2) files.

Output SF2 files are loadable by TinySoundFont (tsf_load_filename) so the
PC port can synthesise authentic game instruments instead of General MIDI.

Also writes per-sample WAVs + JSON manifest under a sibling "_raw/" dir so
assets are editable — replace a WAV, re-run vab2sf2, the SF2 is rebuilt.

Usage:
    python3 pc/tools/vab2sf2.py <INPUT.VH> <INPUT.VB> <OUTPUT.sf2>
    python3 pc/tools/vab2sf2.py --batch DIR              # convert every VH/VB pair
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import wave
from pathlib import Path
from asset_limits import read_asset
from typing import List, Tuple

# VAG ADPCM filter coefficients (×64). Sony PS1 spec.
VAG_FILTERS = [
    (0,    0),
    (60,   0),
    (115, -52),
    (98,  -55),
    (122, -60),
]

# PS1 SPU sample playback rate: pitch 0x1000 = 44100 Hz
PS1_SAMPLE_RATE = 44100


def decode_vag(data: bytes) -> List[int]:
    """Decode a single VAG ADPCM stream into signed 16-bit PCM samples."""
    out: List[int] = []
    p1 = 0
    p2 = 0
    i = 0
    while i + 16 <= len(data):
        shift_filter = data[i]
        flags = data[i + 1]
        shift = shift_filter & 0x0F
        filt = shift_filter >> 4
        if filt > 4:
            filt = 0
        f1, f2 = VAG_FILTERS[filt]
        payload = data[i + 2 : i + 16]
        for byte in payload:
            for nibble in (byte & 0x0F, byte >> 4):
                s = nibble - 16 if nibble & 0x08 else nibble
                # expand to 16-bit range, then apply shift
                s = s << 12
                s >>= shift
                s += (p1 * f1 + p2 * f2) >> 6
                if s > 32767:
                    s = 32767
                elif s < -32768:
                    s = -32768
                out.append(s)
                p2 = p1
                p1 = s
        i += 16
        # flags bit 0 = end of stream
        if flags & 0x01:
            break
    return out


def find_loop_point(data: bytes) -> int:
    """Scan VAG blocks for loop-start flag (bit 2). Returns sample index or -1."""
    for block_idx, i in enumerate(range(0, len(data), 16)):
        if i + 2 > len(data):
            break
        flags = data[i + 1]
        # bit 2 = loop start, bit 1 = loop region flag (set on looped samples)
        if flags & 0x04:
            return block_idx * 28
    return -1


def parse_vab(vh_data: bytes) -> Tuple[dict, List[Tuple[int, int]]]:
    """Parse VAB header. Returns (bank_info, vag_offsets).

    vag_offsets is a list of (start, length) tuples into the VB data.
    """
    if vh_data[:4] != b'pBAV':
        raise ValueError("Not a VAB file (bad magic)")

    (form, ver, vab_id, total_size, reserved,
     num_prog, num_tone, num_vag,
     master_vol, master_pan, attr1, attr2) = struct.unpack_from(
        '<4sIIIHHHHBBBB', vh_data, 0
    )
    # 4 reserved bytes follow attr2 to round header to 32 bytes.

    programs = []
    # Program table: 128 slots × 16 bytes starting at offset 32
    for i in range(128):
        off = 32 + i * 16
        (n_tones, mvol, prior, mode, mpan,
         reserv0, attr, _r1, _r2) = struct.unpack_from('<BBBBBBHII', vh_data, off)
        programs.append({
            'index': i,
            'num_tones': n_tones,
            'volume': mvol,
            'priority': prior,
            'mode': mode,
            'pan': mpan,
            'attr': attr,
            'tones': [],
        })

    # Tone table at offset 2080: one 16×32-byte block per program with tones,
    # packed sequentially in program-index order.
    tone_offset = 32 + 128 * 16
    slot = 0
    for prog in programs:
        if prog['num_tones'] == 0:
            continue
        for t in range(16):
            off = tone_offset + (slot * 16 + t) * 32
            if off + 32 > len(vh_data):
                break
            (prior, mode, vol, pan, center, shift,
             min_note, max_note, vibW, vibT, porW, porT,
             pbMin, pbMax, _r0, _r1,
             adsr1, adsr2, parent_prog, vag_idx,
             _r2, _r3, _r4, _r5) = struct.unpack_from('<BBBBBBBBBBBBBBBBHHHHHHHH', vh_data, off)
            if vag_idx == 0:
                continue
            prog['tones'].append({
                'center': center,
                'shift_center': shift,
                'min_note': min_note,
                'max_note': max_note,
                'volume': vol,
                'pan': pan,
                'vag_index': vag_idx - 1,   # 0-based
                'adsr1': adsr1,
                'adsr2': adsr2,
                'pitch_min': pbMin,
                'pitch_max': pbMax,
            })
        slot += 1

    # VAG size table is always 512 bytes (256 × 2), immediately after tones.
    num_prog_with_tones = slot
    vag_table_off = tone_offset + num_prog_with_tones * 16 * 32
    vag_sizes = []
    # First entry is reserved (zero). Read num_vag + 1 entries.
    usable = min(256, num_vag + 1)
    for i in range(usable):
        off = vag_table_off + i * 2
        if off + 2 > len(vh_data):
            break
        sz_units = struct.unpack_from('<H', vh_data, off)[0]
        vag_sizes.append(sz_units * 8)

    # Real samples start at index 1 (skip reserved zeroth).
    vag_offsets = []
    cursor = 0
    for sz in vag_sizes[1:]:
        vag_offsets.append((cursor, sz))
        cursor += sz

    return {
        'version': ver,
        'vab_id': vab_id,
        'num_programs': num_prog,
        'num_tones': num_tone,
        'num_vag': num_vag,
        'master_volume': master_vol,
        'master_pan': master_pan,
        'programs': programs,
    }, vag_offsets


def write_wav(path: Path, samples: List[int], rate: int = PS1_SAMPLE_RATE) -> None:
    """Write 16-bit mono PCM WAV for human inspection / editing."""
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f'<{len(samples)}h', *samples))


# ----------------------------------------------------------------------------
# SoundFont 2 writer (minimal — enough for TinySoundFont to play the samples)
# ----------------------------------------------------------------------------
#
# SF2 structure:
#   RIFF 'sfbk'
#     LIST 'INFO'   — version + strings
#     LIST 'sdta'   — 16-bit sample data ('smpl')
#     LIST 'pdta'   — presets, instruments, sample headers
#
# We emit one preset per VAB program, each referencing one instrument, each
# with one zone per VAB tone. Samples are concatenated into the sdta/smpl
# chunk with 46 zero samples of padding between each (required by SF2 spec).


def _pad(b: bytes) -> bytes:
    """SF2 chunks must be even-length; pad with 0x00."""
    return b + b'\x00' if len(b) & 1 else b


def _chunk(fourcc: bytes, payload: bytes) -> bytes:
    return fourcc + struct.pack('<I', len(payload)) + _pad(payload)


def _list_chunk(listcc: bytes, payloads: List[bytes]) -> bytes:
    inner = listcc + b''.join(payloads)
    return _chunk(b'LIST', inner)


def _fixed_str(s: str, length: int) -> bytes:
    b = s.encode('ascii', errors='replace')[: length - 1]
    return b.ljust(length, b'\x00')


def _sf2_generator(gen_oper: int, amount_s: int) -> bytes:
    """SF2 generator entry: 2-byte oper + 2-byte amount (signed or range)."""
    return struct.pack('<Hh', gen_oper, amount_s)


def _sf2_gen_range(gen_oper: int, lo: int, hi: int) -> bytes:
    return struct.pack('<HBB', gen_oper, lo, hi)


# SF2 generator opers
GEN_KEY_RANGE = 43
GEN_VEL_RANGE = 44
GEN_INST = 41
GEN_SAMPLE_ID = 53
GEN_OVERRIDING_ROOT_KEY = 58
GEN_FINE_TUNE = 52
GEN_COARSE_TUNE = 51
GEN_INITIAL_ATTENUATION = 48
GEN_PAN = 17
GEN_SAMPLE_MODE = 54   # 0 = no loop, 1 = loop

# Sample types
SAMPLE_MONO = 1


def _semitones_cents(ps1_center: int, ps1_shift: int) -> Tuple[int, int]:
    """Map PS1 tone centre+shift to SF2 root-key + fine-tune cents.

    PS1 tone 'center' is the MIDI note the sample plays unshifted. 'shift'
    is a signed 8-bit fine offset in 1/128 semitone units (typical usage).
    We treat it as signed cents/128 * 100 for simplicity.
    """
    root = max(0, min(127, ps1_center))
    # shift is unsigned in struct, but treated as signed 8-bit in practice.
    shift = ps1_shift - 256 if ps1_shift >= 128 else ps1_shift
    fine = max(-99, min(99, shift * 100 // 128))
    return root, fine


def build_sf2(bank_name: str,
              bank_info: dict,
              sample_streams: List[Tuple[List[int], int]]) -> bytes:
    """Build a minimal SF2 byte blob.

    sample_streams: list of (pcm_samples, loop_start) per VAG. loop_start=-1
    if the sample isn't looped.
    """
    # ---- 1. Concatenate all sample data with required 46-sample zero padding.
    SAMPLE_PAD = 46
    sample_bytes = bytearray()
    sample_headers = []  # list of dicts for shdr chunk
    pos_samples = 0
    for idx, (pcm, loop_start) in enumerate(sample_streams):
        start = pos_samples
        pcm_len = len(pcm)
        sample_bytes += struct.pack(f'<{pcm_len}h', *pcm)
        sample_bytes += b'\x00\x00' * SAMPLE_PAD
        end = start + pcm_len
        if loop_start < 0 or loop_start >= pcm_len - 2:
            loop_s = start
            loop_e = end
        else:
            loop_s = start + loop_start
            loop_e = end - 1
        sample_headers.append({
            'name': f'S{idx:03d}',
            'start': start,
            'end': end,
            'loop_start': loop_s,
            'loop_end': loop_e,
            'rate': PS1_SAMPLE_RATE,
            'root': 60,
            'fine': 0,
            'looped': loop_start >= 0,
        })
        pos_samples = end + SAMPLE_PAD

    # ---- 2. INFO chunk
    info_payloads = [
        _chunk(b'ifil', struct.pack('<HH', 2, 1)),                    # SF 2.01
        _chunk(b'isng', _fixed_str('EMU8000', 8)),
        _chunk(b'INAM', _fixed_str(bank_name, 20)),
    ]
    info = _list_chunk(b'INFO', info_payloads)

    # ---- 3. sdta chunk (just smpl)
    sdta = _list_chunk(b'sdta', [_chunk(b'smpl', bytes(sample_bytes))])

    # ---- 4. pdta: build presets and instruments.
    preset_headers = []  # phdr entries
    preset_bags = []     # pbag entries
    preset_gens = []     # pgen entries
    instruments = []     # inst entries
    inst_bags = []       # ibag entries
    inst_gens = []       # igen entries

    programs = [p for p in bank_info['programs'] if p['tones']]
    for preset_idx, prog in enumerate(programs):
        # Each preset has one bag pointing at one pgen (instrument ref).
        bag_idx = len(preset_bags)
        gen_idx = len(preset_gens)
        preset_headers.append({
            'name': f'P{prog["index"]:03d}',
            'preset': prog['index'],
            'bank': 0,
            'bag_idx': bag_idx,
        })
        preset_bags.append({'gen_idx': gen_idx, 'mod_idx': 0})
        preset_gens.append(_sf2_generator(GEN_INST, preset_idx))

        # Instrument: one bag per tone (zone)
        inst_bag_base = len(inst_bags)
        instruments.append({
            'name': f'I{prog["index"]:03d}',
            'bag_idx': inst_bag_base,
        })

        for tone in prog['tones']:
            gen_base = len(inst_gens)
            inst_bags.append({'gen_idx': gen_base, 'mod_idx': 0})

            lo = max(0, min(127, tone['min_note']))
            hi = max(0, min(127, tone['max_note']))
            if hi < lo:
                lo, hi = hi, lo
            root, fine = _semitones_cents(tone['center'], tone['shift_center'])
            vag = tone['vag_index']
            if vag >= len(sample_headers):
                continue
            # Attenuation: SF2 is centibels. PS1 volume 0-127 maps to 0-40 dB.
            vol = max(1, tone['volume'])
            atten_cb = int((127 - vol) * 10)  # 0 = loudest, 1270 = silent
            # Pan: SF2 is -500..+500 (tenths of percent). PS1 pan 0-127, 64 center.
            pan_sf2 = int((tone['pan'] - 64) * 500 / 64)

            inst_gens.append(_sf2_gen_range(GEN_KEY_RANGE, lo, hi))
            inst_gens.append(_sf2_generator(GEN_INITIAL_ATTENUATION, atten_cb))
            inst_gens.append(_sf2_generator(GEN_PAN, pan_sf2))
            inst_gens.append(_sf2_generator(GEN_OVERRIDING_ROOT_KEY, root))
            if fine:
                inst_gens.append(_sf2_generator(GEN_FINE_TUNE, fine))
            if sample_headers[vag]['looped']:
                inst_gens.append(_sf2_generator(GEN_SAMPLE_MODE, 1))
            # SAMPLE_ID must always be the final generator in an inst zone.
            inst_gens.append(_sf2_generator(GEN_SAMPLE_ID, vag))

    # Terminal sentinel entries
    preset_headers.append({'name': 'EOP', 'preset': 0, 'bank': 0,
                           'bag_idx': len(preset_bags)})
    preset_bags.append({'gen_idx': len(preset_gens), 'mod_idx': 0})
    instruments.append({'name': 'EOI', 'bag_idx': len(inst_bags)})
    inst_bags.append({'gen_idx': len(inst_gens), 'mod_idx': 0})

    # ---- Build each pdta sub-chunk ----
    phdr_bytes = b''
    for ph in preset_headers:
        phdr_bytes += (_fixed_str(ph['name'], 20)
                       + struct.pack('<HHHIII',
                                     ph['preset'], ph['bank'], ph['bag_idx'],
                                     0, 0, 0))
    pbag_bytes = b''.join(struct.pack('<HH', b['gen_idx'], b['mod_idx'])
                          for b in preset_bags)
    pmod_bytes = b'\x00' * 10  # terminal modulator only
    pgen_bytes = b''.join(preset_gens) + b'\x00' * 4  # terminal generator

    inst_bytes = b''
    for inst in instruments:
        inst_bytes += _fixed_str(inst['name'], 20) + struct.pack('<H', inst['bag_idx'])
    ibag_bytes = b''.join(struct.pack('<HH', b['gen_idx'], b['mod_idx'])
                          for b in inst_bags)
    imod_bytes = b'\x00' * 10
    igen_bytes = b''.join(inst_gens) + b'\x00' * 4

    shdr_bytes = b''
    for sh in sample_headers:
        shdr_bytes += (_fixed_str(sh['name'], 20)
                       + struct.pack('<IIIIIBbHH',
                                     sh['start'], sh['end'],
                                     sh['loop_start'], sh['loop_end'],
                                     sh['rate'],
                                     sh['root'], sh['fine'],
                                     0,            # sampleLink
                                     SAMPLE_MONO))
    # Terminal sample header
    shdr_bytes += _fixed_str('EOS', 20) + struct.pack('<IIIIIBbHH',
                                                      0, 0, 0, 0, 0, 0, 0, 0, 0)

    pdta_payloads = [
        _chunk(b'phdr', phdr_bytes),
        _chunk(b'pbag', pbag_bytes),
        _chunk(b'pmod', pmod_bytes),
        _chunk(b'pgen', pgen_bytes),
        _chunk(b'inst', inst_bytes),
        _chunk(b'ibag', ibag_bytes),
        _chunk(b'imod', imod_bytes),
        _chunk(b'igen', igen_bytes),
        _chunk(b'shdr', shdr_bytes),
    ]
    pdta = _list_chunk(b'pdta', pdta_payloads)

    # ---- Wrap in RIFF sfbk ----
    sfbk_inner = b'sfbk' + info + sdta + pdta
    return _chunk(b'RIFF', sfbk_inner)


def convert(vh_path: Path, vb_path: Path, sf2_path: Path,
            raw_dir: Path | None = None) -> None:
    vh = read_asset(vh_path, 1024 * 1024)
    vb = read_asset(vb_path, 2 * 1024 * 1024)

    bank, vag_offsets = parse_vab(vh)

    sample_streams = []
    wavs_written = []
    total_samples = 0
    for idx, (off, sz) in enumerate(vag_offsets):
        if sz == 0:
            sample_streams.append(([0], -1))
            continue
        if off < 0 or sz < 0 or off + sz > len(vb):
            raise ValueError('VAG sample range extends beyond VB')
        raw = vb[off : off + sz]
        pcm = decode_vag(raw)
        if not pcm:
            pcm = [0]
        total_samples += len(pcm)
        if total_samples > 2 * 1024 * 1024:
            raise ValueError('decoded soundbank exceeds sample budget')
        loop_start = find_loop_point(raw)
        sample_streams.append((pcm, loop_start))

        if raw_dir is not None:
            wav_path = raw_dir / f'sample_{idx:03d}.wav'
            write_wav(wav_path, pcm)
            wavs_written.append(wav_path.name)

    sf2 = build_sf2(sf2_path.stem.upper(), bank, sample_streams)
    sf2_path.write_bytes(sf2)

    if raw_dir is not None:
        manifest = {
            'source_vh': vh_path.name,
            'source_vb': vb_path.name,
            'num_programs': bank['num_programs'],
            'num_tones': bank['num_tones'],
            'num_vag': bank['num_vag'],
            'master_volume': bank['master_volume'],
            'master_pan': bank['master_pan'],
            'programs': [
                {
                    'index': p['index'],
                    'volume': p['volume'],
                    'pan': p['pan'],
                    'tones': p['tones'],
                }
                for p in bank['programs'] if p['tones']
            ],
            'wavs': wavs_written,
            'sample_rate': PS1_SAMPLE_RATE,
        }
        (raw_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2))


def batch(root: Path, sf2_out: Path, raw_out: Path | None) -> None:
    sf2_out.mkdir(parents=True, exist_ok=True)
    vh_files = sorted(root.rglob('*.VH'))
    if not vh_files:
        print(f'No .VH files under {root}', file=sys.stderr)
        sys.exit(1)
    ok = 0
    for vh in vh_files:
        vb = vh.with_suffix('.VB')
        if not vb.exists():
            print(f'  SKIP {vh.name}: no matching .VB')
            continue
        name = vh.stem
        sf2_path = sf2_out / f'{name}.sf2'
        this_raw = raw_out / name if raw_out else None
        if this_raw is not None:
            this_raw.mkdir(parents=True, exist_ok=True)
        try:
            convert(vh, vb, sf2_path, this_raw)
            ok += 1
            print(f'  OK   {name} → {sf2_path.name}')
        except Exception as e:
            print(f'  FAIL {name}: {e}')
    print(f'\nConverted {ok}/{len(vh_files)} banks.')


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('vh', nargs='?', help='Input .VH path')
    ap.add_argument('vb', nargs='?', help='Input .VB path')
    ap.add_argument('sf2', nargs='?', help='Output .sf2 path')
    ap.add_argument('--batch', metavar='DIR',
                    help='Convert every VH/VB pair under DIR recursively')
    ap.add_argument('--out', default='pc/assets/soundbanks',
                    help='[batch] output directory for SF2 files')
    ap.add_argument('--raw', default='pc/assets/soundbanks/_raw',
                    help='[batch] directory for per-bank WAV + JSON')
    ap.add_argument('--no-raw', action='store_true',
                    help='Skip WAV/manifest dump (SF2 only)')
    args = ap.parse_args()

    raw_root = None if args.no_raw else Path(args.raw)

    if args.batch:
        out = Path(args.out)
        batch(Path(args.batch), out, raw_root)
        return

    if not (args.vh and args.vb and args.sf2):
        ap.print_help()
        sys.exit(1)

    vh, vb, sf2 = Path(args.vh), Path(args.vb), Path(args.sf2)
    raw_dir = None
    if raw_root is not None:
        raw_dir = raw_root / sf2.stem
        raw_dir.mkdir(parents=True, exist_ok=True)
    convert(vh, vb, sf2, raw_dir)
    print(f'Wrote {sf2}')


if __name__ == '__main__':
    main()
