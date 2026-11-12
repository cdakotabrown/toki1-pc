# SPDX-License-Identifier: GPL-3.0-or-later
"""Texture dumping and replacement packs (pc/hal/gpu_texrep.h) through the
real native GPU.

pc/tests/gpu_texrep_harness.c includes pc/hal/gpu.c and links gpu_texrep.c,
gpu_hires.c and profile.c; every mode is a separate process.  The core
property is that the native VRAM the game sees (and everything StoreImage
returns) is the same with dumping on, with a pack and with neither; the rest
pin the dump (what is written, its names and pixels, what is not written)
and the replacement (sub-pixels from the pack, see-through and
semi-transparent texels, palette swaps and overdrawn texels falling back to
1x, a frame-buffer picture replaced), and the profile gate.

Negative controls: each MUTATIONS entry rebuilds the harness from a copy of
the sources with one mechanism broken and requires the test that guards it
to fail.
"""
import contextlib
import io
import os
from pathlib import Path
import re
import shlex
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from safe_process import run  # noqa: E402
import texture_pack_tool as tpt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ('pc/hal/gpu.c', 'pc/hal/gpu_texrep.c', 'pc/hal/gpu_hires.c', 'pc/hal/profile.c')

# (name, file, old, new, test that must then fail)
MUTATIONS = (
    ('LoadImage does not register sources', 'pc/hal/gpu.c',
     '    if (g_texrep_on)\n        gpu_texrep_upload(rect->x & 1023',
     '    if (0)\n        gpu_texrep_upload(rect->x & 1023', 'test_dump'),
    ('a single frame is enough to dump', 'pc/hal/gpu_texrep.c',
     '            g_pend_n++;\n            return 0;',
     '            g_pend_n++;\n            return 1;', 'test_dump'),
    ('MDEC columns are not joined', 'pc/hal/gpu_texrep.c',
     'if (s && s->w && w <= 32 && h >= 64', 'if (0 && s && s->w && w <= 32 && h >= 64',
     'test_dump'),
    ('overwritten texels stay owned', 'pc/hal/gpu_texrep.c',
     'if (id && g_oval[at] != g_vram[y][x]) return 0;', '(void)0;', 'test_replace'),
    ('the palette is not part of the key', 'pc/hal/gpu_texrep.c',
     'g_prim.clut = g_prim.fmt < 2 ? clut_hash(clut, g_prim.fmt) : 0;',
     'g_prim.clut = g_prim.fmt < 2 ? clut_hash(PAL_CLUT_FIXED, g_prim.fmt) : 0;',
     'test_replace'),
    ('sprites keep the 1x mirror', 'pc/hal/gpu.c',
     'if (HIRES_ON() && g_texrep_replace && gpu_texrep_prim_begin(clut, tpage)) {',
     'if (0) {', 'test_replace'),
    ('the native sprite pass overwrites the shadow', 'pc/hal/gpu.c',
     '        g_hires.in_poly = 1;\n        shadow = 1;',
     '        shadow = 1;', 'test_replace'),
    ('polygons ignore the pack', 'pc/hal/gpu.c',
     'g_texrep_prim = g_texrep_replace && mode->textured &&',
     'g_texrep_prim = 0 && mode->textured &&', 'test_replace'),
    ('the shadow pass writes the native VRAM', 'pc/hal/gpu.c',
     '                        px = gpu_texrep_alpha_mix(row[i], px, c & 255u);\n'
     '                    row[i] = px | mask_set;',
     '                        px = gpu_texrep_alpha_mix(row[i], px, c & 255u);\n'
     '                    row[i] = px | mask_set;\n                    g_vram[dy][dx] ^= 1;',
     'test_native_same'),
    ('the shadow fast path writes the native VRAM', 'pc/hal/gpu.c',
     '                        row[i] = px | mask_set;\n                    }\n'
     '                }\n                continue;',
     '                        row[i] = px | mask_set;\n                        g_vram[dy][dx] ^= 1;\n'
     '                    }\n                }\n                continue;', 'test_native_same'),
    ('the fast path ignores the replacement', 'pc/hal/gpu.c',
     '                        u16 px = src[i];', '                        u16 px = ntex & 0x7fff;',
     'test_replace'),
    ('alpha decides the semi-transparency bit again', 'pc/hal/gpu.c',
     '                 (stp & 0x8000u));', '                 ((c & 255u) < 192u ? 0x8000u : 0u));',
     'test_replace'),
    ('a pack does not raise the scale', 'pc/hal/gpu_hires.c',
     '    if (pack > n) {', '    if (0) {', 'test_config'),
    ('parity turns textures on', 'pc/hal/profile.c',
     '{ "GPU_TEXTURES",       0, 1,', '{ "GPU_TEXTURES",       1, 1,', 'test_config'),
)


def build(folder, mutation=None):
    folder = Path(folder)
    paths = {s: ROOT / s for s in SOURCES}
    if mutation:
        _, path, old, new, _ = mutation
        text = (ROOT / path).read_text()
        assert text.count(old) == 1, f'mutation anchor not unique in {path}: {old!r}'
        text = text.replace(old, new)
        if 'PAL_CLUT_FIXED' in new:    # any fixed CLUT: here the first one
            text = text.replace('PAL_CLUT_FIXED', '((480 << 6) | 0)')
        copy = folder / Path(path).name
        copy.write_text(text)
        paths[path] = copy
    flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2']).require_ok().stdout)
    common = ['-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1', '-fno-pie',
              '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
              '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', '-include',
              'pc/include/game/types.h', *flags]
    objects = []
    units = (('harness', str(ROOT / 'pc/tests/gpu_texrep_harness.c')),
             ('texrep', str(paths['pc/hal/gpu_texrep.c'])),
             ('hires', str(paths['pc/hal/gpu_hires.c'])),
             ('profile', str(paths['pc/hal/profile.c'])),
             ('queue', 'pc/hal/gpu_queue.c'), ('texture', 'pc/hal/gpu_texture.c'),
             ('vram', 'pc/hal/gpu_vram.c'), ('ptr', 'pc/hal/ptr_validate.c'))
    for name, src in units:
        obj = folder / (name + '.o')
        # gpu_texrep.c includes ../3rd/stb relative to itself: a mutated copy
        # needs the tree's include path
        run(['cc', *common, '-I' + str(ROOT / 'pc/hal'),
             '-DGPU_SOURCE="' + str(paths['pc/hal/gpu.c'].resolve()) + '"',
             '-DOT_FIXTURE="' + str(ROOT / 'pc/tests/gpu_ot_harness.c') + '"',
             '-c', src, '-o', str(obj)], cwd=ROOT, timeout=120).require_ok()
        objects.append(str(obj))
    binary = folder / 'texrep'
    run(['cc', '-no-pie', *objects, '-Wl,--gc-sections', '-lm', '-o', str(binary)],
        cwd=ROOT, timeout=60).require_ok()
    return binary


def env_for(profile='enhanced', **kv):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('TOKI_', 'GPU_', 'PC_'))}
    env['TOKI_CONFIG'] = '/dev/null'
    if profile:
        env['TOKI_PROFILE'] = profile
    env.update({k: str(v) for k, v in kv.items()})
    return env


def tool(*argv):
    """texture_pack_tool.main, quietly; returns its exit status."""
    with contextlib.redirect_stdout(io.StringIO()):
        return tpt.main(list(argv))


def run_mode(binary, mode, env):
    """stdout and stderr arrive merged (safe_process)."""
    return run([str(binary), mode], env=env, timeout=120)


def line(r, prefix):
    m = re.search(r'^' + prefix + r'.*$', r.stdout, re.M)
    if not m:
        raise AssertionError(f'no {prefix!r} line in:\n{r.stdout[-2000:]}')
    return m.group(0)


# The harness's palette P and sheet A (gpu_texrep_harness.c make_sheet_a)
def pal_p(i):
    return 0 if i == 0 else 0x801f if i == 1 else (0x0400 * (i & 7) + 0x20 * i + 3) & 0xffff


def rgba_of(p):
    if p == 0:
        return (0, 0, 0, 0)
    r, g, b = p & 31, (p >> 5) & 31, (p >> 10) & 31
    ex = lambda c: (c << 3) | (c >> 2)  # noqa: E731
    return (ex(r), ex(g), ex(b), 255)   # alpha is coverage only: STP is not in the PNG


def rep_pixel(x, y):
    """The test pack's sheet-A replacement pixel (the harness's rep_pixel)."""
    a = 0 if x // 2 == 3 else 128 if y // 2 == 5 else 255
    return ((x * 7) & 255, (y * 13) & 255, ((x + y) * 5) & 255, a)


class TexrepTests(unittest.TestCase):
    binary = None

    @classmethod
    def setUpClass(cls):
        if cls.binary is None:
            cls.build_dir = tempfile.TemporaryDirectory()
            cls.addClassCleanup(cls.build_dir.cleanup)
            cls.binary = build(cls.build_dir.name)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def tmp(self, name):
        d = Path(self.temp.name) / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def names(self):
        r = run_mode(self.binary, 'names', env_for())
        r.require_ok()
        return dict(re.findall(r'^([A-Z]) \S+ (\S+)$', r.stdout, re.M))

    def make_pack(self, folder):
        names = self.names()
        rows = [b''.join(bytes(rep_pixel(x, y)) for x in range(128)) for y in range(32)]
        tpt.write_png(str(folder / (names['A'] + '.png')), 128, 32, rows)
        rows = [b''.join(bytes((255, 0, 0, 255) if x % 2 == 0 else (0, 255, 0, 255))
                         for x in range(64)) for y in range(64)]
        tpt.write_png(str(folder / (names['W'] + '.png')), 64, 64, rows)
        return names

    def test_native_same(self):
        """The native VRAM and StoreImage do not depend on dumping or a pack."""
        dump_root = self.tmp('root')
        off = run_mode(self.binary, 'native', env_for(GPU_TEXTURES=0))
        off.require_ok()
        dump = run_mode(self.binary, 'native', env_for(
            GPU_TEXTURES=1, TOKI_TEXTURE_DUMP=1, TOKI_TEXTURE_DIR=dump_root))
        dump.require_ok()
        self.assertEqual(line(off, 'vram='), line(dump, 'vram='))
        self.assertRegex(line(dump, 'dumped='), r'dumped=[1-9]')
        dumped = dump_root / 'dump' / 'SLPS-00065'
        pack = dump_root / 'packs' / 'test'
        self.assertEqual(tool('make-test-pack', str(dumped), str(pack), '--scale', '3'), 0)
        packed = run_mode(self.binary, 'native', env_for(
            GPU_TEXTURES=1, TOKI_TEXTURE_PACK='test', TOKI_TEXTURE_DIR=dump_root))
        packed.require_ok()
        self.assertEqual(line(off, 'vram='), line(packed, 'vram='))
        self.assertRegex(line(packed, 'dumped='), r'replaced=[1-9]')

    def test_dump(self):
        root = self.tmp('root')
        r = run_mode(self.binary, 'dump', env_for(TOKI_TEXTURE_DUMP=1, TOKI_TEXTURE_DIR=root))
        r.require_ok()
        expect = dict(re.findall(r'^([A-Z]) \S+ (\S+)$', r.stdout, re.M))
        folder = root / 'dump' / 'SLPS-00065'
        index = tpt.read_index(str(folder))
        files = [row['file'] for row in index]
        self.assertEqual(len(files), len(set(files)), 'a texture was dumped twice')
        kinds = {row['file']: row['kind'] for row in index}
        self.assertEqual(kinds.get(expect['A'] + '.png'), 'texture')
        self.assertEqual(kinds.get(expect['S'] + '.png'), 'texture', 'columns not one picture')
        self.assertEqual(kinds.get(expect['F'] + '.png'), 'write')
        self.assertNotIn(expect['B'] + '.png', kinds, 'dumped after one frame')
        self.assertNotIn(expect['G'] + '.png', kinds, 'a glyph under the floor')
        self.assertEqual(len(files), 3, files)
        # sheet A's pixels: palette P applied, 0000h see-through, STP texels opaque
        w, h, rows = tpt.read_png(str(folder / (expect['A'] + '.png')))
        self.assertEqual((w, h), (64, 16))
        for ty in range(16):
            for tx in range(64):
                got = tuple(rows[ty][tx * 4:tx * 4 + 4])
                want = rgba_of(pal_p((tx + ty) & 15))
                if want[3] == 0:
                    self.assertEqual(got[3], 0)
                else:
                    self.assertEqual(got, want, (tx, ty))
        # a second session adds nothing (index.csv remembers)
        r = run_mode(self.binary, 'dump', env_for(TOKI_TEXTURE_DUMP=1, TOKI_TEXTURE_DIR=root))
        r.require_ok()
        self.assertEqual(len(tpt.read_index(str(folder))), 3)
        # the floor is a setting
        low = self.tmp('low')
        r = run_mode(self.binary, 'dump', env_for(TOKI_TEXTURE_DUMP=1, TOKI_TEXTURE_DIR=low,
                                                  TOKI_TEXTURE_DUMP_MIN=0))
        r.require_ok()
        self.assertIn(expect['G'] + '.png',
                      {row['file'] for row in tpt.read_index(str(low / 'dump' / 'SLPS-00065'))})

    def test_replace(self):
        pack = self.tmp('pack')
        self.make_pack(pack)
        self.assertEqual(tool('validate', str(pack)), 0)
        r = run_mode(self.binary, 'replace', env_for(TOKI_TEXTURE_PACK=str(pack)))
        self.assertEqual(r.returncode, 0, r.stdout[-3000:])
        plain = run_mode(self.binary, 'replace-native', env_for(GPU_TEXTURES=0))
        plain.require_ok()
        self.assertEqual(line(r, 'vram=').split()[0], line(plain, 'vram=').split()[0],
                         'the pack changed the native VRAM')

    def test_config(self):
        pack = self.tmp('pack')
        self.make_pack(pack)

        def cfg(profile, **kv):
            r = run_mode(self.binary, 'config', env_for(profile, **kv))
            r.require_ok()
            return line(r, 'textures=')

        dump = self.tmp('d')
        self.assertEqual(cfg('parity', TOKI_TEXTURE_DUMP=1, TOKI_TEXTURE_DIR=dump),
                         'textures=0 replace=0 scale=1')
        self.assertEqual(cfg('enhanced'), 'textures=0 replace=0 scale=1')
        self.assertEqual(cfg('enhanced', TOKI_TEXTURE_DUMP=1, TOKI_TEXTURE_DIR=dump),
                         'textures=1 replace=0 scale=1')
        self.assertEqual(cfg('enhanced', TOKI_TEXTURE_PACK=pack), 'textures=1 replace=1 scale=2')
        self.assertEqual(cfg('enhanced', TOKI_TEXTURE_PACK=pack, TOKI_RES_SCALE=4),
                         'textures=1 replace=1 scale=4')
        # a 3x file makes a 3x pack
        names = self.names()
        p3 = self.tmp('pack3')
        rows = [b'\x10\x20\x30\xff' * 192] * 48
        tpt.write_png(str(p3 / (names['A'] + '.png')), 192, 48, rows)
        self.assertEqual(cfg('enhanced', TOKI_TEXTURE_PACK=p3), 'textures=1 replace=1 scale=3')
        self.assertEqual(cfg('parity', TOKI_TEXTURE_PACK=p3), 'textures=0 replace=0 scale=1')


class NegativeControls(unittest.TestCase):
    def test_each_mutation_is_caught(self):
        for mutation in MUTATIONS:
            name, _, _, _, test = mutation
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                binary = build(tmp, mutation)

                class Mutated(TexrepTests):
                    pass
                Mutated.binary = binary
                result = unittest.TestResult()
                Mutated(test).run(result)
                self.assertTrue(result.failures or result.errors,
                                f'mutation "{name}" was not caught by {test}')


if __name__ == '__main__':
    unittest.main()
