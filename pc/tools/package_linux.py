#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""package_linux.py -- the player's Linux tar.gz, built from a whitelist.

    python3 pc/tools/package_linux.py [--out build/tokimeki-pc-linux-x86_64.tar.gz]

Contents:
    tokimeki-pc/tokimeki-pc         ONE program (make -f pc/Makefile
                                    linux-package, built in the glibc 2.31
                                    root of setup_linux_pkg_root.sh): SDL2,
                                    libstdc++ and libgcc linked in; it needs
                                    glibc 2.31 or newer only (SDL loads the
                                    desktop's X11 / Wayland / PulseAudio /
                                    ALSA libraries itself) and keeps its data
                                    in its own folder (portable)
    tokimeki-pc/README.txt          the player's instructions (pc/package/)
and nothing else: the licence texts are inside the program (--licenses;
the settings menu's About / licences).  Next to the tar.gz: <out stem>-source.tar.gz, the GPL corresponding source
(pc/tools/source_archive.py).  Publish both together.
No launcher script is needed: nothing is bundled beside the binary.  The
binary's DT_NEEDED list is checked (glibc libraries only), so are the
symbol versions it asks of them (none newer than GLIBC_FLOOR: a binary
linked on a new distribution is refused), and the archive (no game data).
Deterministic: fixed owner, mode and mtime.
"""
import argparse
import gzip
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_windows import (ROOT, TOP, LICENSES, check_english, check_licences,  # noqa: E402
                             check_no_game_data, check_retail, common_members)
import source_archive  # noqa: E402

GLIBC_LIBS = {'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1',
              'ld-linux-x86-64.so.2'}
# The newest glibc the package may ask for: 2.31 = Ubuntu 20.04, Debian 11,
# and everything after them.
GLIBC_FLOOR = (2, 31)


def needed(path):
    out = subprocess.run(['objdump', '-p', str(path)], capture_output=True, text=True, check=True).stdout
    return sorted(line.split()[1] for line in out.splitlines() if line.strip().startswith('NEEDED'))


def check_needed(path):
    bad = [n for n in needed(path) if n not in GLIBC_LIBS]
    if bad:
        raise ValueError(f'{path} links shared libraries beyond glibc: {", ".join(bad)} '
                         '(build it with make -f pc/Makefile linux-package)')


def glibc_requirements(dynsym):
    """{symbol version: [symbols]} from `objdump -T` text, for every GLIBC_x.y
    (and GLIBCXX / CXXABI, which a static libstdc++ never leaves)."""
    req = {}
    for line in dynsym.splitlines():
        f = line.split()
        for i, w in enumerate(f):
            ver = w.strip('()')
            if ver.startswith(('GLIBC_', 'GLIBCXX_', 'CXXABI_')) and ver[ver.index('_') + 1:][:1].isdigit():
                req.setdefault(ver, []).append(f[-1] if i < len(f) - 1 else '?')
                break
    return req


def version_tuple(ver):
    return tuple(int(x) for x in ver.split('_', 1)[1].split('.'))


def check_glibc(path, floor=GLIBC_FLOOR):
    """Refuse a binary that asks for a glibc newer than `floor`, or for libstdc++."""
    out = subprocess.run(['objdump', '-T', str(path)], capture_output=True, text=True, check=True).stdout
    req = glibc_requirements(out)
    bad = sorted((v for v in req if not v.startswith('GLIBC_') or version_tuple(v) > floor),
                 key=lambda v: (not v.startswith('GLIBC_'), version_tuple(v)))
    if bad:
        detail = '; '.join(f'{v}: {", ".join(sorted(set(req[v]))[:4])}' for v in bad)
        raise ValueError(f'{path} needs a newer glibc than {".".join(map(str, floor))} ({detail}); '
                         'build it in the old root: pc/tools/setup_linux_pkg_root.sh, '
                         'make -f pc/Makefile linux-package')
    glibc = [v for v in req if v.startswith('GLIBC_')]
    return max(glibc, key=version_tuple) if glibc else None


def build(out, binary=None, force=False, source=True, allow_dirty=False):
    binary = Path(binary or ROOT / 'pc/build-linux-pkg/tokimeki-pc')
    if not binary.is_file():
        raise FileNotFoundError('build the package binary first (pc/tools/setup_linux_pkg_root.sh; '
                                'make -f pc/Makefile linux-package): ' + str(binary))
    check_needed(binary)
    check_glibc(binary)
    check_retail(binary)
    check_english(binary)
    # the MinGW runtime notice is the Windows build's only (still embedded,
    # but not required of a Linux binary)
    check_licences(binary, [n for n in LICENSES if n != 'MinGW-runtime.txt'])
    out = Path(out).expanduser().absolute()
    if out.exists() and not force:
        raise FileExistsError(f'{out} exists (pass --force to replace it)')
    if source:
        source_archive.check_ready(allow_dirty)
    out.parent.mkdir(parents=True, exist_ok=True)
    files = [(f'{TOP}/tokimeki-pc', binary.read_bytes())]
    files += common_members(crlf=False)
    check_no_game_data([n for n, _ in files])
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.PAX_FORMAT) as tf:
        for d in sorted({os.path.dirname(n) for n, _ in files}):
            info = tarfile.TarInfo(d)
            info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, 0
            tf.addfile(info)
        for name, data in files:
            info = tarfile.TarInfo(name)
            info.size, info.mtime = len(data), 0
            info.mode = 0o755 if name == f'{TOP}/tokimeki-pc' else 0o644
            info.uname = info.gname = ''
            tf.addfile(info, io.BytesIO(data))
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        partial = Path(tmp) / 'out.tar.gz'
        with open(partial, 'wb') as fh, gzip.GzipFile(fileobj=fh, mode='wb', mtime=0, filename='') as gz:
            gz.write(raw.getvalue())
        os.replace(partial, out)
    with tarfile.open(out) as tf:
        check_no_game_data(tf.getnames())
    if source:
        source_archive.build(source_archive.source_path(out), force, allow_dirty)
    return out, [n for n, _ in files]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=str(ROOT / 'build/tokimeki-pc-linux-x86_64.tar.gz'))
    ap.add_argument('--binary', help='default pc/build-linux-pkg/tokimeki-pc')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--allow-dirty', action='store_true',
                    help='make the source archive although tracked files have uncommitted changes '
                         '(local tests only: such a package must not be published)')
    a = ap.parse_args(argv)
    try:
        out, names = build(a.out, a.binary, a.force, allow_dirty=a.allow_dirty)
    except (OSError, ValueError) as e:
        print(f'package_linux: {e}')
        return 1
    print(f'package_linux: {out} ({out.stat().st_size // 1024} KiB)')
    for name in names:
        print(f'  {name}')
    src = source_archive.source_path(out)
    print(f'package_linux: source {src} ({src.stat().st_size // 1024} KiB) -- publish it with the package')
    return 0


if __name__ == '__main__':
    sys.exit(main())
