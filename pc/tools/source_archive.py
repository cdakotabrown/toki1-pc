#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""source_archive.py -- the GPL corresponding source for a player package.

    python3 pc/tools/source_archive.py --out build/tokimeki-pc-source.tar.gz
        [--allow-dirty] [--force]

The PC port binary is GPL-3.0 (pc/LICENSE; it links the DuckStation SPU
core), so every binary release must travel with its corresponding source
(GPLv3 section 6).  The packagers (package_windows.py, package_linux.py)
call build() and write <package>-source.tar.gz next to the package; this
script is the same step on its own.

The archive holds, under one top directory named after the archive:
    the committed tree at HEAD, exactly as `git archive HEAD` exports it;
      when the port is built from separate checkouts (toki1-pc with a
      sibling toki1-decomp and, when present, toki1-english:
      pc/tools/pc_paths.py), every one of them,
      each at its HEAD, side by side under <top>/toki1-pc/, <top>/toki1-decomp/,
      <top>/toki1-english/ -- the binary compiles the decompiled C of
      toki1-decomp, so that is part of its source;
    pc/3rd/spu-core/pristine/*   (in the port's tree) the six DuckStation files, byte for byte
                                 (checked against pc/3rd/spu-glue/SHA256SUMS)
    pc/3rd/spu-core/upstream/*   the same with this project's patches applied
                                 (what the binary was compiled from)
    SOURCE.txt                   the commit, the SPU core pin, how to build
The SPU core is not in the repository (pc/3rd/SPU-CORE.md), which is why it
is added here: the archive builds without network access.

A tree (any of them) with uncommitted changes to tracked files is refused
(the archive would not be the source of the binary just built); --allow-dirty overrides
that for local testing only.  Deterministic for a given commit: member
times are the commit time, owners are empty, gzip carries no name or time.
"""
import argparse
import gzip
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP, ENGLISH  # noqa: E402
GLUE = ROOT / 'pc/3rd/spu-glue'
SPU_CORE_MD = ROOT / 'pc/3rd/SPU-CORE.md'


def core_dir():
    return Path(os.environ.get('SPU_CORE_DIR') or ROOT / 'pc/3rd/spu-core')


def source_path(package_out):
    """build/x.zip -> build/x-source.tar.gz; build/x.tar.gz -> build/x-source.tar.gz"""
    p = Path(package_out)
    name = p.name
    for suffix in ('.tar.gz', '.tgz', '.zip'):
        if name.endswith(suffix):
            name = name[:-len(suffix)]
            break
    return p.with_name(name + '-source.tar.gz')


def git(*args, root=ROOT):
    return subprocess.run(['git', '-C', str(root), *args], capture_output=True, check=True).stdout


def parts():
    """[(name, checkout root, directory under the archive's top)]: the
    checkouts the binary is built from.  One tree in the monorepo (every
    part is this checkout); toki1-pc, toki1-decomp and toki1-english side by
    side otherwise."""
    roots = [('toki1-pc', ROOT), ('toki1-decomp', DECOMP)]
    if ENGLISH is not None:
        roots.append(('toki1-english', ENGLISH))
    out, seen = [], set()
    for name, root in roots:
        real = os.path.realpath(root)
        if real not in seen:
            seen.add(real)
            out.append((name, Path(root)))
    if len(out) == 1:
        return [(out[0][0], out[0][1], '')]
    return [(name, root, name + '/') for name, root in out]


def head_commit(root=ROOT):
    return git('rev-parse', 'HEAD', root=root).decode().strip()


def dirty_paths():
    dirty = []
    for name, root, sub in parts():
        out = git('status', '--porcelain', '--untracked-files=no', root=root).decode('utf-8', 'replace')
        dirty += [sub + line[3:] for line in out.splitlines() if line.strip()]
    return dirty


def spu_pin():
    for line in SPU_CORE_MD.read_text(encoding='utf-8').splitlines():
        if line.startswith('SPU_CORE_COMMIT='):
            return line.split('=', 1)[1].strip()
    raise ValueError(f'no SPU_CORE_COMMIT= line in {SPU_CORE_MD}')


def spu_files():
    """[(archive path under pc/3rd/spu-core/, bytes)] for pristine/ and
    upstream/; refuses a core that is missing, unverified or unpatched."""
    core = core_dir()
    if not (core / '.fetched').is_file():
        raise ValueError(f'the SPU core is not fetched in {core}: run pc/3rd/spu-glue/fetch.sh '
                         '(pc/3rd/SPU-CORE.md) before packaging')
    sums = {}
    for line in (GLUE / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
        if line.strip():
            digest, name = line.split(None, 1)
            sums[name.strip().lstrip('*')] = digest
    out = []
    for name, digest in sorted(sums.items()):
        pristine = core / 'pristine' / name
        upstream = core / 'upstream' / name
        if not pristine.is_file() or not upstream.is_file():
            raise ValueError(f'the SPU core in {core} lacks {name}: run pc/3rd/spu-glue/fetch.sh')
        data = pristine.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{pristine} does not match pc/3rd/spu-glue/SHA256SUMS: run fetch.sh')
        out.append((f'pristine/{name}', data))
        out.append((f'upstream/{name}', upstream.read_bytes()))
    stamp = (core / '.fetched').stat().st_mtime
    for p in sorted((GLUE / 'patches').glob('*.patch')):
        if p.stat().st_mtime > stamp:
            raise ValueError(f'{p.name} is newer than the fetched SPU core: run pc/3rd/spu-glue/fetch.sh')
    return out


def source_txt(commit, pin, dirty, commits=None):
    lines = [
        'Corresponding source for the Heartthrob Memorial PC Enhanced / tokimeki-pc',
        'binary published with this archive (GPL-3.0; see pc/LICENSE and',
        'pc/licenses/THIRD-PARTY.txt).',
        '',
    ]
    if commits and len(commits) > 1:
        lines += [f'{name + " commit:":21s}{c}' for name, c in commits] + [
            '                    (each tree is in the directory of that name; build',
            '                    in toki1-pc/, which finds the others beside it)',
        ]
    else:
        lines += [f'Repository commit:  {commit}']
    lines += [
        f'DuckStation SPU:    https://github.com/stenzek/duckstation commit {pin}',
        '                    (C) 2019-2024 Connor McLaughlin, GPL-3.0; the six files',
        '                    are in pc/3rd/spu-core/pristine/, and with this project\'s',
        '                    patches (pc/3rd/spu-glue/patches/) applied in',
        '                    pc/3rd/spu-core/upstream/.  pc/3rd/spu-core/.fetched marks',
        '                    the core as fetched, so the build needs no network.',
        '',
        'Build (see pc/README.md):',
        '    make -f pc/Makefile all',
        'The English translation pack embedded in release binaries is built from',
        'translation/ with pc/tools/translation_pack.py build.',
        '',
        'No game data is included; the program reads it from your own disc.',
    ]
    if dirty:
        lines += ['', 'WARNING: made with --allow-dirty; the working tree had uncommitted',
                  'changes to: ' + ', '.join(dirty[:20]) + (' ...' if len(dirty) > 20 else '')]
    return ('\n'.join(lines) + '\n').encode('utf-8')


def check_ready(allow_dirty=False):
    """Raise ValueError unless an archive can be made now (call before a
    long package build, so the package step fails early)."""
    dirty = dirty_paths()
    if dirty and not allow_dirty:
        raise ValueError('the working tree has uncommitted changes to tracked files ('
                         + ', '.join(dirty[:5]) + (' ...' if len(dirty) > 5 else '')
                         + '): commit them first, so the source archive is the source of the '
                         'binary (or pass --allow-dirty for a local test)')
    spu_files()
    return dirty


def build(out, force=False, allow_dirty=False):
    """Write the source archive to `out`; return (path, info dict)."""
    dirty = check_ready(allow_dirty)
    out = Path(out).expanduser().absolute()
    if out.exists() and not force:
        raise FileExistsError(f'{out} exists (pass --force to replace it)')
    out.parent.mkdir(parents=True, exist_ok=True)
    trees = parts()
    commits = [(name, head_commit(root)) for name, root, _sub in trees]
    commit = commits[0][1]
    mtime = int(git('show', '-s', '--format=%ct', 'HEAD').decode().strip())
    top = out.name[:-len('.tar.gz')] if out.name.endswith('.tar.gz') else out.stem
    pc_sub = trees[0][2]
    extra = [(f'{pc_sub}pc/3rd/spu-core/{n}', d) for n, d in spu_files()]
    extra.append((f'{pc_sub}pc/3rd/spu-core/.fetched', f'source archive of {commit}\n'.encode()))
    extra.append(('SOURCE.txt', source_txt(commit, spu_pin(), dirty, commits)))
    count = 0
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        partial = Path(tmp) / 'out.tar.gz'
        with open(partial, 'wb') as fh, \
                gzip.GzipFile(fileobj=fh, mode='wb', mtime=0, filename='', compresslevel=9) as gz, \
                tarfile.open(fileobj=gz, mode='w|', format=tarfile.PAX_FORMAT) as dst:
            for (_name, root, sub), (_n, c) in zip(trees, commits):
                if sub:
                    info = tarfile.TarInfo(f'{top}/{sub.rstrip("/")}')
                    info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, mtime
                    dst.addfile(info)
                proc = subprocess.Popen(['git', '-C', str(root), 'archive', '--format=tar',
                                         f'--prefix={top}/{sub}', c], stdout=subprocess.PIPE)
                with tarfile.open(fileobj=proc.stdout, mode='r|') as src:
                    for m in src:
                        if m.type == tarfile.XGLTYPE or m.name == 'pax_global_header':
                            continue
                        m.uname = m.gname = ''
                        m.uid = m.gid = 0
                        dst.addfile(m, src.extractfile(m) if m.isfile() else None)
                        count += 1
                if proc.wait() != 0:
                    raise ValueError(f'git archive failed in {root}')
            dirs = set()
            for name, _ in extra:
                parts_ = name.split('/')[:-1]
                for i in range(1, len(parts_) + 1):
                    dirs.add('/'.join(parts_[:i]))
            for d in sorted(dirs):
                if d in (pc_sub + 'pc', pc_sub + 'pc/3rd', pc_sub.rstrip('/')):
                    continue            # already in the git tree
                info = tarfile.TarInfo(f'{top}/{d}')
                info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, mtime
                dst.addfile(info)
            for name, data in extra:
                info = tarfile.TarInfo(f'{top}/{name}')
                info.size, info.mode, info.mtime = len(data), 0o644, mtime
                dst.addfile(info, io.BytesIO(data))
                count += 1
        os.replace(partial, out)
    return out, {'commit': commit, 'spu_core': spu_pin(), 'files': count, 'top': top,
                 'dirty': dirty, 'commits': commits}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=str(ROOT / 'build/tokimeki-pc-source.tar.gz'))
    ap.add_argument('--allow-dirty', action='store_true',
                    help='archive HEAD although tracked files have uncommitted changes (testing only)')
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args(argv)
    try:
        out, info = build(a.out, a.force, a.allow_dirty)
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f'source_archive: {e}')
        return 1
    print(f'source_archive: {out} ({out.stat().st_size // 1024} KiB, {info["files"]} files, '
          f'commit {info["commit"][:12]}, SPU core {info["spu_core"][:9]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
