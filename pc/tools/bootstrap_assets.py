#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare verified native runtime assets from the user's own supported disc.

No exporters, guessed palettes, Java, dumpsxiso, or compiler are needed.
Existing unmanaged/damaged directories are never replaced or deleted.
"""
import argparse
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile

from disc_image import ANOMALY_REASON, DiscImage, MAX_OUTPUT, SUPPORTED, hash_file, safe_relative

SCHEMA = 1
MANIFEST = 'assets.manifest.json'
CATEGORIES = {'.EXN': 'overlays', '.NBN': 'nbn', '.VH': 'music', '.VB': 'music',
              '.SEQ': 'music', '.XA': 'audio', '.DA': 'audio', '.STR': 'video', '.TIM': 'textures'}


def alias_for(name):
    path = PurePosixPath(name)
    if name == 'PSX.EXE':
        return 'data/PSX.EXE'
    category = CATEGORIES.get(path.suffix)
    return f'{category}/{path.name}' if category else None


def checked_path(root, name):
    safe_relative(name)
    path = root
    for part in name.split('/'):
        path = path / part
        if path.is_symlink():
            raise ValueError(f'asset path must not contain symlinks: {name}')
    return path


def verify_cache(destination, profile=None):
    profile = profile or SUPPORTED
    root = Path(destination)
    if root.is_symlink() or not root.is_dir():
        raise ValueError('asset directory is missing or is a symlink')
    manifest_path = checked_path(root, MANIFEST)
    if not manifest_path.is_file() or manifest_path.stat().st_size > 4 * 1024**2:
        raise ValueError('asset completion manifest is missing or exceeds 4 MiB')
    data = json.loads(manifest_path.read_text(encoding='utf-8'))
    if (not isinstance(data, dict) or data.get('schema') != SCHEMA or data.get('complete') is not True or
            data.get('source') != asdict(profile)):
        raise ValueError('asset manifest has unsupported schema or disc identity')
    rows = data.get('files')
    if not isinstance(rows, list) or not 0 < len(rows) <= 10000:
        raise ValueError('invalid asset manifest file list')
    names = set()
    total = 0
    for row in rows:
        name = row['path']
        safe_relative(name)
        size = row['size']
        digest = row['sha256']
        if (name.casefold() in names or type(size) is not int or size < 0 or
                not isinstance(digest, str) or len(digest) != 64 or
                any(char not in '0123456789abcdef' for char in digest)):
            raise ValueError('duplicate or invalid asset manifest entry')
        names.add(name.casefold())
        total += size
    if total > MAX_OUTPUT:
        raise ValueError('asset cache exceeds 2 GiB verification budget')
    raw = {row['path'][4:]: row for row in rows if row['path'].startswith('raw/')}
    if len(raw) != profile.file_count or 'PSX.EXE' not in raw:
        raise ValueError('incomplete raw filesystem inventory')
    anomalies = data.get('preserved_source_anomalies')
    expected_anomalies = int(profile.preserved_subheader_lba >= 0)
    if not isinstance(anomalies, list) or len(anomalies) != expected_anomalies:
        raise ValueError('missing or inconsistent source anomaly record')
    if expected_anomalies:
        anomaly = anomalies[0]
        if (not isinstance(anomaly, dict) or anomaly.get('file') not in raw or
                anomaly.get('lba') != profile.preserved_subheader_lba or
                anomaly.get('reason') != ANOMALY_REASON):
            raise ValueError('invalid source anomaly record')
    by_name = {row['path']: row for row in rows}
    expected_paths = {f'raw/{name}' for name in raw}
    for name, row in raw.items():
        alias = alias_for(name)
        if alias:
            expected_paths.add(alias)
            if alias not in by_name or any(by_name[alias][key] != row[key] for key in ('size', 'sha256')):
                raise ValueError(f'missing or inconsistent organized asset: {alias}')
    if expected_paths != set(by_name):
        raise ValueError('unexpected managed files in asset manifest')
    for row in rows:
        path = checked_path(root, row['path'])
        if (not path.is_file() or path.stat().st_size != row['size'] or
                hash_file(path) != row['sha256']):
            raise ValueError(f'missing or corrupt asset: {row["path"]}')
    if hash_file(root / 'raw/PSX.EXE', 'sha1') != profile.exe_sha1:
        raise ValueError('cached executable differs from supported retail executable')
    return data


def link_or_copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def extract_to_stage(image, stage):
    rows = []
    names = set()
    for entry in image.entries:
        name = f'raw/{entry.name}'
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        written = 0
        with target.open('xb') as output:
            for block in image.blocks(entry):
                output.write(block)
                digest.update(block)
                written += len(block)
        if written != entry.output_size:
            raise ValueError(f'incomplete extracted file: {entry.name}')
        row = {'path': name, 'size': written, 'sha256': digest.hexdigest()}
        rows.append(row)
        alias = alias_for(entry.name)
        if alias:
            if alias.casefold() in names:
                raise ValueError(f'cannot flatten ambiguous asset basename: {alias}')
            names.add(alias.casefold())
            link_or_copy(target, stage / alias)
            rows.append(dict(row, path=alias))
    image.unchanged()
    result = {'schema': SCHEMA, 'complete': True, 'source': asdict(image.profile),
              'preserved_source_anomalies': image.anomalies,
              'files': sorted(rows, key=lambda row: row['path'])}
    (stage / MANIFEST).write_text(json.dumps(result, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    # Verify actual copied output too, including the portable no-hard-link path.
    verify_cache(stage, image.profile)
    return result


def prepare_assets(image_path, destination, profile=None, audio_track=None):
    profile = profile or SUPPORTED
    if audio_track is not None and image_path is None:
        raise ValueError('--audio-track requires a supplied --disc')
    destination = Path(destination).expanduser().absolute()
    if destination.exists() or destination.is_symlink():
        try:
            result = verify_cache(destination, profile)
        except (ValueError, KeyError, TypeError, OSError) as error:
            raise ValueError(f'{destination}: invalid/unmanaged assets ({error}); '
                             'move this folder aside or choose a different game directory; '
                             'existing files were NOT changed') from error
        if image_path is not None:
            with DiscImage(image_path, profile, audio_track):
                pass  # A supplied image must pass the gate even on a cache hit.
        return result
    if image_path is None:
        raise ValueError('first run requires --disc /path/to/Track1.bin (or .cue); '
                         'you must own a legal copy of SLPS-00065')
    destination.parent.mkdir(parents=True, exist_ok=True)
    lock = destination.parent / f'.{destination.name}.prepare.lock'
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise ValueError(f'asset setup lock exists: {lock}; another setup may be running. '
                         'If interrupted, inspect it before removing the stale lock.') from error
    try:
        (lock / 'owner.json').write_text(json.dumps({'pid': os.getpid()}), encoding='utf-8')
        with DiscImage(image_path, profile, audio_track) as image:
            needed = sum(e.output_size * (2 if alias_for(e.name) else 1) for e in image.entries)
            if needed > MAX_OUTPUT or shutil.disk_usage(destination.parent).free < needed + 16 * 1024**2:
                raise ValueError('not enough free disk space for complete extraction and portable copies')
            with tempfile.TemporaryDirectory(prefix=f'.{destination.name}.stage-', dir=destination.parent) as temporary:
                stage = Path(temporary) / 'assets'
                stage.mkdir()
                result = extract_to_stage(image, stage)
                image.unchanged()
                if destination.exists() or destination.is_symlink():
                    raise ValueError('asset destination appeared during setup; refusing to replace it')
                stage.rename(destination)
                return result
    finally:
        owner = lock / 'owner.json'
        if owner.exists():
            owner.unlink()
        lock.rmdir()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', nargs='?', help='supported raw Track 1 BIN or complete CUE')
    parser.add_argument('legacy_out', nargs='?', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--out', type=Path, help='new asset directory (existing valid cache is reused)')
    parser.add_argument('--audio-track', type=Path, help='Track 2 BIN when not using a complete CUE')
    args = parser.parse_args(argv)
    if args.out is not None and args.legacy_out is not None:
        parser.error('specify an output directory once')
    args.out = args.out if args.out is not None else args.legacy_out
    if args.out is None:
        parser.error('--out is required')
    try:
        result = prepare_assets(args.disc, args.out, audio_track=args.audio_track)
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f'Asset setup failed: {error}\n')
    print(f'Assets verified: {args.out} ({len(result["files"])} managed files)')
    if result.get('preserved_source_anomalies'):
        print('Warning: source XA subheader anomaly preserved byte-for-byte; see assets.manifest.json.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
