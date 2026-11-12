# SPDX-License-Identifier: GPL-3.0-or-later
"""The Windows build's toolchain-level fixes.

  * pe_rebind_relocs: a COFF object's references to a global it defines are
    rebound to the symbol, so the link's definition wins as on ELF --
    executable under Wine (the object's own store reaches the absolute PS1
    address); negative control: without the rebind the object keeps its
    private copy.
  * audit_pe_binding: a weak definition next to another object's weak
    reference is reported (WEAK); the strong spelling cpu_cost.h now uses
    is clean.
  * win_dl: dladdr names a global function and not a static one (glibc
    over -rdynamic), dlsym finds a global -- under Wine.
  * safe_process: the Windows runner's timeout and output limit (the logic
    is plain Python and runs here).
  * audit_ps1_call_args --width: the WIN64-STACK / STACK-WIDTH judge.
  * package_windows: the whitelist refuses game data.
Tests that need MinGW or Wine skip without them (pc/tools/
setup_windows_toolchain.sh).
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import audit_pe_binding
import audit_ps1_call_args as aca
import package_windows
import pe_rebind_relocs
import safe_process

ROOT = Path(__file__).resolve().parents[2]


def mingw(tool):
    for cand in (f'x86_64-w64-mingw32-{tool}-posix', f'x86_64-w64-mingw32-{tool}'):
        if shutil.which(cand):
            return shutil.which(cand)
    for cand in (f'x86_64-w64-mingw32-{tool}-posix', f'x86_64-w64-mingw32-{tool}'):
        p = ROOT / 'build/mingw-root/usr/bin' / cand
        if p.is_file():
            return str(p)
    return None


GCC = mingw('gcc')
NM = mingw('nm')
WINE = shutil.which('wine')
PE_LINK = ['-Wl,--image-base=0x400000', '-Wl,--disable-dynamicbase',
           '-Wl,--disable-high-entropy-va']


def cc(args, cwd):
    r = subprocess.run([GCC, *args], cwd=cwd, capture_output=True, text=True,
                       env=dict(os.environ, TMPDIR=str(cwd)))
    if r.returncode:
        raise AssertionError(r.stderr)


# Wine's report of a crash (on stderr, which safe_process merges into the
# output): "wine: Unhandled page fault on write access to ... at address ...".
WINE_CRASH = ('Unhandled page fault', 'Unhandled exception', 'Unhandled stack overflow')
WINE_ATTEMPTS = 3


def wine(exe, cwd):
    """The output of a test program that exits 0 under Wine.  Under heavy
    load (24 test processes, each with a fresh Wine prefix) Wine has been
    seen to report an unhandled page fault while the process exits, after
    the program printed its correct output.  That run is not judged: the
    program runs again in a new prefix, up to WINE_ATTEMPTS times, and each
    discarded run is printed with its trace.  A run is accepted only with
    exit status 0 and no crash report, so a real fault (one that recurs)
    still fails, with the last run's output."""
    runs = []
    for _ in range(WINE_ATTEMPTS):
        # winedbg.exe disabled: a crash ends the run at once instead of
        # holding it in the debugger until the timeout
        r = safe_process.run([str(exe)], cwd=cwd, timeout=120,
                             env=dict(os.environ, WINEDEBUG='-all',
                                      WINEDLLOVERRIDES='winedbg.exe=d'))
        if r.reason is None and r.returncode == 0 and not any(c in r.stdout for c in WINE_CRASH):
            for i, bad in enumerate(runs):
                print(f'\n{Path(exe).name}: Wine run {i + 1} discarded (exit {bad.returncode}, '
                      f'{bad.reason}), a later run passed:\n{bad.stdout[-2000:]}', file=sys.stderr)
            return r.stdout
        runs.append(r)
    raise AssertionError(f'{Path(exe).name} under Wine: {WINE_ATTEMPTS} runs, none exited 0 '
                         f'without a crash report; last (exit {r.returncode}, {r.reason}):\n'
                         + r.stdout[-3000:])


REBIND_PROGRAM = r'''
#include <stdio.h>
#include <windows.h>
volatile unsigned char g_def[4] = {1, 2, 3, 4};   /* defined here AND absolute in the .syms */
static void bump(void) { g_def[2]++; }
int main(void)
{
    VirtualAlloc((void *)0x80000000, 0x200000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    *(volatile unsigned char *)0x80100002 = 40;
    bump();
    printf("%p %d %d\n", (void *)g_def, g_def[2], *(volatile unsigned char *)0x80100002);
    return 0;
}
'''


@unittest.skipUnless(GCC, 'MinGW-w64 gcc missing')
class RebindTests(unittest.TestCase):
    def build(self, d, rebind):
        (d / 't.c').write_text(REBIND_PROGRAM)
        (d / 'g.syms').write_text('g_def = 0x80100000;\n')
        cc(['-O2', '-ffunction-sections', '-fdata-sections', '-c', 't.c', '-o', 't.o'], d)
        planned = len(pe_rebind_relocs.plan((d / 't.o').read_bytes()))
        if rebind:
            self.assertEqual(pe_rebind_relocs.rebind(str(d / 't.o')), planned)
            self.assertEqual(pe_rebind_relocs.plan((d / 't.o').read_bytes()), [])  # idempotent
        cc(['t.o', '-Wl,--allow-multiple-definition', '-Wl,--just-symbols=g.syms', *PE_LINK,
            '-o', 't.exe'], d)
        return planned

    def test_rebind_plans_the_own_global(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            self.assertGreaterEqual(self.build(Path(t), rebind=True), 2)

    @unittest.skipUnless(WINE, 'Wine missing')
    def test_rebound_object_reaches_the_link_definition(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            self.build(Path(t), rebind=True)
            out = wine(Path(t) / 't.exe', t).split()
            self.assertEqual(out, ['0000000080100000', '41', '41'])

    @unittest.skipUnless(WINE, 'Wine missing')
    def test_negative_control_private_copy(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            self.build(Path(t), rebind=False)
            out = wine(Path(t) / 't.exe', t).split()
            self.assertNotEqual(out[0], '0000000080100000')
            self.assertEqual(out[1:], ['4', '40'])     # its copy moved, PS1 RAM did not


@unittest.skipUnless(GCC and NM, 'MinGW-w64 gcc/nm missing')
class BindingAuditTests(unittest.TestCase):
    def link(self, d, define_weak):
        (d / 'def.c').write_text(
            ('void hook(void) __attribute__((weak));\n' if define_weak else '') +
            'int hits;\nvoid hook(void) { hits++; }\n')
        (d / 'use.c').write_text(
            'extern void hook(void) __attribute__((weak));\nextern int hits;\n'
            'int main(void) { if (hook) hook(); return hits == 1 ? 0 : 3; }\n')
        os.makedirs(d / 'obj', exist_ok=True)
        cc(['-O2', '-c', 'use.c', '-o', 'obj/use.o'], d)
        cc(['-O2', '-c', 'def.c', '-o', 'obj/def.o'], d)
        cc(['obj/use.o', 'obj/def.o', '-o', 'x.exe'], d)
        return audit_pe_binding.main(['--exe', str(d / 'x.exe'), '--objs', str(d / 'obj'),
                                      '--nm', NM])

    def test_weak_definition_is_reported(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            self.assertEqual(self.link(Path(t), define_weak=True), 1)
            if WINE:   # and it is the real defect: the hook is never called
                r = safe_process.run([str(Path(t) / 'x.exe')], cwd=t, timeout=60,
                                     env=dict(os.environ, WINEDEBUG='-all'))
                self.assertEqual(r.returncode, 3)

    def test_strong_definition_is_clean(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            self.assertEqual(self.link(Path(t), define_weak=False), 0)


DL_PROGRAM = r'''
#include <stdio.h>
#include <dlfcn.h>
int dl_global(int x) { return x * 3 + 1; }
static int __attribute__((noinline)) dl_static(int x) { return x * 5 + 2; }
int dl_data = 7;
int main(int argc, char **argv)
{
    Dl_info a, b;
    int r = dladdr((char *)dl_global + 2, &a) && dladdr((char *)dl_static + 2, &b);
    printf("%d %s %d %s %d\n", r, a.dli_sname ? a.dli_sname : "-",
           a.dli_saddr == (void *)dl_global, b.dli_sname ? b.dli_sname : "-",
           dlsym(RTLD_DEFAULT, "dl_data") == (void *)&dl_data);
    return dl_static(argc) == 7 ? 0 : 1;
}
'''


@unittest.skipUnless(GCC and WINE, 'MinGW-w64 gcc or Wine missing')
class DlTests(unittest.TestCase):
    def test_dladdr_names_globals_not_statics(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            d = Path(t)
            (d / 'dl.c').write_text(DL_PROGRAM)
            cc(['-O1', '-static', '-I', str(ROOT / 'pc/include/win'), 'dl.c',
                str(ROOT / 'pc/hal/win/win_dl.c'), '-o', 'dl.exe'], d)
            self.assertEqual(wine(d / 'dl.exe', t).split(), ['1', 'dl_global', '1', '-', '1'])


class SafeProcessWindowsPathTests(unittest.TestCase):
    """_run_windows is the launcher's runner on a Windows Python."""

    def test_output_and_status(self):
        r = safe_process._run_windows([sys.executable, '-c', 'print("hi"); raise SystemExit(3)'],
                                      cwd=None, env=None, timeout=30, max_output=1 << 20,
                                      log_path=None, stop=None, watch_files=())
        self.assertEqual((r.returncode, r.stdout.strip(), r.reason), (3, 'hi', None))

    def test_timeout_and_output_limit(self):
        r = safe_process._run_windows([sys.executable, '-c', 'import time; time.sleep(30)'],
                                      cwd=None, env=None, timeout=.5, max_output=1 << 20,
                                      log_path=None, stop=None, watch_files=())
        self.assertEqual(r.reason, 'TIMEOUT')
        r = safe_process._run_windows([sys.executable, '-c', 'print("x" * 100000)'],
                                      cwd=None, env=None, timeout=30, max_output=1000,
                                      log_path=None, stop=None, watch_files=())
        self.assertEqual(r.reason, 'OUTPUT_LIMIT')


class WidthJudgeTests(unittest.TestCase):
    def test_stack_argument_width(self):
        defs = {'f': dict(params=['a', 'b', 'c', 'd', 'e', 'p', 'q'], read=[True] * 7,
                          ptypes=['int'] * 4 + ['RECT *', 'u8 *', 'long long'], file='f.c'),
                'SetDrawMode': dict(params=list('abcde'), read=[True] * 5,
                                    ptypes=['void *', 'int', 'int', 'int', 'RECT *'])}
        calls = [dict(func='g', callee='f', nargs=7, file='g.c', line=1,
                      atypes=['int'] * 4 + ['int', 'unsigned char *', 'unsigned int']),
                 dict(func='g', callee='f', nargs=7, file='g.c', line=2,
                      atypes=['int'] * 4 + ['RECT *', 'u8 *', 'unsigned long long']),
                 dict(func='g', callee='SetDrawMode', nargs=5, file='g.c', line=3,
                      atypes=['void *', 'int', 'int', 'int', 'int'])]
        rows = aca.judge_width(calls, defs.get)
        self.assertEqual([(r['line'], r['shape']) for r in rows],
                         [(1, 'WIN64-STACK'), (1, 'STACK-WIDTH')])   # line 2 matches; SetDrawMode is safe


class PackageTests(unittest.TestCase):
    def test_refuses_game_data(self):
        for bad in ('tokimeki-pc/pc/assets/raw/PSX.EXE', 'tokimeki-pc/disc.bin',
                    'tokimeki-pc/saves/card1.mcr', 'tokimeki-pc/x.tks', 'tokimeki-pc/SDL2.dll',
                    'tokimeki-pc/disk/x.cue', 'tokimeki-pc/assets/audio/TR02.DA',
                    'tokimeki-pc/data/raw/x.txt', 'tokimeki-pc/data/en/x.txt'):
            with self.assertRaises(ValueError):
                package_windows.check_no_game_data([bad])
        package_windows.check_no_game_data(['tokimeki-pc/tokimeki-pc.exe', 'tokimeki-pc/README.txt'])

    def test_licences_must_be_in_the_binary(self):
        """The zip carries no licence files, so the packager refuses a
        program that lacks one of the texts (and the project's LICENSE)."""
        texts = package_windows.licence_texts()
        with tempfile.TemporaryDirectory() as t:
            good = Path(t) / 'good.exe'
            good.write_bytes(b'MZ' + b'\n'.join(x.encode() for x in texts))
            package_windows.check_licences(good)
            bad = Path(t) / 'bad.exe'
            bad.write_bytes(b'MZ' + b'\n'.join(x.encode() for x in texts[:-1]))   # no LICENSE
            with self.assertRaises(ValueError):
                package_windows.check_licences(bad)

    @unittest.skipUnless((ROOT / 'pc/build-win/tokimeki-pc.exe').is_file() and
                         (ROOT / 'build/mingw-root/usr/bin/x86_64-w64-mingw32-objdump').is_file(),
                         'Windows build missing')
    def test_package_contents(self):
        """The exe and the README, nothing else (2026-11-07): no DLL, no
        Python launcher, no licence files (they are in the exe), no manifest."""
        import zipfile
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as t:
            out, _ = package_windows.build(Path(t) / 'p.zip', allow_dirty=True)
            # the GPL corresponding source is written next to the zip
            src = Path(t) / 'p-source.tar.gz'
            self.assertTrue(src.is_file())
            names = sorted(zipfile.ZipFile(out).namelist())
            self.assertEqual(names, sorted(
                [f'tokimeki-pc/{package_windows.PLAYER_EXE}', 'tokimeki-pc/README.txt']))
            # the README points at the licences inside the program
            readme = zipfile.ZipFile(out).read('tokimeki-pc/README.txt').decode()
            self.assertIn('Licences: Settings > About / licences, or run with --licenses.', readme)
            # deterministic
            again, _ = package_windows.build(Path(t) / 'p.zip', force=True, source=False)
            self.assertEqual(Path(out).read_bytes(), Path(again).read_bytes())


if __name__ == '__main__':
    unittest.main()
