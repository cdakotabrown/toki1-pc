#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fresh_install.py -- install the player package into empty folders, as a
player would, and prove each first-run path works.

    python3 pc/tools/fresh_install.py [--platform linux|windows|both]
        [--out build/fresh-install] [--only CASE,CASE] [--keep]

Needs the packages (pc/tools/package_linux.py, package_windows.py), the dev
build (pc/build/tokimeki-pc, the reference) and your own disc in disks/.
Every run is isolated: its own HOME (Linux) or a fresh WINEPREFIX under
--out (Windows, `wineserver -k` at the end); the disc is symlinked
into each install's disk/ folder.  Nothing here touches
pc/build, ~/.local/share/tokimeki-pc or ~/.wine.  Error-case discs are
sparse files or tiny stubs: no game bytes are copied.

Cases (each also has its negative control, named in its result):
  disk           exe + disk/ only -> setup -> boot; trace == the dev build's
  second-launch  no setup again, fast check only; trace == the dev build's
  python-equal   (linux) the C-extracted tree == pc/tools/bootstrap_assets.py's
  enhanced       the enhanced profile boots from the same assets
  playthrough    new_game playthrough on the install: traces + card == dev
  disc-flag      --disc PATH (no disk/)
  drop           an injected SDL_DROPFILE on the no-disc screen
  autodetect     disc in ~/Downloads/<dir>: the "found it elsewhere" screen
  packs          texture + music dumps, then packs in textures/packs and
                 music/packs, picked up from the portable folder
  audit          a player-like run from an unrelated working folder creates
                 nothing outside the install folder (control: TOKI_SAVE_DIR
                 pointed at the old per-user place must be caught)
  check          --check passes; on damaged data it fails
  damaged        a truncated asset -> the damaged screen -> re-extract
  err-*          each error screen (see ERROR_CASES)
  readonly       read-only install folder -> per-user fallback, said on screen
Screenshots (BMP) of every setup screen go to OUT/shots/PLATFORM/CASE/.

--userland ROOTFS (Linux): every run of the installed game, the
playthrough's included, happens inside that root file system through
pc/tools/linux_pkg_exec.sh --run (bwrap, no root): the package on an old
distribution's glibc and nothing of the host's /usr.  `--userland default`
is build/linux-pkg-root, the Ubuntu 20.04 root of setup_linux_pkg_root.sh.
The dev build's reference runs stay on the host.
"""
import argparse
import filecmp
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_process import run  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RELEASE = 'Tokimeki Memorial - Forever with You (Japan) (Rev 1)'
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import disc_input  # noqa: E402
# your disc: this checkout's disks/, else the toki1-decomp checkout's
CUE = disc_input(f'disks/{RELEASE}.cue')
TRACK1 = CUE.parent / f'{RELEASE} (Track 1).bin'
TRACK2 = CUE.parent / f'{RELEASE} (Track 2).bin'
TRACK1_SIZE, TRACK2_SIZE = 702104928, 45151344
FRAMES = 600
KEEP = False      # --keep: leave every case's folder for inspection
USERLAND = None   # --userland: the Linux runs go through linux_pkg_exec.sh --run
EXEC = ROOT / 'pc/tools/linux_pkg_exec.sh'
CUE_TEXT = (f'FILE "{RELEASE} (Track 1).bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
            f'FILE "{RELEASE} (Track 2).bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n'
            '    INDEX 01 00:02:00\n')


class Platform:
    def __init__(self, name, out):
        self.name, self.out = name, out
        self.windows = name == 'windows'
        self.prefix = out / 'wineprefix' if self.windows else None
        self.package = ROOT / ('build/tokimeki-pc-windows.zip' if self.windows
                               else 'build/tokimeki-pc-linux-x86_64.tar.gz')
        self.exe_name = 'Heartthrob Memorial PC Enhanced.exe' if self.windows else 'tokimeki-pc'

    def path(self, p):
        """A host path as the game sees it (Wine: Z:\\...)."""
        p = str(p)
        return 'Z:' + p.replace('/', '\\') if self.windows else p

    def command(self, exe, args=()):
        return self.runner() + [str(exe)] + [str(a) for a in args]

    def runner(self):
        """What goes before the binary: wine, the old-userland wrapper, or nothing."""
        if self.windows:
            return ['wine']
        return ['sh', str(EXEC), '--run'] if USERLAND else []

    def base_env(self, home):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'CPU_COST', 'GP0_', 'XDG_',
                                    'SDL_', 'WINE'))}
        env.update(HOME=str(home), SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', WINEDEBUG='-all')
        env.setdefault('LANG', 'C.UTF-8')       # Wine decodes UTF-8 folder names by it
        if USERLAND and not self.windows:
            env.update(userland_env())
        if self.windows:
            env['WINEPREFIX'] = str(self.prefix)
            # no display for Wine: nothing may appear on the desktop (WSLg)
            env.pop('DISPLAY', None)
            env.pop('WAYLAND_DISPLAY', None)
        return env

    def user_dir(self):
        """Wine: the prefix's user folder (C:\\users\\NAME)."""
        users = self.prefix / 'drive_c/users'
        names = [n for n in os.listdir(users) if n not in ('Public', 'Default')]
        return users / names[0]

    def appdata(self, home):
        if self.windows:
            return self.user_dir() / 'AppData/Roaming/tokimeki-pc'
        return home / '.local/share/tokimeki-pc'


def userland_env():
    """linux_pkg_exec.sh --run's root and the disc's folders (read-only)."""
    dirs = sorted({str(f.resolve().parent) for f in (CUE, TRACK1, TRACK2)})
    return {'LINUX_PKG_ROOTFS': str(USERLAND), 'LINUX_PKG_RUN_BINDS': ':'.join(dirs)}


def link_disc(dest, names=None):
    """The real disc files into dest, as symlinks.  Not hard links: a new
    link changes the file's ctime, which the extractor's "source changed
    during extraction" check (the Python tool's too) rightly reports when
    another scenario is reading the same file at that moment."""
    dest.mkdir(parents=True, exist_ok=True)
    names = names or {CUE: CUE.name, TRACK1: TRACK1.name, TRACK2: TRACK2.name}
    for src, name in names.items():
        os.symlink(src.resolve(), dest / name)


def sparse(path, size):
    with open(path, 'wb') as fh:
        fh.truncate(size)


def harness_env(plat, home, trace, frames=FRAMES, extra=()):
    env = plat.base_env(home)
    env.update(PC_TURBO='1', PC_TRACE=str(trace), PC_TRACE_PHASE='pre_irq',
               PC_TRACE_FRAMES=str(frames), TOKI_INPUT_DEVICE='pad')
    env.update(dict(extra))
    return env


def player_env(plat, home, frames=900, extra=()):
    env = plat.base_env(home)
    env.update(TOKI_TEST_AS_PLAYER='1', TOKI_TEST_QUIT_FRAMES=str(frames))
    env.update(dict(extra))
    return env


class Runner:
    def __init__(self, plat, out):
        self.plat, self.out = plat, out
        self.results = []

    def fresh(self, case):
        """An empty install: the package unpacked into its own folder."""
        base = self.out / self.plat.name / case
        if base.exists():
            make_writable(base)
            shutil.rmtree(base)
        (base / 'home').mkdir(parents=True)
        (base / 'cwd').mkdir()
        if self.plat.windows:
            with zipfile.ZipFile(self.plat.package) as zf:
                zf.extractall(base)
        else:
            with tarfile.open(self.plat.package) as tf:
                tf.extractall(base, filter='data')
        game = base / 'tokimeki-pc'
        return base, game, game / self.plat.exe_name

    def shots(self, case):
        d = self.out / 'shots' / self.plat.name / case
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        return d

    def game(self, exe, args, cwd, env, log, timeout=900):
        t0 = time.monotonic()
        r = run(self.plat.command(exe, args), cwd=str(cwd), env=env, timeout=timeout,
                log_path=str(log), max_output=256 * 1024 * 1024)
        r.elapsed = time.monotonic() - t0
        r.text = Path(log).read_text(errors='replace')
        return r

    def record(self, case, ok, detail, **extra):
        row = dict(platform=self.plat.name, case=case, ok=bool(ok), detail=detail, **extra)
        self.results.append(row)
        print(f'[{self.plat.name}] {case:16s} {"PASS" if ok else "FAIL"}  {detail[:160]}', flush=True)
        return ok


def discard(path):
    if not KEEP:
        make_writable(path)
        shutil.rmtree(path, ignore_errors=True)


def make_writable(path):
    for d, dirs, files in os.walk(path):
        try:
            os.chmod(d, 0o755)
        except OSError:
            pass


def shot_names(d):
    return sorted(p.name for p in Path(d).glob('*.bmp'))


def reference_traces(out):
    """The dev build's 600-frame boots (parity, enhanced) on its usual layout."""
    ref = {}
    dev = ROOT / 'pc/build/tokimeki-pc'
    for profile in ('parity', 'enhanced'):
        work = out / 'ref' / profile
        shutil.rmtree(work, ignore_errors=True)
        (work / 'pc').mkdir(parents=True)
        os.symlink(ROOT / 'pc/assets', work / 'pc/assets')
        trace = out / 'ref' / f'{profile}.trace'
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'CPU_COST', 'GP0_'))}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1', PC_TRACE=str(trace),
                   PC_TRACE_PHASE='pre_irq', PC_TRACE_FRAMES=str(FRAMES), TOKI_INPUT_DEVICE='pad')
        if profile == 'enhanced':
            env['TOKI_PROFILE'] = 'enhanced'
        r = run([str(dev), str(ROOT / 'pc/assets')], cwd=str(work), env=env, timeout=600,
                log_path=str(out / 'ref' / f'{profile}.log'), max_output=256 * 1024 * 1024)
        if r.returncode != 0 or not trace.exists():
            raise SystemExit(f'reference {profile} boot failed (log {out}/ref/{profile}.log)')
        ref[profile] = trace
    return ref


def same_file(a, b):
    return Path(a).exists() and Path(b).exists() and filecmp.cmp(a, b, shallow=False)


# ---------------------------------------------------------------- cases

def case_disk(R, ref):
    base, game, exe = R.fresh('disk')
    link_disc(game / 'disk')
    shots = R.shots('disk')
    trace = base / 'boot.trace'
    env = harness_env(R.plat, base / 'home', trace, extra={'TOKI_SETUP_SHOTS': R.plat.path(shots)})
    r = R.game(exe, [], base / 'cwd', env, base / 'run.log')
    log = (game / 'data/tokimeki-pc.log').read_text(errors='replace') if (game / 'data/tokimeki-pc.log').exists() else ''
    names = shot_names(shots)
    ok = (r.returncode == 0 and 'game data ready' in log and (game / 'data/assets.manifest.json').exists()
          and same_file(trace, ref['parity']) and any('progress' in n for n in names)
          and any('ready' in n for n in names) and '[FOLD] total=0 ' in r.text)
    R.record('disk', ok, f'exit {r.returncode}, {r.elapsed:.0f} s, trace '
             f'{"==" if same_file(trace, ref["parity"]) else "!="} dev, shots {names}',
             negative_control='second-launch must NOT run the setup')
    return base, game, exe


def case_second(R, ref, main):
    base, game, exe = main
    trace = base / 'boot2.trace'
    r = R.game(exe, [], base / 'cwd', harness_env(R.plat, base / 'home', trace), base / 'run2.log')
    log = (game / 'data/tokimeki-pc.log').read_text(errors='replace')
    old = (game / 'data/tokimeki-pc.log.old').exists()
    ok = (r.returncode == 0 and 'sizes and executable SHA1 checked' in log and '[SETUP] preparing' not in log
          and same_file(trace, ref['parity']) and old)
    R.record('second-launch', ok, f'exit {r.returncode}, {r.elapsed:.1f} s, no setup: '
             f'{"[SETUP] preparing" not in log}, previous log kept as .log.old: {old}')


def case_python_equal(R, main):
    base, game, exe = main
    py = R.out / 'python-ref'
    if not py.exists():
        r = run([sys.executable, str(ROOT / 'pc/tools/bootstrap_assets.py'), str(CUE), '--out', str(py)],
                timeout=900, log_path=str(R.out / 'python-ref.log'))
        if r.returncode != 0:
            return R.record('python-equal', False, f'python extraction failed ({R.out}/python-ref.log)')
    c = game / 'data'
    a = sorted(str(p.relative_to(py)) for p in py.rglob('*') if p.is_file())
    # data/ also holds the log and the English files (en/): the program's own
    b = sorted(str(p.relative_to(c)) for p in c.rglob('*') if p.is_file()
               and p.relative_to(c).parts[0] not in ('en', 'tokimeki-pc.log', 'tokimeki-pc.log.old'))
    diff = [n for n in a if not filecmp.cmp(py / n, c / n, shallow=False)] if a == b else ['file lists differ']
    R.record('python-equal', a == b and not diff,
             f'{len(a)} files, manifest {"identical" if same_file(py / "assets.manifest.json", c / "assets.manifest.json") else "DIFFERS"}, '
             f'{len(diff)} differing', negative_control='test_asset_setup_c.py flips one byte')


def case_enhanced(R, ref, main):
    base, game, exe = main
    trace = base / 'enhanced.trace'
    r = R.game(exe, [], base / 'cwd', harness_env(R.plat, base / 'home', trace,
                                                   extra={'TOKI_PROFILE': 'enhanced'}), base / 'enh.log')
    same = same_file(trace, ref['enhanced'])
    differs_from_parity = not same_file(trace, ref['parity'])
    R.record('enhanced', r.returncode == 0 and same and differs_from_parity,
             f'exit {r.returncode}; trace == dev enhanced: {same}; differs from parity (control): {differs_from_parity}')


def case_playthrough(R, main, dev_pt):
    base, game, exe = main
    out = base / 'playthrough'
    cmd = [sys.executable, str(ROOT / 'pc/tools/playthrough.py'), str(ROOT / 'pc/tools/playthrough/new_game.txt'),
           '--out', str(out), '--bin', str(exe), '--installed']
    env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'HOME'))}
    env.update(HOME=str(base / 'home'))
    if R.plat.windows:
        cmd += ['--runner', 'wine']
        env.update(WINEPREFIX=str(R.plat.prefix), WINEDEBUG='-all')
    elif USERLAND:
        cmd += ['--runner', ' '.join(shlex.quote(a) for a in R.plat.runner())]
        env.update(userland_env())
    r = run(cmd, cwd=str(ROOT), env=env, timeout=3600, log_path=str(base / 'playthrough.log'),
            max_output=64 * 1024 * 1024)
    text = Path(base / 'playthrough.log').read_text(errors='replace')
    same = all(same_file(out / n, dev_pt / n) for n in
               ('new_game.pc.trace', 'reboot.pc.trace', 'new_game.card1.mcr', 'reboot.card1.mcr'))
    R.record('playthrough', r.returncode == 0 and 'PASS: 29 milestones' in text and same,
             f'{"PASS 29" if "PASS: 29 milestones" in text else "not passed"}; traces + cards == dev: {same}')


def setup_case(R, ref, case, place, args=(), script=None, extra=(), frames=120, expect_ready=True):
    """A fresh install with the disc placed by `place(base, game)`, one run."""
    base, game, exe = R.fresh(case)
    shots = R.shots(case)
    place(base, game)
    trace = base / 'boot.trace'
    env = harness_env(R.plat, base / 'home', trace, frames=frames,
                      extra=dict({'TOKI_SETUP_SHOTS': R.plat.path(shots)}, **dict(extra)))
    if script:
        env['TOKI_SETUP_SCRIPT'] = script
    r = R.game(exe, list(args), base / 'cwd', env, base / 'run.log')
    logp = game / 'data/tokimeki-pc.log'
    log = logp.read_text(errors='replace') if logp.exists() else ''
    ready = 'game data ready' in log and r.returncode == 0
    return base, game, exe, r, log, shot_names(shots), ready


def case_utf8(R, ref):
    """The install in a folder whose name is not ASCII (Windows: the UTF-8
    code page manifest; every narrow fopen in the HAL depends on it)."""
    base, game, exe = R.fresh('utf8')
    named = base / 'ときめき メモリアル'
    game.rename(named)
    exe = named / exe.name
    link_disc(named / 'disk')
    trace = base / 'boot.trace'
    r = R.game(exe, [], base / 'cwd', harness_env(R.plat, base / 'home', trace), base / 'run.log')
    logp = named / 'data/tokimeki-pc.log'
    log = logp.read_text(errors='replace') if logp.exists() else ''
    same = same_file(trace, ref['parity'])
    R.record('utf8-folder', r.returncode == 0 and 'game data ready' in log and same,
             f'exit {r.returncode}; extracted into {named.name}/data: {"game data ready" in log}; '
             f'trace == dev: {same}')
    discard(base)


def case_disc_flag(R, ref):
    elsewhere = R.out / R.plat.name / 'elsewhere-disc'
    base, game, exe, r, log, names, ready = setup_case(
        R, ref, 'disc-flag', lambda b, g: None, args=['--disc', R.plat.path(elsewhere / CUE.name)])
    R.record('disc-flag', ready and 'from --disc' in log, f'ready {ready}; shots {names}',
             negative_control='err-no-disk: no --disc, no disk/ -> no-disc screen')
    discard(base)


def case_drop(R, ref):
    elsewhere = R.out / R.plat.name / 'elsewhere-disc'
    base, game, exe, r, log, names, ready = setup_case(
        R, ref, 'drop', lambda b, g: None, script=f'drop:{R.plat.path(elsewhere / CUE.name)}')
    R.record('drop', ready and 'dropped:' in log and any('no-disc' in n for n in names),
             f'ready {ready}; shots {names}')
    discard(base)


def case_autodetect(R, ref):
    def place(base, game):
        if R.plat.windows:
            dl = R.plat.user_dir() / 'Downloads' / 'Tokimeki dump'
            shutil.rmtree(dl, ignore_errors=True)
            link_disc(dl)
        else:
            link_disc(base / 'home/Downloads/Tokimeki dump')
    base, game, exe, r, log, names, ready = setup_case(R, ref, 'autodetect', place, script='key:return')
    if R.plat.windows:
        shutil.rmtree(R.plat.user_dir() / 'Downloads' / 'Tokimeki dump', ignore_errors=True)
    R.record('autodetect', ready and any('found-elsewhere' in n for n in names),
             f'ready {ready}; shots {names}')
    discard(base)


def snapshot(*roots):
    seen = set()
    for root in roots:
        root = Path(root)
        if not root.exists():
            continue
        for d, dirs, files in os.walk(root):
            for n in dirs + files:
                seen.add(os.path.join(d, n))
    return seen


def case_audit(R, main):
    base, game, exe = main
    home, cwd = base / 'home', base / 'cwd'
    watch = [home, cwd, base]
    if R.plat.windows:
        watch.append(R.plat.user_dir())
    results = {}
    for label, extra in (('clean', {}), ('control', {'TOKI_SAVE_DIR': None})):
        env = player_env(R.plat, home, frames=600)
        if label == 'control':
            old = R.plat.appdata(home) / 'saves'
            env['TOKI_SAVE_DIR'] = R.plat.path(old)
        before = snapshot(*watch)
        r = R.game(exe, [], cwd, env, R.out / R.plat.name / f'audit-{label}.log', timeout=600)
        after = snapshot(*watch)
        outside = sorted(p for p in after - before if not p.startswith(str(game) + os.sep))
        inside = sorted(os.path.relpath(p, game) for p in after - before if p.startswith(str(game) + os.sep))
        results[label] = (r.returncode, outside, inside)
    rc, outside, inside = results['clean']
    crc, coutside, _ = results['control']
    ok = rc == 0 and not outside and bool(coutside)
    R.record('audit', ok, f'clean run: {len(outside)} new path(s) outside the install '
             f'{outside[:3]}; inside: {inside[:12]}; control (TOKI_SAVE_DIR at the old place) caught '
             f'{len(coutside)} path(s)', outside=outside, inside=inside, control_outside=coutside)


def case_packs(R, main):
    base, game, exe = main
    home, cwd = base / 'home', base / 'cwd'
    settings = game / 'settings.ini'
    original = settings.read_text() if settings.exists() else None
    settings.write_text('profile = "enhanced"\n[graphics]\ntexture_dump = true\n[audio]\nmusic_dump = true\n'
                        '[enhanced]\nskip_intro = true\n')
    r = R.game(exe, [], cwd, player_env(R.plat, home, frames=2400), base / 'packs-dump.log', timeout=900)
    pngs = sorted((game / 'textures/dump/SLPS-00065').glob('*.png'))
    wavs = sorted((game / 'music/dump').glob('seq-*.wav'))
    if not pngs or not wavs:
        if original is not None:
            settings.write_text(original)
        return R.record('packs', False, f'dump: {len(pngs)} textures, {len(wavs)} songs (log packs-dump.log)')
    tp = game / 'textures/packs/fresh'
    mp = game / 'music/packs/fresh'
    shutil.rmtree(tp, ignore_errors=True)
    shutil.rmtree(mp, ignore_errors=True)
    tp.mkdir(parents=True)
    mp.mkdir(parents=True)
    for p in pngs[:20]:
        shutil.copy(p, tp / p.name)
    shutil.copy(wavs[0], mp / wavs[0].name)
    settings.write_text('profile = "enhanced"\n[graphics]\ntexture_pack = "fresh"\n[audio]\nmusic_pack = "fresh"\n'
                        '[enhanced]\nskip_intro = true\n')
    r2 = R.game(exe, [], cwd, player_env(R.plat, home, frames=2400), base / 'packs-use.log', timeout=900)
    log = (game / 'data/tokimeki-pc.log').read_text(errors='replace')
    tex = [ln for ln in log.splitlines() if ln.startswith('[TEXTURES] texture pack')]
    used = [ln for ln in log.splitlines() if ln.startswith('[TEXTURES] pack:')]
    mus = [ln for ln in log.splitlines() if ' file acc ' in ln and 'packs' in ln]
    ok = r.returncode == 0 and r2.returncode == 0 and tex and 'replacement' in tex[0] and mus
    R.record('packs', ok, f'dumped {len(pngs)} textures + {len(wavs)} songs into the install; '
             f'pack lines: {tex[:1]} {used[:1]} music: {mus[:1]}')
    shutil.rmtree(tp, ignore_errors=True)
    shutil.rmtree(mp, ignore_errors=True)
    if original is not None:
        settings.write_text(original)
    else:
        settings.unlink()


def case_check(R, main, expect_ok=True, label='check'):
    base, game, exe = main
    env = {k: v for k, v in R.plat.base_env(base / 'home').items()}
    r = R.game(exe, ['--check'], base / 'cwd', env, base / f'{label}.log', timeout=900)
    text = r.text + (game / 'data/tokimeki-pc.log').read_text(errors='replace')
    passed = r.returncode == 0 and 'ALL OK' in text
    R.record(label, passed == expect_ok, f'exit {r.returncode}; '
             f'{"ALL OK" if "ALL OK" in text else "PROBLEMS FOUND" if "PROBLEMS FOUND" in text else "?"} '
             f'(expected {"OK" if expect_ok else "a failure"})')


def case_damaged(R, ref, main):
    base, game, exe = main
    victim = game / 'data/raw/CDROM/EXEDIR/OPTION.EXN'
    size = victim.stat().st_size
    with open(victim, 'r+b') as fh:
        fh.truncate(size - 1)
    case_check(R, main, expect_ok=False, label='check-damaged')
    shots = R.shots('damaged')
    trace = base / 'damaged.trace'
    env = harness_env(R.plat, base / 'home', trace, frames=120,
                      extra={'TOKI_SETUP_SHOTS': R.plat.path(shots), 'TOKI_SETUP_SCRIPT': 'key:return'})
    r = R.game(exe, [], base / 'cwd', env, base / 'damaged.log')
    log = (game / 'data/tokimeki-pc.log').read_text(errors='replace')
    names = shot_names(shots)
    aside = (game / 'data.damaged-1').exists()
    ok = r.returncode == 0 and any('damaged' in n for n in names) and aside and 'game data ready' in log
    R.record('damaged', ok, f'shots {names}; moved aside: {aside}; re-extracted: {"game data ready" in log}')
    shutil.rmtree(game / 'data.damaged-1', ignore_errors=True)


ERROR_CASES = {
    # case: (how the disk/ folder looks, script, expected screenshot tag)
    'err-no-disk': ('none', 'quit', 'no-disc'),
    'err-empty-disk': ('empty', 'quit', 'no-disc'),
    'err-other-files': ('other', 'quit', 'no-disc'),
    'err-cue-names': ('renamed', 'quit', 'a_file_the__cue_needs_is_missing'),
    'err-truncated': ('truncated', 'quit', 'track_1_has_the_wrong_size'),
    'err-wrong-release': ('modified', 'quit', 'this_is_not_the_supported_release'),
    'err-cooked-iso': ('iso', 'quit', 'this_is_a_cooked_iso'),
    'err-chd': ('chd', 'quit', 'chd_images_are_not_supported'),
    'err-pbp': ('pbp', 'quit', 'pbp_images_are_not_supported'),
    'err-ecm': ('ecm', 'quit', 'ecm_images_are_not_supported'),
    'err-unrelated': ('unrelated', 'quit', 'this_is_not_a_disc_image'),
    'err-track2-differs': ('track2-sparse', 'quit', 'track_2__cd_audio__differs'),
    'err-disk-full': ('real', 'quit', 'not_enough_free_disk_space'),
    'err-not-writable': ('real', 'quit', 'the_game_data_cannot_be_written'),
    'err-track2-missing': ('no-track2', 'key:c', 'track_2__cd_audio__is_missing'),
}


def place_error(kind, R, base, game):
    disk = game / 'disk'
    if kind == 'none':
        return
    disk.mkdir()
    if kind == 'empty':
        return
    if kind == 'other':
        (disk / 'readme.txt').write_text('my notes\n')
        return
    if kind in ('real', 'no-track2', 'renamed'):
        names = {CUE: CUE.name, TRACK1: TRACK1.name, TRACK2: TRACK2.name}
        if kind == 'no-track2':
            names.pop(TRACK2)
        if kind == 'renamed':
            names = {CUE: CUE.name, TRACK1: 'track01.bin', TRACK2: 'track02.bin'}
        if kind != 'real':
            # the CUE as a file of its own: a symlinked one resolves to the
            # real disks/ folder, where every .bin is present
            names.pop(CUE)
            (disk / CUE.name).write_bytes(CUE.read_bytes())
        link_disc(disk, names)
        return
    (disk / CUE.name).write_text(CUE_TEXT)
    if kind == 'truncated':
        sparse(disk / TRACK1.name, TRACK1_SIZE - 64 * 2352)
        link_disc(disk, {TRACK2: TRACK2.name})
    elif kind == 'modified':
        sparse(disk / TRACK1.name, TRACK1_SIZE)
        link_disc(disk, {TRACK2: TRACK2.name})
    elif kind == 'track2-sparse':
        link_disc(disk, {TRACK1: TRACK1.name})
        sparse(disk / TRACK2.name, TRACK2_SIZE)
    elif kind == 'iso':
        (disk / CUE.name).unlink()
        sparse(disk / 'Tokimeki Memorial.iso', (TRACK1_SIZE // 2352) * 2048)
    elif kind in ('chd', 'pbp', 'ecm'):
        (disk / CUE.name).unlink()
        magic = {'chd': b'MComprHD' + bytes(120), 'pbp': b'\0PBP' + bytes(60), 'ecm': b'ECM\0' + bytes(60)}[kind]
        (disk / f'Tokimeki Memorial.{kind}').write_bytes(magic)
    elif kind == 'unrelated':
        (disk / CUE.name).unlink()


def case_errors(R, ref, only):
    for case, (kind, script, tag) in ERROR_CASES.items():
        if only and case not in only:
            continue
        extra, args = {}, []
        if kind == 'unrelated':
            note = R.out / R.plat.name / 'notes.txt'
            note.parent.mkdir(parents=True, exist_ok=True)
            note.write_text('not a disc\n')
            script = f'drop:{R.plat.path(note)};quit'
        if case == 'err-disk-full':
            extra['TOKI_TEST_FREE_BYTES'] = '1000000'
        if case == 'err-not-writable':
            ro = R.out / R.plat.name / 'readonly-root'
            make_writable(ro) if ro.exists() else None
            shutil.rmtree(ro, ignore_errors=True)
            ro.mkdir(parents=True)
            os.chmod(ro, 0o555)
            extra['TOKI_CONFIG_DIR'] = R.plat.path(ro)
        base, game, exe, r, log, names, ready = setup_case(
            R, ref, case, lambda b, g, k=kind: place_error(k, R, b, g), args=args, script=script,
            extra=extra, frames=60)
        hit = any(tag in n for n in names)
        if case == 'err-track2-missing':
            ok = hit and ready and 'Track 2 left out' in log
            detail = f'screen {hit}; continued without Track 2 and booted: {ready}'
        else:
            ok = hit and not ready
            detail = f'screen "{tag}": {hit}; shots {names}'
        R.record(case, ok, detail, negative_control='disk: the real disc shows no error screen')
        if case == 'err-not-writable':
            make_writable(R.out / R.plat.name / 'readonly-root')
        discard(base)


def case_readonly(R, ref):
    base, game, exe = R.fresh('readonly')
    link_disc(game / 'disk')
    shots = R.shots('readonly')
    os.chmod(game, 0o555)
    trace = base / 'boot.trace'
    env = harness_env(R.plat, base / 'home', trace, frames=60,
                      extra={'TOKI_SETUP_SHOTS': R.plat.path(shots), 'TOKI_SETUP_SCRIPT': 'quit'})
    r = R.game(exe, [], base / 'cwd', env, base / 'run.log', timeout=900)
    make_writable(game)
    appdata = R.plat.appdata(base / 'home')
    used = (appdata / 'data/assets.manifest.json').exists()
    logp = appdata / 'data/tokimeki-pc.log'
    log = logp.read_text(errors='replace') if logp.exists() else ''
    R.record('readonly', r.returncode == 0 and used and 'not writable' in log,
             f'data went to {appdata}: {used}; logged the fallback: {"not writable" in log}; shots {shot_names(shots)}')
    shutil.rmtree(appdata, ignore_errors=True)
    discard(base)


def init_wine(plat):
    """The WINEPREFIX template: wineboot once; every scenario gets a copy."""
    if plat.prefix.exists():
        shutil.rmtree(plat.prefix)
    env = plat.base_env(plat.out / 'wine-home')
    (plat.out / 'wine-home').mkdir(parents=True, exist_ok=True)
    r = run(['wine', 'wineboot', '--init'], env=env, timeout=600, log_path=str(plat.out / 'wineboot.log'))
    stop_wine(plat)
    if r.returncode != 0:
        raise SystemExit(f'wineboot failed ({plat.out}/wineboot.log)')


def wineserver():
    """Debian/Ubuntu keep wineserver off PATH."""
    for cand in (shutil.which('wineserver'), '/usr/lib/x86_64-linux-gnu/wine/wineserver',
                 '/usr/lib/wine/wineserver', '/usr/libexec/wine/wineserver'):
        if cand and os.path.exists(cand):
            return cand
    return None


def stop_wine(plat):
    """wineserver -k for this prefix (it would exit by itself when idle)."""
    server = wineserver()
    if server:
        subprocess.run([server, '-k'], env=plat.base_env(plat.out / 'wine-home'), timeout=120)


def job_platform(template, job):
    """A Platform for one scenario: on Windows its own copy of the template prefix."""
    plat = Platform(template.name, template.out)
    if plat.windows:
        plat.prefix = template.out / 'prefixes' / job
        if plat.prefix.exists():
            shutil.rmtree(plat.prefix)
        plat.prefix.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '-a', str(template.prefix), str(plat.prefix)], check=True)
    return plat


def run_job(template, out, job, fn):
    """One scenario on its own Runner (and prefix); returns its result rows."""
    plat = job_platform(template, job)
    R = Runner(plat, out)
    try:
        fn(R)
    except Exception as e:  # noqa: BLE001 -- a crashed scenario is a failed one
        R.record(job, False, f'scenario raised {type(e).__name__}: {e}')
    finally:
        if plat.windows:
            stop_wine(plat)
            if not KEEP:
                shutil.rmtree(plat.prefix, ignore_errors=True)
    return R.results


def main(argv=None):
    from concurrent.futures import ThreadPoolExecutor
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--platform', choices=('linux', 'windows', 'both'), default='both')
    ap.add_argument('--out', default=str(ROOT / 'build/fresh-install'))
    ap.add_argument('--only', default='', help='comma-separated case names')
    ap.add_argument('--dev-playthrough', default=str(ROOT / 'build/pt-new'),
                    help='the dev build\'s new_game playthrough output to compare with')
    ap.add_argument('--jobs', type=int, default=8, help='scenarios at once (default 8)')
    ap.add_argument('--keep', action='store_true', help='keep the installs')
    ap.add_argument('--userland', default='',
                    help='Linux: run the installed game inside this root file system '
                         '(bwrap; "default" = build/linux-pkg-root, glibc 2.31)')
    a = ap.parse_args(argv)
    global KEEP, USERLAND
    KEEP = a.keep
    if a.userland:
        USERLAND = (ROOT / 'build/linux-pkg-root' if a.userland == 'default'
                    else Path(a.userland).absolute())
        if not (USERLAND / 'usr/bin/gcc').exists():
            print(f'no root file system at {USERLAND} (pc/tools/setup_linux_pkg_root.sh)')
            return 2
    for f in (CUE, TRACK1, TRACK2):
        if not f.exists():
            print(f'missing {f}: this test needs your own disc in disks/')
            return 2
    out = Path(a.out).absolute()
    out.mkdir(parents=True, exist_ok=True)
    only = set(filter(None, a.only.split(',')))
    want = lambda c: not only or c in only  # noqa: E731
    plats = ['linux', 'windows'] if a.platform == 'both' else [a.platform]
    templates = {}
    for name in plats:
        plat = Platform(name, out / name)
        plat.out.mkdir(parents=True, exist_ok=True)
        if not plat.package.exists():
            print(f'missing package {plat.package}')
            return 2
        templates[name] = plat
        elsewhere = plat.out / 'elsewhere-disc'
        if not elsewhere.exists():
            link_disc(elsewhere)
    t0 = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        ref_job = pool.submit(reference_traces, out)
        wine_job = pool.submit(init_wine, templates['windows']) if 'windows' in templates else None
        ref = ref_job.result()
        if wine_job:
            wine_job.result()
        futures = []
        for name, template in templates.items():
            def main_chain(R, windows=template.windows):
                # the shared install: these cases run in order on it
                main_install = case_disk(R, ref)
                if want('second-launch'):
                    case_second(R, ref, main_install)
                if not windows and want('python-equal'):
                    case_python_equal(R, main_install)
                if want('enhanced'):
                    case_enhanced(R, ref, main_install)
                if want('playthrough'):
                    case_playthrough(R, main_install, Path(a.dev_playthrough))
                if want('audit'):
                    case_audit(R, main_install)
                if want('packs'):
                    case_packs(R, main_install)
                if want('check'):
                    case_check(R, main_install)
                if want('damaged'):
                    case_damaged(R, ref, main_install)
            jobs = [('main', main_chain)]
            for case, fn in (('disc-flag', case_disc_flag), ('drop', case_drop), ('utf8-folder', case_utf8),
                             ('autodetect', case_autodetect), ('readonly', case_readonly)):
                if want(case):
                    jobs.append((case, lambda R, f=fn: f(R, ref)))
            for case in ERROR_CASES:
                if want(case) or (not only):
                    jobs.append((case, lambda R, c=case: case_errors(R, ref, {c})))
            for job, fn in jobs:
                futures.append(pool.submit(run_job, template, out, job, fn))
        results = [row for f in futures for row in f.result()]
    order = {n: i for i, n in enumerate(plats)}
    results.sort(key=lambda r: order[r['platform']])
    if not a.keep:
        for name in plats:
            make_writable(out / name)
            for p in (out / name).iterdir():
                if p.is_dir() and p.name not in ('wineprefix',):
                    shutil.rmtree(p, ignore_errors=True)
    report = out / 'report.json'
    report.write_text(json.dumps(results, indent=2) + '\n')
    failed = [r for r in results if not r['ok']]
    for r in results:
        print(f'[{r["platform"]}] {r["case"]:18s} {"PASS" if r["ok"] else "FAIL"}  {r["detail"][:200]}')
    print(f'{len(results) - len(failed)}/{len(results)} passed in {time.monotonic() - t0:.0f} s; '
          f'report {report}; shots {out}/shots')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
