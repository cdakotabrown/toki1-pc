# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict, streaming reader for the PC port's one supported retail data track.

The whole-track pin is measured from this project's verified local dump, not
an independently asserted Redump identity. The executable pin is the existing
project gate. Cooked 2048-byte ISOs cannot retain the XA/STR framing we need.
"""
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import struct

from build_lba_map import read_sector_user, walk

SECTOR = 2352
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
MAX_OUTPUT = 2 * 1024**3
ANOMALY_REASON = 'source XA subheader copies differ; bytes preserved unchanged'


@dataclass(frozen=True)
class Profile:
    track_size: int
    track_sha1: str
    exe_lba: int
    exe_size: int
    exe_sha1: str
    file_count: int
    audio_size: int = 0
    audio_sha1: str = ''
    audio_skip: int = 0
    cdda_lba: int = 0
    cdda_size: int = 0
    preserved_subheader_lba: int = -1


SUPPORTED = Profile(702104928, '7b2f72d1319db27d0700070aa0bffe886b5ef7a4',
                    24, 690176, '2d8d474816e4bf937b09dc3b017f550f7ce4e14b', 1372,
                    45151344, '6cdd8500168ab4ecb18b78ef7589586d26bc4501',
                    150, 298664, 39008256, 121080)


def hash_file(path, algorithm='sha256'):
    with Path(path).open('rb') as source:
        return hash_stream(source, algorithm)


def hash_stream(source, algorithm):
    digest = hashlib.new(algorithm)
    while block := source.read(1024 * 1024):
        digest.update(block)
    return digest.hexdigest()


def safe_relative(name):
    """Reject traversal and cross-platform path aliases, rather than sanitize."""
    if not isinstance(name, str) or not name or len(name) > 240:
        raise ValueError('invalid asset path length')
    parts = name.split('/')
    if any(part in ('', '.', '..') or part.endswith((' ', '.')) for part in parts):
        raise ValueError(f'unsafe asset path: {name!r}')
    if any(not 32 <= ord(char) < 127 or char in '\\:<>"|?*' for char in name):
        raise ValueError(f'unsafe asset path: {name!r}')
    reserved = {'CON', 'PRN', 'AUX', 'NUL'} | {
        f'{prefix}{number}' for prefix in ('COM', 'LPT') for number in range(1, 10)}
    if any(part.split('.')[0].upper() in reserved for part in parts):
        raise ValueError(f'reserved asset path: {name!r}')
    return PurePosixPath(name)


def resolve_sources(path, audio_track=None):
    path = Path(path).expanduser().resolve(strict=True)
    if not path.is_file():
        raise ValueError('disc image must be a regular file')
    if path.suffix.lower() != '.cue':
        # Convenience for the conventional split-BIN name; never guess by glob.
        partner = re.sub(r'(?i)(\(track\s+)1(\))', r'\g<1>2\2', path.name)
        audio = Path(audio_track).expanduser() if audio_track else path.with_name(partner)
        return path, audio.resolve() if audio != path else None, None
    if audio_track is not None:
        raise ValueError('use CUE track references OR --audio-track, not both')
    if path.stat().st_size > 65536:
        raise ValueError('CUE exceeds 64 KiB')
    current_file = None
    current_format = None
    current_track = None
    tracks = {}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if len(line) > 4096:
            raise ValueError('CUE line exceeds 4 KiB')
        line = line.strip()
        fields = line.split()
        keyword = fields[0].upper() if fields else ''
        if keyword == 'FILE':
            match = re.fullmatch(r'FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)', line, re.I)
            if not match:
                raise ValueError('invalid CUE FILE record')
            current_file, current_format = match[1] or match[2], match[3].upper()
        elif keyword == 'TRACK':
            match = re.fullmatch(r'TRACK\s+(\d+)\s+(\S+)', line, re.I)
            if not match:
                raise ValueError('invalid CUE TRACK record')
            current_track = int(match[1])
            if current_track in tracks or current_file is None:
                raise ValueError('duplicate CUE track or missing FILE')
            tracks[current_track] = [path.parent / current_file, current_format, match[2].upper(), None]
        elif keyword == 'INDEX' and len(fields) >= 2 and fields[1] == '01':
            match = re.fullmatch(r'INDEX\s+01\s+(\d{2}):(\d{2}):(\d{2})', line, re.I)
            if not match or current_track not in tracks or tracks[current_track][3] is not None:
                raise ValueError('invalid or duplicate CUE INDEX 01')
            minute, second, frame = map(int, match.groups())
            if second >= 60 or frame >= 75:
                raise ValueError('invalid CUE index time')
            tracks[current_track][3] = (minute * 60 + second) * 75 + frame
    if 1 not in tracks or tracks[1][1:] != ['BINARY', 'MODE2/2352', 0]:
        raise ValueError('need BINARY Track 1 MODE2/2352 with INDEX 01 00:00:00')
    audio = tracks.get(2)
    if audio and (audio[1:3] != ['BINARY', 'AUDIO'] or audio[3] is None):
        raise ValueError('need BINARY Track 2 AUDIO with INDEX 01')
    return tracks[1][0].resolve(strict=True), audio[0].resolve() if audio else None, audio[3] if audio else None


def resolve_track(path):
    return resolve_sources(path)[0]


@dataclass(frozen=True)
class Entry:
    name: str
    lba: int
    size: int

    @property
    def media(self):
        return PurePosixPath(self.name).suffix in ('.XA', '.STR')

    @property
    def cdda(self):
        return PurePosixPath(self.name).suffix == '.DA'

    @property
    def sectors(self):
        return (self.size + 2047) // 2048

    @property
    def output_size(self):
        if self.cdda:
            return self.sectors * SECTOR
        return self.sectors * 2336 if self.media else self.size


class DiscImage:
    def __init__(self, path, profile=None, audio_track=None):
        self.path, self.audio_path, self.audio_index = resolve_sources(path, audio_track)
        self.profile = profile or SUPPORTED
        self.source = None
        self.audio_source = None
        self.anomalies = []

    def __enter__(self):
        self.source = self.path.open('rb')
        try:
            self.initial_stat = os.fstat(self.source.fileno())
            if self.initial_stat.st_size != self.profile.track_size:
                raise ValueError('unsupported image size: supply the supported raw MODE2/2352 '
                                 'Track 1 BIN (or its CUE), not a cooked 2048-byte ISO/CHD')
            track_sha1 = hash_stream(self.source, 'sha1')
            if track_sha1 != self.profile.track_sha1:
                raise ValueError(f'unsupported Track 1 SHA1: got {track_sha1}, expected '
                                 f'{self.profile.track_sha1}; patched or different releases are not supported')
            if self.profile.audio_size:
                if self.audio_path is None or not self.audio_path.is_file():
                    raise ValueError('complete extraction needs CD-DA Track 2; supply the complete CUE '
                                     'or --audio-track /path/to/Track2.bin')
                if self.audio_index is not None and self.audio_index != self.profile.audio_skip:
                    raise ValueError('unsupported Track 2 INDEX 01/pregap')
                self.audio_source = self.audio_path.open('rb')
                self.audio_stat = os.fstat(self.audio_source.fileno())
                if self.audio_stat.st_size != self.profile.audio_size:
                    raise ValueError(f'unsupported CD-DA Track 2 size/SHA1: got {self.audio_stat.st_size} bytes, '
                                     f'expected {self.profile.audio_size}')
                audio_sha1 = hash_stream(self.audio_source, 'sha1')
                if audio_sha1 != self.profile.audio_sha1:
                    raise ValueError(f'unsupported CD-DA Track 2 size/SHA1: got {audio_sha1}, '
                                     f'expected {self.profile.audio_sha1}')
            self.source.seek(0)
            sector = self.source.read(SECTOR)
            self.validate_sector(sector)
            pvd = read_sector_user(self.source, 16)
            if pvd[:7] != b'\x01CD001\x01' or struct.unpack_from('<H', pvd, 128)[0] != 2048:
                raise ValueError('invalid ISO9660 primary volume descriptor')
            lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
            files = walk(self.source, lba, size)
            if len(files) != self.profile.file_count:
                raise ValueError('filesystem inventory differs from supported disc')
            seen = set()
            self.entries = []
            for name, lba, size in sorted(files):
                safe_relative(name)
                folded = name.casefold()
                if folded in seen:
                    raise ValueError(f'duplicate/case-colliding ISO path: {name}')
                seen.add(folded)
                entry = Entry(name, lba, size)
                if entry.cdda:
                    if (not self.audio_source or lba != self.profile.cdda_lba or size != self.profile.cdda_size or
                            (self.profile.audio_skip + entry.sectors) * SECTOR != self.profile.audio_size):
                        raise ValueError('unsupported CD-DA filesystem reference')
                elif size < 0 or lba < 0 or (lba + entry.sectors) * SECTOR > self.profile.track_size:
                    raise ValueError(f'out-of-range file extent: {name}')
                if (entry.media or entry.cdda) and size % 2048:
                    raise ValueError(f'media extent is not sector-aligned: {name}')
                self.entries.append(entry)
            if sum(entry.output_size for entry in self.entries) > MAX_OUTPUT:
                raise ValueError('extracted data exceeds 2 GiB')
            exe = next((e for e in self.entries if e.name == 'PSX.EXE'), None)
            if exe != Entry('PSX.EXE', self.profile.exe_lba, self.profile.exe_size):
                raise ValueError('executable location/size differs from supported layout')
            data = b''.join(self.blocks(exe))
            if hashlib.sha1(data).hexdigest() != self.profile.exe_sha1:
                raise ValueError('executable SHA1 mismatch')
            if data[:8] != b'PS-X EXE' or len(data) < 0x800:
                raise ValueError('invalid PS-X EXE')
            load, length = struct.unpack_from('<II', data, 0x18)
            if not (0x80000000 <= load < load + length <= 0x80200000 and length <= len(data) - 0x800):
                raise ValueError('invalid executable load range')
            return self
        except BaseException:
            self.__exit__()
            raise

    @staticmethod
    def validate_sector(raw, require_duplicate=True):
        if (len(raw) != SECTOR or raw[:12] != SYNC or raw[15] != 2 or
                (require_duplicate and raw[16:20] != raw[20:24])):
            raise ValueError('invalid/truncated Mode2 sector or duplicated XA subheader')

    def blocks(self, entry):
        if entry.cdda:
            self.audio_source.seek(self.profile.audio_skip * SECTOR)
            for _ in range(entry.sectors):
                block = self.audio_source.read(SECTOR)
                if len(block) != SECTOR:
                    raise ValueError('truncated CD-DA audio sector')
                yield block
            return
        self.source.seek(entry.lba * SECTOR)
        remaining = entry.size
        for number in range(entry.sectors):
            raw = self.source.read(SECTOR)
            lba = entry.lba + number
            preserve_mismatch = entry.media and lba == self.profile.preserved_subheader_lba
            self.validate_sector(raw, require_duplicate=not preserve_mismatch)
            if preserve_mismatch and raw[16:20] != raw[20:24]:
                self.anomalies.append({'file': entry.name, 'lba': lba,
                                       'reason': ANOMALY_REASON})
            if entry.media:
                yield raw[16:]  # Preserve video Form1 AND audio Form2 sectors.
            else:
                if raw[18] & 0x20:
                    raise ValueError(f'unexpected Form2 data in {entry.name}')
                yield raw[24:24 + min(remaining, 2048)]
                remaining -= min(remaining, 2048)

    def unchanged(self):
        sources = [(self.source, self.path, self.initial_stat)]
        if self.audio_source:
            sources.append((self.audio_source, self.audio_path, self.audio_stat))
        keys = ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')
        for source, path, initial in sources:
            for now in (os.fstat(source.fileno()), path.stat()):
                if any(getattr(now, key) != getattr(initial, key) for key in keys):
                    raise ValueError('source image changed during extraction')

    def __exit__(self, *args):
        self.source.close()
        if self.audio_source:
            self.audio_source.close()
