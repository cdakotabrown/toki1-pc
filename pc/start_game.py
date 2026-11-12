#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""First-run launcher: supply your own supported disc once, then just play.

The portable directory contains this launcher, tokimeki-pc, tools/, and
locally generated pc/assets/. No game data is included in the distribution.

Player settings (window, scaling, keys, controller, volumes) live in a
settings file the game creates on its first launch; --where prints its path.
Saves (the memory card and quicksave slots) live in <data dir>/saves, outside
the game folder; the launcher passes it to the game as TOKI_SAVE_DIR and copies
an older <game dir>/saves/card1.mcr there once.
Reference: docs/pc-port/PLAYER-GUIDE.md.
"""
import argparse
import math
import os
from pathlib import Path
import sys

if sys.version_info < (3, 10):
    raise SystemExit('The launcher requires Python 3.10 or newer.')

HERE = Path(__file__).resolve().parent
if HERE.is_dir():
    sys.path.insert(0, str(HERE / 'tools'))
else:  # __file__ points inside an executable .pyz, not a real directory.
    HERE = Path(sys.argv[0]).resolve().parent
from bootstrap_assets import prepare_assets  # noqa: E402
from safe_process import run  # noqa: E402


def default_game():
    name = 'tokimeki-pc.exe' if os.name == 'nt' else 'tokimeki-pc'
    packaged = HERE / name
    # the Windows package's name for it (pc/tools/package_windows.py PLAYER_EXE)
    if os.name == 'nt' and (HERE / 'Heartthrob Memorial PC Enhanced.exe').is_file():
        return HERE / 'Heartthrob Memorial PC Enhanced.exe'
    if packaged.is_file() or not (HERE / 'Makefile').is_file():
        return packaged
    return HERE / 'build' / name


def config_dir():
    """Mirror of pc/hal/settings.c pc_settings_paths (config directory)."""
    if os.environ.get('TOKI_CONFIG_DIR'):
        return Path(os.environ['TOKI_CONFIG_DIR'])
    home = Path.home()
    if os.name == 'nt':
        return Path(os.environ.get('APPDATA', '.')) / 'tokimeki-pc'
    if sys.platform == 'darwin':
        return home / 'Library' / 'Application Support' / 'tokimeki-pc'
    xdg = os.environ.get('XDG_CONFIG_HOME', '')
    return (Path(xdg) if xdg.startswith('/') else home / '.config') / 'tokimeki-pc'


def data_dir():
    if os.environ.get('TOKI_CONFIG_DIR') or os.name == 'nt' or sys.platform == 'darwin':
        return config_dir()
    xdg = os.environ.get('XDG_DATA_HOME', '')
    return (Path(xdg) if xdg.startswith('/') else Path.home() / '.local' / 'share') / 'tokimeki-pc'


def _ini_value(text, section, key):
    """[section] key from the settings file's INI subset (pc/hal/settings.c):
    '#'/';' comments, optional quotes; the last assignment wins."""
    current, found = '', None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in '#;':
            continue
        if line.startswith('['):
            current = line[1:line.find(']')].strip().lower() if ']' in line else ''
            continue
        name, eq, value = line.partition('=')
        if not eq or current != section or name.strip().lower() != key:
            continue
        value = value.strip()
        if value[:1] in ('"', "'") and value[0] in value[1:]:
            value = value[1:value.index(value[0], 1)]
        else:
            for i, ch in enumerate(value):
                if ch in '#;' and (i == 0 or value[i - 1].isspace()):
                    value = value[:i]
                    break
            value = value.strip()
        found = value
    return found


def save_dir(game_dir, settings_file=None, overrides=()):
    """Where the player's memory card and quicksaves live, as the HAL resolves
    it (pc/hal/save_paths.c): TOKI_SAVE_DIR, else [system] save_dir (--set,
    then the settings file), else <data dir>/saves.  A relative save_dir is
    relative to the game's folder (the HAL's working directory)."""
    if os.environ.get('TOKI_SAVE_DIR'):
        return Path(os.environ['TOKI_SAVE_DIR'])
    configured = None
    if settings_file is not None:
        try:
            configured = _ini_value(Path(settings_file).read_text(errors='replace'), 'system', 'save_dir')
        except OSError:
            configured = None
    for item in overrides:
        key, _, value = item.partition('=')
        if key.strip().lower() == 'system.save_dir':
            configured = value.strip().strip('"\'')
    if configured:
        path = Path(configured).expanduser()
        return path if path.is_absolute() else Path(game_dir) / path
    return data_dir() / 'saves'


def migrate_saves(old_dir, new_dir):
    """Copy (never move) <old_dir>/card1.mcr into new_dir once: only when the
    new folder has no card yet.  Returns the copied path or None."""
    import shutil
    old_card, new_card = Path(old_dir) / 'card1.mcr', Path(new_dir) / 'card1.mcr'
    try:
        if Path(old_dir).resolve() == Path(new_dir).resolve():
            return None
    except OSError:
        return None
    if new_card.exists() or not old_card.is_file():
        return None
    Path(new_dir).mkdir(parents=True, exist_ok=True)
    tmp = new_card.with_name('card1.mcr.migrating')
    shutil.copy2(old_card, tmp)
    os.replace(tmp, new_card)
    return new_card


FIRST_RUN_HELP = """
No game data has been prepared yet. Supply your own disc image once:

  {prog} --disc '/path/to/Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue'

Use the complete BIN/CUE dump (data and audio tracks), not a cooked ISO.
No PlayStation BIOS is needed. Later launches need no disc.
Help: docs/pc-port/PLAYER-GUIDE.md
"""


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disc', type=Path, help='your supported raw Track 1 BIN or CUE (first run only)')
    parser.add_argument('--audio-track', type=Path, help='Track 2 BIN if not using a complete CUE')
    parser.add_argument('--game', type=Path, default=default_game(), help='native game executable')
    parser.add_argument('--prepare-only', action='store_true', help='prepare/verify assets without starting the game')
    parser.add_argument('--headless', action='store_true', help='dummy SDL devices; exit after --frames VBlanks')
    parser.add_argument('--frames', type=int, default=1200, help='headless VBlank budget (1..40000)')
    parser.add_argument('--timeout', type=float, default=120, help='headless deadline in seconds (max 600)')
    parser.add_argument('--auto', '-a', action='store_true', help='developer autoplay (not normal interactive play)')
    parser.add_argument('--vcxsrv', action='store_true', help='legacy launcher handles WSL display selection')
    player = parser.add_argument_group('player settings (this launch only; see --where)')
    player.add_argument('--settings', type=Path, help='use this settings file instead of the default one')
    player.add_argument('--set', action='append', default=[], metavar='SECTION.KEY=VALUE',
                        help='override one setting, e.g. --set display.scale=integer (repeatable)')
    mode = player.add_mutually_exclusive_group()
    mode.add_argument('--fullscreen', action='store_true', help='start fullscreen')
    mode.add_argument('--windowed', action='store_true', help='start in a window')
    profile = player.add_mutually_exclusive_group()
    profile.add_argument('--profile', choices=('parity', 'enhanced'),
                         help='run profile (default: the settings file, else enhanced; automated runs: parity)')
    profile.add_argument('--enhanced', action='store_const', const='enhanced', dest='profile',
                         help='same as --profile enhanced: instant loads, steady frame rate, '
                              'fast-forward on the ` key')
    player.add_argument('--where', action='store_true',
                        help='print where settings, saves, screenshots and assets live, then exit')
    player.add_argument('--resume', action='store_true',
                        help='continue where you left off: load the state saved when you last quit '
                             '(the same build of the game only; see PLAYER-GUIDE.md "Saves")')
    args = parser.parse_args(argv)
    if not 1 <= args.frames <= 40000:
        parser.error('--frames must be between 1 and 40000')
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 600:
        parser.error('--timeout must be finite and between 0 and 600 seconds')
    overrides = list(args.set)
    for item in overrides:
        key, eq, _ = item.partition('=')
        if not eq or not key.strip() or ('.' not in key and key.strip() != 'profile'):
            parser.error(f'--set expects SECTION.KEY=VALUE, got {item!r}')
    if args.fullscreen or args.windowed:
        overrides.append(f'display.fullscreen={"true" if args.fullscreen else "false"}')
    if args.profile:
        overrides.append(f'profile={args.profile}')
    game = args.game.expanduser().resolve()
    settings_file = (args.settings.expanduser().resolve() if args.settings
                     else config_dir() / 'settings.ini')
    saves = save_dir(game.parent, settings_file, overrides)
    old_saves = game.parent / 'saves'
    if args.where:
        note = '' if settings_file.exists() else '  (created on first launch)'
        print(f'Settings:    {settings_file}{note}')
        print(f'Window:      [state] of {settings_file} (the place and size the game window had)')
        print(f'Screenshots: {data_dir() / "screenshots"}  (unless [system] screenshot_dir is set)')
        print(f'Saves:       {saves}  (memory card card1.mcr, quicksave/ slots)')
        print(f'Textures:    {data_dir() / "textures"}  (dump/SLPS-00065/ and packs/NAME/; '
              'enhanced profile)')
        if (old_saves / 'card1.mcr').is_file() and old_saves.resolve() != saves.resolve():
            state = ('copied' if (saves / 'card1.mcr').exists()
                     else 'copied there on the next launch')
            print(f'Old saves:   {old_saves}  ({state}; this copy is left in place)')
        print(f'Assets:      {game.parent / "pc" / "assets"}')
        return 0
    if args.disc is not None and not args.disc.expanduser().exists():
        parser.exit(1, f'Disc image not found: {args.disc}\n')
    if not args.prepare_only and not game.is_file():
        parser.exit(1, f'Native game executable not found: {game}\nBuild/package the port first.\n')
    # The HAL still has legacy pc/assets-relative readers. A fixed portable
    # working-directory contract handles all of them without symlinks, extra
    # asset copies, developer disks/, or assumptions about the caller's cwd.
    assets = game.parent / 'pc' / 'assets'
    try:
        result = prepare_assets(args.disc, assets, audio_track=args.audio_track)
    except (ValueError, KeyError, TypeError, OSError) as error:
        if args.disc is None and not (assets.exists() or assets.is_symlink()):
            parser.exit(1, FIRST_RUN_HELP.format(prog=os.environ.get('TOKI_LAUNCHER', parser.prog)))
        parser.exit(1, f'Asset setup failed: {error}\n')
    print(f'Assets ready: {assets} ({len(result["files"])} verified files)', flush=True)
    if result.get('preserved_source_anomalies'):
        print('Warning: source XA subheader anomaly preserved byte-for-byte; see assets.manifest.json.', flush=True)
    if args.prepare_only:
        return 0
    if any(len(os.fsencode(assets / row['path'])) >= 512 for row in result['files']):
        parser.exit(1, 'Installation path exceeds the native HAL path buffer; move the game to a shorter path.\n')
    os.chdir(game.parent)
    if args.settings:
        os.environ['TOKI_SETTINGS'] = str(settings_file)
    if overrides:
        os.environ['TOKI_SET'] = ';'.join(overrides)
    if not args.headless:
        # The player's saves live outside the game folder (a clean rebuild
        # of a source checkout deletes pc/build); headless smoke runs keep
        # the historical <game dir>/saves.
        os.environ['TOKI_SAVE_DIR'] = str(saves)
        try:
            copied = migrate_saves(old_saves, saves)
        except OSError as error:
            copied = None
            print(f'Warning: could not copy {old_saves / "card1.mcr"} to {saves}: {error}',
                  file=sys.stderr, flush=True)
        print(f'Settings: {settings_file}', flush=True)
        print(f'Saves: {saves} (back this folder up to keep your memory card and quicksaves)',
              flush=True)
        if copied:
            print(f'Copied your memory card {old_saves / "card1.mcr"} -> {copied} '
                  '(the old copy is left in place)', flush=True)
        try:
            inside = saves.resolve().is_relative_to(game.parent.resolve())
        except OSError:
            inside = False
        if inside and ((game.parent / 'Makefile').is_file() or game.parent.name == 'build'):
            print(f'Warning: {saves} is inside {game.parent}; deleting that folder (e.g. a clean '
                  'rebuild) deletes your saves too', file=sys.stderr, flush=True)
        if (sys.platform.startswith('linux') and not os.environ.get('SDL_VIDEODRIVER')
                and not os.environ.get('DISPLAY') and not os.environ.get('WAYLAND_DISPLAY')):
            print('Warning: no DISPLAY or WAYLAND_DISPLAY is set, so no window can open '
                  '(the game would run invisibly). Start it from a desktop session.',
                  file=sys.stderr, flush=True)
    if args.headless:
        os.environ.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                          PC_TRACE_CLOCK='vblank', PC_TRACE_FRAMES=str(args.frames))
    if args.auto:
        os.environ['AUTOPLAY'] = '1'
    if args.resume:
        if args.headless:
            parser.error('--resume is for interactive play, not --headless')
        if os.name == 'nt':
            parser.error('--resume needs save states, which the Windows build does not have yet; '
                         'your memory card saves work as usual')
        resume = saves / 'quicksave' / 'resume.tks'
        if not resume.is_file():
            print(f'Nothing to resume ({resume} does not exist); starting normally.', flush=True)
        else:
            os.environ['TOKI_RESUME'] = '1'
    command = [str(game), str(assets)]
    if args.profile:
        command.append(f'--profile={args.profile}')
    if args.headless:
        log = game.parent / 'headless.log'
        result = run(command, timeout=args.timeout, max_output=8 * 1024**2,
                     log_path=log, watch_files=[(game.parent / 'audio_dump.wav', 128 * 1024**2, 'AUDIO_LIMIT')])
        print(result.stdout[-2000:])
        print(f'Headless log: {log}')
        if not result.ok:
            print(f'Headless run failed: {result.reason or result.returncode}', file=sys.stderr)
            return 124 if result.reason == 'TIMEOUT' else 1
        return 0
    if os.name == 'nt':
        # Windows has no exec(): os.execv would start the game and return to
        # the console at once.  Run it and hand back its exit status.
        import subprocess
        return subprocess.call(command)
    os.execv(str(game), command)


if __name__ == '__main__':
    raise SystemExit(main())
