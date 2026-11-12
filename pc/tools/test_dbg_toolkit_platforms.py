# SPDX-License-Identifier: GPL-3.0-or-later
"""The debug toolkit end to end on Linux and, under Wine, on Windows
(pc/hal/dbg_cmds.c, dbg_console.c, dbg_state.c over pc/hal/dbg_platform.h;
docs/pc-port/DEBUG-TOOLS.md).  Until 2026-11-02 the Windows build answered
every one of these with "not available"; each test runs on both builds:

* the live console on TCP: info, pause, step, peek, sym (a static by its
  file), poke, save, load, shot, qlist, quit -- one reply line each;
* the console on stdin (a reader thread on Windows) and on a unix socket
  (AF_UNIX: Linux, and Windows 10+ / Wine where the host has it);
* the toolkit's own F5 / F9 with PC_DBG_SLOT (key events through
  TOKI_INPUT_SCRIPT, the path a typed key takes);
* PC_SAVESTATE / PC_LOADSTATE / PC_DBG_CMDS: a state saved by one process
  loads into a fresh one;
* the inventory (PC_STATE_INVENTORY): every data object of the
  executable's writable sections is classified, the project's statics and
  globals are saved unless a KEEP rule names them, nothing unnamed holds
  live data (the unnamed bytes are the same at two moments);
* a state of the other platform is refused (another build), also with
  PC_LOADSTATE_FORCE=1 (its records do not describe this binary).

Needs the built port(s) and pc/assets; each build's tests are skipped when
it (or Wine) is missing.
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savestate_roundtrip as rt  # noqa: E402
import savestate_tool as sst  # noqa: E402

ROOT = Path(rt.ROOT)
LINUX = ROOT / 'pc/build/tokimeki-pc'
WINDOWS = ROOT / 'pc/build-win/tokimeki-pc.exe'
ASSETS = ROOT / 'pc/assets'
WINE = shutil.which('wine')
HAVE_ASSETS = (ASSETS / 'raw/PSX.EXE').is_file()


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def base_env(extra):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PC_', 'SCENE', 'TOKI_', 'AUTOPLAY', 'GP0_', 'MDFN_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1',
               TOKI_INPUT_DEVICE='pad', WINEDEBUG='-all')
    env.update(extra)
    return env


def run_logged(binary, cwd, env, timeout, stdin_text=None):
    """run BINARY with stdin and stdout/stderr through files, never pipes:
    under Wine the helper processes the first `wine` of a session starts
    inherit a pipe and keep it open after the game has exited, so a reader
    waiting for end-of-file hangs (and other Wine runs keep them alive)
    -> (returncode, output)"""
    with tempfile.TemporaryDirectory(dir=cwd) as d:
        inp, out = os.path.join(d, 'in'), os.path.join(d, 'out')
        Path(inp).write_text(stdin_text or '')
        with open(inp, 'rb') as fi, open(out, 'wb') as fo:
            rc = subprocess.run(rt.command(binary) + [str(ASSETS)], cwd=cwd, env=env, stdin=fi,
                                stdout=fo, stderr=subprocess.STDOUT, timeout=timeout).returncode
        return rc, Path(out).read_text(errors='replace')


class Toolkit:
    binary = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='dbg-toolkit-', dir=ROOT / 'build')
        os.makedirs(os.path.join(cls.tmp, 'pc'))
        os.symlink(ASSETS, os.path.join(cls.tmp, 'pc/assets'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_to_end(self, extra, timeout=600, stdin_text=None, binary=None):
        return run_logged(binary or str(self.binary), self.tmp, base_env(extra), timeout, stdin_text)

    def converse(self, connect, lines, extra):
        """start the port with a console, send LINES one at a time; -> replies, log"""
        log = tempfile.NamedTemporaryFile(dir=self.tmp, suffix='.log', delete=False)
        proc = subprocess.Popen(rt.command(str(self.binary)) + [str(ASSETS)], cwd=self.tmp,
                                env=base_env(extra), stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT)
        log.close()
        replies = []
        try:
            deadline = time.time() + 120
            sock = None
            while sock is None:
                try:
                    sock = connect()
                except OSError:
                    if time.time() > deadline or proc.poll() is not None:
                        raise
                    time.sleep(0.2)
            f = sock.makefile('rb')
            sock.settimeout(300)
            for line in lines:
                sock.sendall(line.encode() + b'\n')
                reply = f.readline().decode('utf-8', 'replace').rstrip('\n')
                replies.append(reply)
                if line.startswith('quit'):
                    break
            sock.close()
            proc.wait(timeout=120)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        return replies, Path(log.name).read_text(errors='replace'), proc.returncode

    def console_session(self, connect, spec):
        state = os.path.join(self.tmp, f'console-{spec.replace(":", "_").replace("/", "_")[-20:]}.tks')
        shot = os.path.join(self.tmp, 'console.ppm')
        lines = ['info', 'pause', 'step 30', 'peek now_step 1', 'sym memcard.c:g_card',
                 'poke cd_audio.c:g_played 7', 'peek cd_audio.c:g_played 4',
                 f'save {state}', 'step 10', f'load {state}', 'peek cd_audio.c:g_played 4',
                 f'shot {shot}', 'qlist', 'nonsense', 'quit 0']
        replies, log, rc = self.converse(connect, lines, {'PC_DBG_CONSOLE': spec,
                                                         'TOKI_SAVE_DIR': os.path.join(self.tmp, 'saves')})
        self.assertEqual(rc, 0, log[-3000:])
        self.assertEqual(len(replies), len(lines), (replies, log[-3000:]))
        r = dict(zip(lines, replies))
        self.assertRegex(r['info'], r'^ok frame=\d+ vblanks=\d+ step=')
        self.assertTrue(r['pause'].startswith('ok paused'))
        self.assertTrue(r['step 30'].startswith('ok'))
        self.assertRegex(r['peek now_step 1'], r'^ok 0x[0-9a-f]+ 1 ')
        size = int(r['sym memcard.c:g_card'].split()[-1])
        self.assertGreaterEqual(size, 128 * 1024, r['sym memcard.c:g_card'])
        self.assertTrue(r[f'save {state}'].startswith('ok saved'), r[f'save {state}'])
        self.assertTrue(r[f'load {state}'].startswith('ok loaded'), r[f'load {state}'])
        peeks = [rep for line, rep in zip(lines, replies) if line == 'peek cd_audio.c:g_played 4']
        self.assertTrue(all(p.endswith('u=7 s=7') for p in peeks), peeks)
        self.assertTrue(r[f'shot {shot}'].startswith('ok'))
        self.assertEqual(Path(shot).read_bytes()[:2], b'P6')
        self.assertTrue(r['qlist'].startswith('ok Quicksave slot'))
        self.assertTrue(r['nonsense'].startswith('err unknown command'))
        st = sst.read_state(state)
        self.assertEqual(st['format'], 2)
        self.assertIn('g_vram', st['records'])
        return replies

    def test_console_tcp(self):
        port = free_port()
        self.console_session(lambda: socket.create_connection(('127.0.0.1', port), timeout=1),
                             str(port))

    def test_console_unix(self):
        path = os.path.join(self.tmp, 'console.sock')
        if rt.is_windows(self.binary):
            # AF_UNIX is Windows 10 1803+; Wine 10 has none (WSAEAFNOSUPPORT):
            # then the console must say so and point to the TCP console
            rc, out = self.run_to_end({'PC_DBG_CONSOLE': f'unix:{path}', 'PC_TRACE_FRAMES': '200',
                                       'PC_TRACE': os.path.join(self.tmp, 't')})
            self.assertEqual(rc, 0, out[-3000:])
            if 'this system has no unix sockets' in out:
                self.assertIn('use PC_DBG_CONSOLE=PORT', out)
                self.skipTest('this Windows host (Wine) has no AF_UNIX; the console said so')

        def connect():
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        self.console_session(connect, f'unix:{path}')

    def test_console_stdin(self):
        rc, out = self.run_to_end({'PC_DBG_CONSOLE': 'stdin'}, stdin_text='pause\ninfo\nstep 5\n'
                                  'peek now_step 1\nquit 0\n')
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn('[DBG] console on stdin', out)
        self.assertRegex(out, r'\[DBG\] ok frame=\d+ vblanks=\d+')
        self.assertRegex(out, r'\[DBG\] ok 0x[0-9a-f]+ 1 \w+ u=\d+')
        self.assertIn('[DBG] quit (code 0)', out)

    def test_f5_f9_with_the_toolkit_slot(self):
        slot = os.path.join(self.tmp, 'slot.tks')
        keys = os.path.join(self.tmp, 'keys')
        # after the boot movie (one long main-loop frame, in which both keys
        # would act at the same boundary)
        Path(keys).write_text('1200 key F5\n1500 key F9\n')
        rc, out = self.run_to_end({'PC_DBG_SLOT': slot, 'TOKI_INPUT_SCRIPT': keys,
                                   'PC_TRACE_FRAMES': '900', 'PC_TRACE': os.path.join(self.tmp, 't')})
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn(f'ok saved {slot}', out)
        self.assertIn(f'ok loaded {slot}', out)
        self.assertEqual(sst.read_state(slot)['format'], 2)

    def test_savestate_loadstate_scripts(self):
        a = os.path.join(self.tmp, 'a.tks')
        cmds = os.path.join(self.tmp, 'cmds')
        Path(cmds).write_text('at 250 peek frame_counter 4\nafter 5 quit 0\n')
        rc, out = self.run_to_end({'PC_SAVESTATE': f'200:{a}', 'PC_DBG_CMDS': cmds})
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn(f'PC_SAVESTATE {a}: frame 200', out)
        rc, out = self.run_to_end({'PC_LOADSTATE': a, 'PC_DBG_CMDS': cmds})
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn(f'PC_LOADSTATE {a}: frame 200', out)
        # the loaded process starts at frame 200: line 1 runs at its 250
        self.assertRegex(out, r'cmds line 1 at frame 250: peek frame_counter')

    def test_stripped_executable_says_not_available(self):
        """no symbol table, no states: the player is told, nothing fails
        silently, and no autosave is armed"""
        exe = str(self.binary)
        stripped = os.path.join(self.tmp, 'stripped' + ('.exe' if exe.endswith('.exe') else ''))
        strip = (str(ROOT / 'build/mingw-root/usr/bin/x86_64-w64-mingw32-strip')
                 if exe.endswith('.exe') else 'strip')
        if exe.endswith('.exe') and not os.path.exists(strip):
            strip = shutil.which('x86_64-w64-mingw32-strip') or strip
        shutil.copy(exe, stripped)
        subprocess.run([strip, '--strip-all', stripped], check=True)
        cmds = os.path.join(self.tmp, 'cmds-stripped')
        state = os.path.join(self.tmp, 'stripped.tks')
        Path(cmds).write_text(f'at 200 qsave 3\nafter 1 save {state}\nafter 1 qload 3\n'
                              'after 1 quit 0\n')
        rc, out = self.run_to_end({'PC_DBG_CMDS': cmds,
                                   'TOKI_SAVE_DIR': os.path.join(self.tmp, 'saves-stripped')},
                                  binary=stripped)
        self.assertEqual(rc, 0, out[-3000:])
        self.assertEqual(out.count('[QUICKSAVE] Save states are not available in this build'), 2, out[-3000:])
        self.assertIn('save states are not available: cannot read the executable', out)
        self.assertRegex(out, r'err save \S+: cannot read the executable.s symbols')
        self.assertNotIn('slot 3 saved', out)
        self.assertFalse(os.path.exists(state))

    def test_inventory(self):
        inv1, inv2 = os.path.join(self.tmp, 'inv1.tsv'), os.path.join(self.tmp, 'inv2.tsv')
        # (frames past the boot movie, one long main-loop frame)
        for inv, frame in ((inv1, 300), (inv2, 900)):
            rc, out = self.run_to_end({'PC_SAVESTATE': f'{frame}:{inv}.tks', 'PC_STATE_INVENTORY': inv,
                                       'PC_TRACE_FRAMES': str(frame + 5),
                                       'PC_TRACE': os.path.join(self.tmp, 't')})
            self.assertEqual(rc, 0, out[-3000:])
        rows = [l.split('\t') for l in Path(inv1).read_text().splitlines() if not l.startswith('#')]
        cls_of = {}
        for c, key, addr, size, *_ in rows:
            cls_of.setdefault(key, set()).add(c)
        # the machine's device models are saved, the session's objects are not
        for key in ('g_vram', 'spu.cpp:_ZN3SPUL5s_ramE', 'memcard.c:g_card', 'overlay.c:g_loaded'):
            self.assertIn('saved', cls_of.get(key, ()), key)
        for key in ('quicksave.c:g_slot', 'dbg_boundary_sp', 'rewind.c:g_thread'):
            self.assertEqual(cls_of.get(key), {'keep-file'}, key)
        self.assertEqual(cls_of.get('system.c:g_window'), {'keep-sym'})
        self.assertIn('partial', cls_of['spu.cpp:_ZN3SPUL7s_stateE'])
        # the state's records are exactly the saved objects
        st = sst.read_state(inv1 + '.tks')
        saved = {k for k, cs in cls_of.items() if cs & {'saved', 'partial'}}
        self.assertEqual({k.split('#')[0] for k in st['records']}, saved)
        # nothing unnamed is live: the unnamed bytes are identical at two moments
        h1 = [l for l in Path(inv1).read_text().splitlines() if l.startswith('# unnamed')]
        h2 = [l for l in Path(inv2).read_text().splitlines() if l.startswith('# unnamed')]
        self.assertEqual(h1, h2)


def runnable(binary):
    if not HAVE_ASSETS or not binary.is_file():
        return False
    return not str(binary).endswith('.exe') or bool(WINE)


@unittest.skipUnless(runnable(LINUX), 'needs the Linux build and pc/assets')
class ToolkitLinux(Toolkit, unittest.TestCase):
    binary = LINUX


@unittest.skipUnless(runnable(WINDOWS), 'needs the Windows build, Wine and pc/assets')
class ToolkitWindows(Toolkit, unittest.TestCase):
    binary = WINDOWS


@unittest.skipUnless(runnable(LINUX) and runnable(WINDOWS), 'needs both builds, Wine and pc/assets')
class CrossPlatform(unittest.TestCase):
    """a state names its platform and build: the other build refuses it"""

    def test_each_refuses_the_others_state(self):
        tmp = tempfile.mkdtemp(prefix='dbg-cross-', dir=ROOT / 'build')
        try:
            os.makedirs(os.path.join(tmp, 'pc'))
            os.symlink(ASSETS, os.path.join(tmp, 'pc/assets'))
            states = {}
            for name, binary in (('linux', LINUX), ('windows', WINDOWS)):
                states[name] = os.path.join(tmp, f'{name}.tks')
                rc, out = run_logged(str(binary), tmp,
                                     base_env({'PC_SAVESTATE': f'200:{states[name]}',
                                               'PC_TRACE_FRAMES': '202',
                                               'PC_TRACE': os.path.join(tmp, 't')}), 600)
                self.assertEqual(rc, 0, out[-2000:])
                self.assertEqual(sst.read_state(states[name])['info']['platform'],
                                 f'{name}-x86_64')
            for name, binary, other in (('linux', LINUX, 'windows'), ('windows', WINDOWS, 'linux')):
                for force in ('0', '1'):
                    rc, out = run_logged(str(binary), tmp,
                                         base_env({'PC_LOADSTATE': states[other],
                                                   'PC_LOADSTATE_FORCE': force}), 600)
                    self.assertEqual(rc, 2, out[-2000:])
                    if force == '0':
                        self.assertIn('was saved by another build', out)
                    else:
                        self.assertIn('does not describe this binary', out)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
