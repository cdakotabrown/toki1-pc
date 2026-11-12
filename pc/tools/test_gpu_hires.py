# SPDX-License-Identifier: GPL-3.0-or-later
"""Internal resolution scale (pc/hal/gpu_hires.h) through the real native GPU.

pc/tests/gpu_hires_harness.c includes pc/hal/gpu.c and links gpu_hires.c and
profile.c; every mode is a separate process.  The core property is that the
native VRAM the game sees (and everything StoreImage returns) is the same at
every scale; the rest pin the shadow's rules: 2D mirrored exactly, 1:1
textures unshifted, edges finer, replaced pixels replaced, moves carried,
unmirrored writes shown at 1x.

Negative controls: each MUTATIONS entry rebuilds the harness from a copy of
the sources with one mechanism broken and requires the test that guards it
to fail.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ('pc/hal/gpu.c', 'pc/hal/gpu_hires.c', 'pc/hal/profile.c')

# (name, file, old, new, mode that must then fail)
MUTATIONS = (
    ('shadow pass fills the texture cache', 'pc/hal/gpu.c',
     'u16 texel = texel_peek_windowed((u8)(v.u >> 24), (u8)(v.v >> 24),',
     'u16 texel = texel_fetch_windowed((u8)(v.u >> 24), (u8)(v.v >> 24),',
     'texcache'),
    ('1:1 textures sampled with the native bias', 'pc/hal/gpu.c',
     'bias = hires ? (1u << 11) / (u32)g_hires.scale : (1u << 11);',
     'bias = (1u << 11);', 'poly1to1'),
    ('sprites not mirrored', 'pc/hal/gpu.c',
     'if (HIRES_ON()) hires_native_stored(x, y, old, front, blend, abr);',
     '(void)old; (void)front;', 'sprite'),
    ('blocks claimed only where a texel is written', 'pc/hal/gpu.c',
     '            HIRES_SPAN_CLAIM();\n            if (texel) {',
     '            if (texel) {\n                HIRES_SPAN_CLAIM();', 'coverage'),
    ('shadow polygons at 1x', 'pc/hal/gpu.c',
     'hv[i].x = input[i].x * g_hires.scale;', 'hv[i].x = input[i].x;', 'edges'),
    ('fills not mirrored', 'pc/hal/gpu.c',
     'static void hires_mirror_fill(u32 position, u32 size)\n{\n    if (!HIRES_ON()) return;',
     'static void hires_mirror_fill(u32 position, u32 size)\n{\n    return;', 'fill'),
    ('LoadImage not mirrored', 'pc/hal/gpu.c',
     '    /* gpu_vram_load\'s wrapped rect, upscaled into the shadow */\n    if (HIRES_ON())',
     '    if (0)', 'fill'),
    ('MoveImage drops the high-resolution blocks', 'pc/hal/gpu_hires.c',
     'if (!overlap && g_hires.ref[ay * 1024 + ax] == g_vram[ay][ax]) {',
     'if (0) {', 'move'),
    ('no resync before presenting', 'pc/hal/gpu_hires.c',
     'if (g_hires.scale <= 1) return 0;\n    for (j = 0; j < h; j++) {',
     'return 0;\n    for (j = 0; j < h; j++) {', 'resync'),
    ('parity turns the scale on', 'pc/hal/profile.c',
     '{ "GPU_HIRES",          0, 1,', '{ "GPU_HIRES",          1, 1,', 'config'),
)


def build(folder, mutation=None):
    """Compile the harness from the tree, or from copies with `mutation`."""
    folder = Path(folder)
    paths = {s: ROOT / s for s in SOURCES}
    if mutation:
        _, path, old, new, _ = mutation
        text = (ROOT / path).read_text()
        assert text.count(old) == 1, f'mutation anchor not unique in {path}: {old!r}'
        copy = folder / Path(path).name
        copy.write_text(text.replace(old, new))
        paths[path] = copy
    flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2']).require_ok().stdout)
    common = ['-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-w', '-O1', '-fno-pie',
              '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
              '-Ipc/include', '-Ipc/include/game', '-Ipc/hal', '-include',
              'pc/include/game/types.h', *flags]
    objects = []
    units = (('harness', str(ROOT / 'pc/tests/gpu_hires_harness.c')),
             ('hires', str(paths['pc/hal/gpu_hires.c'])),
             ('profile', str(paths['pc/hal/profile.c'])),
             ('queue', 'pc/hal/gpu_queue.c'), ('texture', 'pc/hal/gpu_texture.c'),
             ('vram', 'pc/hal/gpu_vram.c'), ('ptr', 'pc/hal/ptr_validate.c'))
    for name, src in units:
        obj = folder / (name + '.o')
        run(['cc', *common,
             '-DGPU_SOURCE="' + str(paths['pc/hal/gpu.c'].resolve()) + '"',
             '-DOT_FIXTURE="' + str(ROOT / 'pc/tests/gpu_ot_harness.c') + '"',
             '-c', src, '-o', str(obj)], cwd=ROOT, timeout=60).require_ok()
        objects.append(str(obj))
    binary = folder / 'hires'
    run(['cc', '-no-pie', *objects, '-Wl,--gc-sections', '-o', str(binary)],
        cwd=ROOT, timeout=60).require_ok()
    return binary


def harness_env(scale, profile=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('TOKI_', 'GPU_', 'PC_'))}
    env['TOKI_CONFIG'] = '/dev/null'
    if profile:
        env['TOKI_PROFILE'] = profile
    if scale:
        env.update(GPU_HIRES='1', TOKI_RES_SCALE=str(scale))
    else:
        env['GPU_HIRES'] = '0'
    return env


def run_mode(binary, mode, scale, profile=None):
    return run([str(binary), mode], env=harness_env(scale, profile), timeout=120)


class HiresTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = build(cls.temp.name)

    def same_at_every_scale(self, binary, mode):
        outs = {}
        for scale in (0, 2, 3, 4):
            r = run_mode(binary, mode, scale)
            if r.returncode != 0:
                return False, f'scale {scale}: exit {r.returncode}: {r.stdout[-400:]}'
            outs[scale] = r.stdout.strip().splitlines()[-1]
        return len(set(outs.values())) == 1, outs

    def test_native_vram_and_reads_do_not_depend_on_the_scale(self):
        ok, outs = self.same_at_every_scale(self.binary, 'stream')
        self.assertTrue(ok, outs)

    def test_shadow_pass_leaves_the_texture_cache_alone(self):
        ok, outs = self.same_at_every_scale(self.binary, 'texcache')
        self.assertTrue(ok, outs)
        # the 1x answer is the stale cached line (red), not the filled green
        self.assertEqual(outs[0], '001f 001f 001f 001f')

    def test_profile_gates_and_clamps_the_scale(self):
        cases = ((('parity', '4'), 'scale=1'), (('enhanced', '3'), 'scale=3'),
                 (('enhanced', '9'), 'scale=4'), (('enhanced', None), 'scale=1'))
        for (profile, scale), want in cases:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('TOKI_', 'GPU_', 'PC_'))}
            env.update(TOKI_CONFIG='/dev/null', TOKI_PROFILE=profile)
            if scale:
                env['TOKI_RES_SCALE'] = scale
            r = run([str(self.binary), 'config'], env=env, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn(want, r.stdout, (profile, scale))

    def check(self, mode):
        for scale in (2, 3, 4):
            with self.subTest(scale=scale):
                r = run_mode(self.binary, mode, scale)
                self.assertEqual(r.returncode, 0, r.stdout[-800:])

    def test_sprites_tiles_lines_are_the_1x_pixels(self):
        self.check('sprite')

    def test_one_to_one_texture_is_not_shifted(self):
        self.check('poly1to1')

    def test_polygon_edges_gain_resolution(self):
        self.check('edges')

    def test_fills_uploads_and_clears_replace_sub_pixel_edges(self):
        self.check('fill')

    def test_move_image_carries_the_shadow(self):
        self.check('move')

    def test_unmirrored_native_write_presented_at_1x(self):
        self.check('resync')

    def test_every_native_polygon_pixel_is_in_a_claimed_block(self):
        for scale in (2, 3, 4):
            with self.subTest(scale=scale):
                r = run_mode(self.binary, 'coverage', scale)
                self.assertEqual(r.returncode, 0, r.stdout[-800:])
                line = r.stdout.strip().splitlines()[-1]
                drawn, misses = int(line.split()[1]), int(line.split()[3])
                self.assertGreater(drawn, 1000000, line)
                self.assertEqual(misses, 0, line)


class HiresNegativeControls(unittest.TestCase):
    """Each mechanism broken in a copy of its source: its test must fail."""

    def test_each_mutation_is_caught(self):
        for mutation in MUTATIONS:
            name, _, _, _, mode = mutation
            with self.subTest(mutation=name), tempfile.TemporaryDirectory() as d:
                binary = build(d, mutation)
                if mode in ('stream', 'texcache'):
                    ok, outs = HiresTests.same_at_every_scale(self, binary, mode)
                    self.assertFalse(ok, f'{name}: still {outs}')
                elif mode == 'config':
                    env = {k: v for k, v in os.environ.items()
                           if not k.startswith(('TOKI_', 'GPU_', 'PC_'))}
                    env.update(TOKI_CONFIG='/dev/null', TOKI_PROFILE='parity',
                               TOKI_RES_SCALE='4')
                    r = run([str(binary), 'config'], env=env, timeout=60)
                    self.assertNotIn('scale=1', r.stdout, name)
                elif mode == 'coverage':
                    r = run_mode(binary, mode, 2)
                    line = r.stdout.strip().splitlines()[-1]
                    self.assertNotEqual(int(line.split()[3]), 0, f'{name}: {line}')
                else:
                    r = run_mode(binary, mode, 2)
                    self.assertNotEqual(r.returncode, 0, f'{name}: {mode} still passes')


if __name__ == '__main__':
    unittest.main()
