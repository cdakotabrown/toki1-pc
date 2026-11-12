#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a single executable Python zipapp launcher; never package game data.

Place the resulting start_game.pyz beside a native tokimeki-pc executable.
Python 3.10+ is still required; this is not a frozen Windows game build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = {'__main__.py': ROOT / 'pc/start_game.py'}
SOURCES.update({name: ROOT / 'pc/tools' / name for name in
                ('bootstrap_assets.py', 'disc_image.py', 'build_lba_map.py', 'safe_process.py')})


def build_launcher(destination):
    """Deterministic whitelist-only bundle; refuse to replace existing output."""
    payloads = {}
    for name, source in SOURCES.items():
        if source.stat().st_size > 2 * 1024**2:
            raise ValueError(f'launcher source exceeds 2 MiB: {source}')
        payloads[name] = source.read_bytes()
    manifest = {'schema': 1, 'requires_python': '3.10', 'game_assets_included': False,
                'native_executable_included': False,
                'files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payloads.items())}}
    payloads['launcher.manifest.json'] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    destination = Path(destination).expanduser().absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        with destination.open('xb') as output:
            created = True
            output.write(b'#!/usr/bin/env python3\n')
            with zipfile.ZipFile(output, 'w') as archive:
                for name, data in sorted(payloads.items()):
                    entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                    entry.create_system = 3
                    entry.external_attr = 0o100644 << 16
                    entry.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(entry, data, compresslevel=9)
        destination.chmod(0o755)
    except BaseException:
        if created:
            destination.unlink(missing_ok=True)  # Only our newly created output.
        raise
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path, help='new start_game.pyz output path')
    args = parser.parse_args(argv)
    try:
        build_launcher(args.out)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Launcher packaging failed: {error}\n')
    print(f'Launcher created: {args.out} (code only; no native executable or game assets)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
