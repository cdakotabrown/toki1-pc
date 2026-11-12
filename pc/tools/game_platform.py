#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""game_platform.py -- which build the pc/tools tests run: Linux or Windows.

One switch, the environment variable PC_TEST_PLATFORM:

    PC_TEST_PLATFORM=windows python3 pc/tools/run_unittests.py
    PC_TEST_PLATFORM=windows python3 pc/tools/playthrough.py pc/tools/playthrough/new_game.txt

  * GAME is the game binary the tests start: pc/build/tokimeki-pc (Linux,
    the default) or pc/build-win/tokimeki-pc.exe (windows);
  * safe_process.run() hands every command whose program is a Windows .exe
    to wrap(), which starts it as `wine EXE ...` in a WINEPREFIX of its own:
    a copy (cp -a, about a second) of one template prefix that `wineboot
    --init` made once under build/wine-test/.  Runs are therefore isolated
    from ~/.wine, from each other (parallel-safe: each has its own
    wineserver) and from other users' Wine; the cleanup stops that prefix's
    wineserver (`wineserver -k` with that WINEPREFIX only) and deletes it.

Paths need no translation for the game: Wine resolves an absolute Unix path
through drive Z:, which maps "/".  host_path() gives the spelling the game
prints (Z:\\...) for tests that compare logged paths.
"""
import fcntl
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[2]
PLATFORM = os.environ.get('PC_TEST_PLATFORM', 'linux').strip().lower() or 'linux'
if PLATFORM not in ('linux', 'windows'):
    raise SystemExit(f'PC_TEST_PLATFORM={PLATFORM!r}: expected linux or windows')
WINDOWS = PLATFORM == 'windows'

LINUX_GAME = ROOT / 'pc/build/tokimeki-pc'
# PC_TEST_WINDOWS_EXE: another exe (a packaged one, or an older build as a
# negative control)
WINDOWS_GAME = Path(os.environ.get('PC_TEST_WINDOWS_EXE') or ROOT / 'pc/build-win/tokimeki-pc.exe')
GAME = WINDOWS_GAME if WINDOWS else LINUX_GAME
GAME_NAME = GAME.name               # the file name a copied install uses

WINE_DIR = Path(os.environ.get('PC_TEST_WINE_DIR', ROOT / 'build/wine-test'))
TEMPLATE = WINE_DIR / 'template'
_counter = itertools.count()
_lock = threading.Lock()


def wineserver():
    """Debian/Ubuntu keep wineserver off PATH."""
    for cand in (shutil.which('wineserver'), '/usr/lib/x86_64-linux-gnu/wine/wineserver',
                 '/usr/lib/wine/wineserver', '/usr/libexec/wine/wineserver'):
        if cand and os.path.exists(cand):
            return cand
    return None


def wine_available():
    return shutil.which('wine') is not None and wineserver() is not None


def game_available():
    """The selected build exists (and, for Windows, Wine can run it)."""
    return GAME.is_file() and (not WINDOWS or wine_available())


def is_windows_exe(program):
    return os.name != 'nt' and str(program).lower().endswith('.exe')


def _wine_env(base, prefix):
    env = dict(os.environ if base is None else base)
    env['WINEPREFIX'] = str(prefix)
    env.setdefault('WINEDEBUG', '-all')       # Wine's diagnostics stay out of game logs
    env.setdefault('LANG', 'C.UTF-8')         # Wine decodes UTF-8 path names by it
    env.pop('WINESERVER', None)
    # never on the developer's desktop (WSLg shows any window): no display
    # for Wine at all -- its own dialogs and explorer included -- and the
    # game's SDL on the dummy drivers unless a test asks otherwise
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    env.setdefault('SDL_VIDEODRIVER', 'dummy')
    env.setdefault('SDL_AUDIODRIVER', 'dummy')
    # no desktop-menu entries in $HOME, no Mono/Gecko install prompts
    env.setdefault('WINEDLLOVERRIDES', 'winemenubuilder.exe=d;mscoree=d;mshtml=d')
    return env


def template():
    """The template prefix, made once (wineboot --init) under a file lock so
    that parallel test processes wait for one another instead of racing."""
    WINE_DIR.mkdir(parents=True, exist_ok=True)
    with open(WINE_DIR / '.template.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not (TEMPLATE / '.ready').exists():
            shutil.rmtree(TEMPLATE, ignore_errors=True)
            env = _wine_env(None, TEMPLATE)
            subprocess.run(['wine', 'wineboot', '--init'], env=env, timeout=900,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            subprocess.run([wineserver(), '-w'], env=env, timeout=300)
            (TEMPLATE / '.ready').write_text('wineboot --init\n')
    return TEMPLATE


def new_prefix():
    """A private copy of the template prefix."""
    tpl = template()
    with _lock:
        n = next(_counter)
    prefix = WINE_DIR / 'runs' / f'{os.getpid()}-{threading.get_ident() % 100000}-{n}'
    shutil.rmtree(prefix, ignore_errors=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cp', '-a', str(tpl), str(prefix)], check=True)
    return prefix


def stop_prefix(prefix, remove=True):
    """Stop this prefix's wineserver (and with it every process of the
    prefix) and delete the prefix.  Touches no other Wine."""
    server = wineserver()
    if server:
        env = _wine_env(None, prefix)
        try:
            subprocess.run([server, '-k'], env=env, timeout=60,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            pass
    if remove and not os.environ.get('PC_TEST_KEEP_PREFIX'):
        shutil.rmtree(prefix, ignore_errors=True)


def wrap(cmd, env):
    """(cmd, env, cleanup) for safe_process.run: a Windows .exe runs under
    Wine in a fresh private prefix; anything else is returned unchanged."""
    cmd = [str(c) for c in cmd]
    if cmd and os.path.basename(cmd[0]) == 'tokimeki-pc':
        # the Linux game: headless unless the caller chose a driver
        env = dict(os.environ if env is None else env)
        env.setdefault('SDL_VIDEODRIVER', 'dummy')
        env.setdefault('SDL_AUDIODRIVER', 'dummy')
        return cmd, env, lambda: None
    if not cmd or not is_windows_exe(cmd[0]):
        return cmd, env, lambda: None
    prefix = new_prefix()
    return ['wine'] + cmd, _wine_env(env, prefix), lambda: stop_prefix(prefix)


def host_path(path):
    """The spelling of a host path as the selected build prints it."""
    path = os.path.abspath(str(path))
    return 'Z:' + path.replace('/', '\\') if WINDOWS else path


def describe():
    return f'{PLATFORM} ({GAME})'


def command_path():
    """A program path that starts the selected game like GAME does, for
    helpers that call subprocess directly with [binary, args...] (no
    safe_process.run): the Linux binary itself, or on Windows a small
    launcher script that runs `game_platform.py run EXE ARGS...`."""
    if not WINDOWS:
        return str(GAME)
    WINE_DIR.mkdir(parents=True, exist_ok=True)
    script = WINE_DIR / 'tokimeki-pc-wine'
    text = ('#!/bin/sh\n# written by pc/tools/game_platform.py: the Windows build under Wine\n'
            f'exec "{os.path.realpath(os.sys.executable)}" "{Path(__file__).resolve()}" run '
            f'"{WINDOWS_GAME}" "$@"\n')
    if not script.exists() or script.read_text() != text:
        tmp = script.with_suffix(f'.{os.getpid()}')
        tmp.write_text(text)
        tmp.chmod(0o755)
        os.replace(tmp, script)
    return str(script)


def _run_cli(argv):
    """`game_platform.py run PROGRAM ARGS...`: run it (an .exe under Wine in
    a private prefix), pass stdin/stdout/stderr through, exit with its
    status, and stop and delete the prefix afterwards (also on SIGTERM)."""
    import signal
    cmd, env, cleanup = wrap(argv, None)
    proc = subprocess.Popen(cmd, env=env)

    def stop(signum, frame):
        proc.terminate()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        rc = proc.wait()
    finally:
        cleanup()
    return rc if rc >= 0 else 128 - rc


if __name__ == '__main__':
    import sys
    if sys.argv[1:2] == ['run'] and len(sys.argv) > 2:
        sys.exit(_run_cli(sys.argv[2:]))
    print(describe())
