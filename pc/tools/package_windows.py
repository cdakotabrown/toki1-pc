#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""package_windows.py -- the player's Windows zip, built from a whitelist.

    python3 pc/tools/package_windows.py [--out build/tokimeki-pc-windows.zip]

Contents (docs/pc-port/WINDOWS-BUILD.md "Package", PLAYER-GUIDE.md section 1):
    tokimeki-pc/Heartthrob Memorial PC Enhanced.exe   (PLAYER_EXE)
                                    the game: ONE self-contained program
                                    (make -f pc/Makefile windows; SDL2 and the
                                    MinGW runtime are linked in, the first-run
                                    setup is built in -- no Python, no DLLs)
    tokimeki-pc/README.txt          the player's instructions (pc/package/)
and nothing else (2026-11-07): the licence texts are inside the exe
(pc/hal/licenses.c: Settings > About / licences, or --licenses), and the
first run makes the rest (disk/ is the player's; data/, saves/ and
settings.ini are created).  Next to the zip it writes the GPL corresponding source,
<out stem>-source.tar.gz (pc/tools/source_archive.py: the committed tree
plus the fetched, patched DuckStation SPU core).  Publish both together.  A
tree with uncommitted changes is refused (--allow-dirty: local tests only).
The player adds disk/ with their own disc image; everything else is created
on the first run.  Never game data: the whitelist names every member, and
the zip is checked afterwards for anything that looks like an asset (a disc
image, an extracted file, a memory card, a save state, a DLL).
The exe's import table is checked first (only DLLs every Windows 10/11 has;
pc/tools/test_windows_imports.py).  Deterministic: fixed timestamps, order.
Refuses an existing --out or source archive (pass --force to replace them).
"""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_archive  # noqa: E402
from pc_paths import DECOMP, ENGLISH  # noqa: E402

# The player's name for pc/build-win/tokimeki-pc.exe; the exe's own version
# block says the same (pc/win/tokimeki-pc.rc OriginalFilename).  The
# launcher (pc/start_game.py) looks for it first.
PLAYER_EXE = 'Heartthrob Memorial PC Enhanced.exe'

ROOT = Path(__file__).resolve().parents[2]
TOP = 'tokimeki-pc'
FORBIDDEN_SUFFIXES = ('.bin', '.cue', '.iso', '.img', '.chd', '.pbp', '.ecm', '.mcr', '.mcs',
                      '.tks', '.exn', '.str', '.xa', '.da', '.vab', '.vh', '.vb', '.seq', '.tim',
                      '.tmd', '.nbn', '.wav', '.png', '.sf2', '.dll', '.so', '.pyz', '.bat')
FORBIDDEN_PARTS = ('/assets/', '/pc/assets', '/data/', '/saves/', '/disk/', '/raw/', 'psx.exe')
# The licence texts the exe carries (pc/hal/licenses.c), checked before
# packaging: pc/licenses/*.txt and the project's own LICENSE.
LICENSES = ('THIRD-PARTY.txt', 'SDL2.txt', 'GPL-3.0.txt', 'TinySoundFont.txt', 'stb.txt',
            'dr_libs.txt', 'MinGW-runtime.txt')
# Every DLL the exe may import: part of every Windows 10/11 install.
SYSTEM_DLLS = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'winmm.dll', 'imm32.dll', 'ole32.dll',
    'oleaut32.dll', 'shell32.dll', 'setupapi.dll', 'version.dll', 'advapi32.dll',
    'comdlg32.dll', 'msvcrt.dll', 'cfgmgr32.dll', 'dinput8.dll', 'hid.dll', 'ws2_32.dll',
    'shlwapi.dll', 'dwmapi.dll', 'uxtheme.dll', 'xinput1_4.dll', 'dxgi.dll', 'd3d11.dll',
    'd3d9.dll', 'opengl32.dll', 'bcrypt.dll', 'ntdll.dll', 'comctl32.dll', 'rpcrt4.dll',
}


def objdump():
    for cand in (ROOT / 'build/mingw-root/usr/bin/x86_64-w64-mingw32-objdump',):
        if cand.is_file():
            return str(cand)
    return 'x86_64-w64-mingw32-objdump'


def pe_imports(path):
    """The DLL names in a PE file's import table (objdump -p)."""
    import subprocess
    out = subprocess.run([objdump(), '-p', str(path)], capture_output=True, text=True, check=True).stdout
    return sorted({line.split('DLL Name:', 1)[1].strip() for line in out.splitlines() if 'DLL Name:' in line})


def check_imports(path):
    """Raise unless every imported DLL is a system DLL (or an api-ms-win-* set)."""
    bad = [d for d in pe_imports(path)
           if d.lower() not in SYSTEM_DLLS and not d.lower().startswith(('api-ms-win-', 'ext-ms-win-'))]
    if bad:
        raise ValueError(f'{path} imports DLLs that are not part of Windows: {", ".join(bad)}')


def check_retail(binary):
    """No disc bytes in the program (pc/tools/audit_retail_bytes.py), when an
    extracted disc is at hand to compare with; refuses the package otherwise."""
    import audit_retail_bytes
    assets = ROOT / 'pc/assets'
    if not (assets / 'raw/PSX.EXE').is_file():
        print(f'package: retail-bytes audit skipped (no extracted disc at {assets})')
        return
    status, _ = audit_retail_bytes.run_audit([str(binary)], str(assets), show=10,
                                             allow_path=str(ROOT / 'pc/tools/retail_bytes_allow.txt'))
    if status != 0:
        raise ValueError(f'{binary} carries retail bytes (audit_retail_bytes.py); not packaging it')


def check_english(binary):
    """English is the player default (settings.c, 2026-11-05): a package
    whose binary carries no translation pack would start in Japanese with an
    error line.  The pack is embedded only when pc/build/translation/en.tktp
    exists at link time (pc/Makefile TKTP=; docs/pc-port/TRANSLATION-ON-PC.md).
    The requirement holds when the English patch is part of the build
    (toki1-english present, pc/tools/pc_paths.py); without it the build is the
    Japanese game (docs/REPO-SPLIT-PLAN.md), which this says out loud."""
    if ENGLISH is None:
        print('package: toki1-english not found (../toki1-english, TOKI_ENGLISH_DIR): '
              'packaging the Japanese-only build, no English translation pack required')
        return
    if b'TKTP\x01\x00\x00\x00' not in Path(binary).read_bytes():
        raise ValueError(f'{binary} has no English translation pack embedded; build it '
                         '(pc/tools/translation_pack.py build) and relink before packaging')


def windows_inputs():
    exe = ROOT / 'pc/build-win/tokimeki-pc.exe'
    if not exe.is_file():
        raise FileNotFoundError('build the Windows port first (pc/tools/setup_windows_toolchain.sh; '
                                'make -f pc/Makefile windows): ' + str(exe))
    return exe


def text_crlf(path, crlf):
    data = Path(path).read_text(encoding='utf-8')
    return (data.replace('\n', '\r\n') if crlf else data).encode('utf-8')


def common_members(crlf):
    """The README, shared with package_linux.py: the only file beside the
    program."""
    return [(f'{TOP}/README.txt', text_crlf(ROOT / 'pc/package/README.txt', crlf))]


def licence_texts(names=LICENSES):
    """The licence texts the program must carry: pc/licenses/<names> and
    the project's own MIT LICENSE (the shipped exe is GPL-3.0 as a combined
    work, the sources it was built from are MIT): toki1-decomp's, the
    checkout that holds the decompiled C (pc/hal/licenses.c)."""
    return [(ROOT / 'pc/licenses' / n).read_text(encoding='utf-8') for n in names] + \
        [(DECOMP / 'LICENSE').read_text(encoding='utf-8')]


def check_licences(binary, names=LICENSES):
    """The download carries no licence files, so the program must: every
    text, whole, in the binary (pc/hal/licenses.c .incbin)."""
    data = Path(binary).read_bytes()
    missing = [t.splitlines()[0] for t in licence_texts(names) if t.encode('utf-8') not in data]
    if missing:
        raise ValueError(f'{binary} does not carry every licence text (missing: {"; ".join(missing)}); '
                         'rebuild it (pc/hal/licenses.c)')


def check_no_game_data(names):
    bad = [n for n in names
           if any(part in '/' + n.lower() for part in FORBIDDEN_PARTS) or n.lower().endswith(FORBIDDEN_SUFFIXES)]
    if bad:
        raise ValueError('refusing to package game data, saves or libraries: ' + ', '.join(bad))


def build(out, force=False, source=True, allow_dirty=False):
    """Write the zip to `out` and, unless source=False, its source archive
    (source_archive.source_path(out)); return (zip path, member names)."""
    exe = windows_inputs()
    check_imports(exe)
    check_retail(exe)
    check_english(exe)
    check_licences(exe)
    out = Path(out).expanduser().absolute()
    if out.exists() and not force:
        raise FileExistsError(f'{out} exists (pass --force to replace it)')
    if source:
        source_archive.check_ready(allow_dirty)
    out.parent.mkdir(parents=True, exist_ok=True)
    files = [(f'{TOP}/{PLAYER_EXE}', exe.read_bytes())] + common_members(crlf=True)
    check_no_game_data([n for n, _ in files])
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        partial = Path(tmp) / 'out.zip'
        with zipfile.ZipFile(partial, 'w') as zf:
            for name, data in files:
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 0
                info.compress_type = zipfile.ZIP_DEFLATED
                zf.writestr(info, data, compresslevel=9)
        os.replace(partial, out)
    with zipfile.ZipFile(out) as zf:
        check_no_game_data(zf.namelist())
    if source:
        source_archive.build(source_archive.source_path(out), force, allow_dirty)
    return out, [n for n, _ in files]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=str(ROOT / 'build/tokimeki-pc-windows.zip'))
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--allow-dirty', action='store_true',
                    help='make the source archive although tracked files have uncommitted changes '
                         '(local tests only: such a package must not be published)')
    a = ap.parse_args(argv)
    try:
        out, names = build(a.out, a.force, allow_dirty=a.allow_dirty)
    except (OSError, ValueError) as e:
        print(f'package_windows: {e}')
        return 1
    print(f'package_windows: {out} ({out.stat().st_size // 1024} KiB)')
    for name in names:
        print(f'  {name}')
    src = source_archive.source_path(out)
    print(f'package_windows: source {src} ({src.stat().st_size // 1024} KiB) -- publish it with the zip')
    return 0


if __name__ == '__main__':
    sys.exit(main())
