# SPDX-License-Identifier: GPL-3.0-or-later
"""The SPU core the port links (pc/3rd/SPU-CORE.md): where the fetched
sources and the built archive are, for the audio unit tests.

  CORE      the fetched core (SPU_CORE_DIR, default pc/3rd/spu-core): pristine/,
            upstream/ (patched), .fetched, spu_psx.a
  GLUE      pc/3rd/spu-glue: stubs/, patches/, Makefile, fetch.sh
  archive() the native spu_psx.a, built by pc/3rd/spu-glue/Makefile if stale
  build(d)  build a copy of the core in directory d (d/upstream, d/.fetched)
            into d/spu_psx.a
  copy(d)   copy the fetched core to d (no build output), for a mutated build
"""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
GLUE = ROOT / 'pc/3rd/spu-glue'
CORE = Path(os.environ.get('SPU_CORE_DIR', ROOT / 'pc/3rd/spu-core'))
if not CORE.is_absolute():
    CORE = ROOT / CORE
PATCHES = sorted((GLUE / 'patches').glob('*.patch'))
MISSING = (f'the SPU core is not fetched in {CORE}: run pc/3rd/spu-glue/fetch.sh '
           '(pc/3rd/SPU-CORE.md)')


class SpuCoreMissing(RuntimeError):
    pass


def require():
    if not (CORE / '.fetched').is_file() or not (CORE / 'upstream/spu.cpp').is_file():
        raise SpuCoreMissing(MISSING)


def build(core, jobs=8, timeout=600):
    """make -C pc/3rd/spu-glue SPU_CORE_DIR=core; returns core/spu_psx.a"""
    core = Path(core).resolve()
    r = subprocess.run(['make', '--no-print-directory', '-C', str(GLUE), f'-j{jobs}',
                        f'SPU_CORE_DIR={core}'],
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f'SPU core build failed in {core}:\n{r.stdout}{r.stderr}')
    return core / 'spu_psx.a'


def archive():
    require()
    return build(CORE)


def copy(dest):
    """the fetched core (pristine/, upstream/, the stamp) at dest, unbuilt"""
    require()
    shutil.copytree(CORE, dest, ignore=shutil.ignore_patterns('obj', '*.o', '*.a'))
    return Path(dest)


def patched(dest, upto):
    """a core at dest whose upstream/ is pristine/ plus the first `upto`
    patches of the series (e.g. 2 = before 0003-sound-quality)"""
    dest = copy(dest)
    shutil.rmtree(dest / 'upstream')
    shutil.copytree(dest / 'pristine', dest / 'upstream')
    for p in PATCHES[:upto]:
        subprocess.run(['patch', '-p1', '-s', '-d', str(dest), '-i', str(p)], check=True)
    for f in (dest / 'upstream').glob('*.orig'):
        f.unlink()
    return dest
