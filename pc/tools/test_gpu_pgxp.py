# SPDX-License-Identifier: GPL-3.0-or-later
"""Sub-pixel vertex precision for the high-resolution shadow (pc/hal/gpu_pgxp.h).

pc/tests/gpu_pgxp_harness.c includes pc/hal/gpu.c and links the real GTE
model (pc/hal/gte.c), gpu_hires.c, gpu_pgxp.c and profile.c; polygons are
built as the port's TMD builders build them (LDV, RTPT / RTPS, the SXY FIFO
stored into a GP0 packet, DrawOTag).  Every mode is a separate process.

  * the native VRAM is the same with sub-pixel precision on and off, at
    every scale, while the shadow differs (fractions were used);
  * a slowly moving object's vertex, as the shadow shows it, follows its
    straight-line path N times more closely with fractions than without:
    RMS 1/sqrt(12) native pixel (the 1x grid) -> 1/(N sqrt(12)) (the N x
    grid); that 1x snapping is the "wobble";
  * the table's rules: generations, ambiguity, all-or-nothing, and the
    GTE's fractions against an exact projection;
  * the profile gates it: parity off, enhanced on, TOKI_SUBPIXEL=0 off,
    scale 1 off.

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
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402
SOURCES = ('pc/hal/gpu.c', 'pc/hal/gpu_hires.c', 'pc/hal/gpu_pgxp.c',
           'pc/hal/profile.c', 'pc/hal/gte.c')

# (name, file, old, new, mode that must then fail)
MUTATIONS = (
    ('shadow ignores the fractions', 'pc/hal/gpu.c',
     'memcpy(hv, pv, sizeof hv);', '(void)pv;', 'jitter'),
    ('GTE records nothing', 'pc/hal/gte.c',
     '#define GTE_PGXP_ON() (&g_gpu_pgxp_active && g_gpu_pgxp_active && gpu_pgxp_record)',
     '#define GTE_PGXP_ON() (0)', 'jitter'),
    ('lookups never hit', 'pc/hal/gpu_pgxp.c',
     '        if (s->key == word) {', '        if (0) {', 'jitter_single'),
    ('generations never close', 'pc/hal/gpu_pgxp.c',
     'void gpu_pgxp_list_done(void)\n{\n    new_generation();',
     'void gpu_pgxp_list_done(void)\n{', 'rules'),
    ('ambiguous words never marked', 'pc/hal/gpu_pgxp.c',
     '                s->ambiguous = 1;\n                g_gpu_pgxp_stats.ambiguous++;',
     '                g_gpu_pgxp_stats.ambiguous++;', 'rules'),
    ('partly matched polygons use fractions', 'pc/hal/gpu.c',
     '        if (!have[k]) {\n            if (known) g_gpu_pgxp_stats.polys_gte++;\n            return;',
     '        if (0) {\n            if (known) g_gpu_pgxp_stats.polys_gte++;\n            return;',
     'rules'),
    ('fractions off by a sub-pixel', 'pc/hal/gte.c',
     'g_pf[2].fx = (u32)ax & 0xFFFFu;', 'g_pf[2].fx = ((u32)ax + 0x4000u) & 0xFFFFu;',
     'rules'),
    ('native pass reads the shadow vertices', 'pc/hal/gpu.c',
     '        g_hires.in_poly = 1;\n        raster_triangle(input, mode, 0);',
     '        g_hires.in_poly = 1;\n        if (g_pgxp_prim.n) { RasterVertex nv[3]; '
     'memcpy(nv, input, sizeof nv); nv[0].x += 1; raster_triangle(nv, mode, 0); } '
     'else raster_triangle(input, mode, 0);', 'stream'),
    ('missed edge blocks left stale', 'pc/hal/gpu.c',
     '            (g_pgxp_tri && g_hires.ref[at] == old))', '            0)', 'stream'),
    ('parity turns it on', 'pc/hal/profile.c',
     '{ "GPU_PGXP",           0, 1,', '{ "GPU_PGXP",           1, 1,', 'config'),
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
              '-I' + str(ROOT / 'pc/hal'), '-Ipc/include', '-Ipc/include/game',
              '-include', 'pc/include/game/types.h', *flags]
    objects = []
    units = (('harness', str(ROOT / 'pc/tests/gpu_pgxp_harness.c')),
             ('hires', str(paths['pc/hal/gpu_hires.c'])),
             ('pgxp', str(paths['pc/hal/gpu_pgxp.c'])),
             ('profile', str(paths['pc/hal/profile.c'])),
             ('gte', str(paths['pc/hal/gte.c'])),
             ('tables', 'pc/tests/gte_test_tables.c'),
             ('queue', 'pc/hal/gpu_queue.c'), ('texture', 'pc/hal/gpu_texture.c'),
             ('vram', 'pc/hal/gpu_vram.c'), ('ptr', 'pc/hal/ptr_validate.c'))
    for name, src in units:
        obj = folder / (name + '.o')
        extra = ['-I' + decomp('include')] if name in ('gte', 'tables') else []
        run(['cc', *common, *extra,
             '-DGPU_SOURCE="' + str(paths['pc/hal/gpu.c'].resolve()) + '"',
             '-DOT_FIXTURE="' + str(ROOT / 'pc/tests/gpu_ot_harness.c') + '"',
             '-c', src, '-o', str(obj)], cwd=ROOT, timeout=60).require_ok()
        objects.append(str(obj))
    binary = folder / 'pgxp'
    run(['cc', '-no-pie', *objects, '-Wl,--gc-sections', '-lm', '-o', str(binary)],
        cwd=ROOT, timeout=60).require_ok()
    return binary


def harness_env(scale, subpixel=True, profile='enhanced', extra=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('TOKI_', 'GPU_', 'PC_', 'PGXP'))}
    env.update(TOKI_CONFIG='/dev/null', TOKI_PROFILE=profile,
               TOKI_RES_SCALE=str(scale), TOKI_SUBPIXEL='1' if subpixel else '0')
    env.update(extra or {})
    return env


def run_mode(binary, mode, scale, subpixel=True, profile='enhanced', extra=None):
    r = run([str(binary), mode], env=harness_env(scale, subpixel, profile, extra),
            timeout=120)
    return r


# parity with the shadow forced on (GPU_HIRES=1): GPU_PGXP's own default
PARITY_HIRES = {'GPU_HIRES': '1'}


def last_line(r):
    return r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''


def stream_outputs(binary):
    """{(scale, subpixel): (native hash, shadow hash, precise polygons,
    stale blocks)}"""
    outs = {}
    for scale in (1, 2, 3, 4):
        for sub in (False, True):
            r = run_mode(binary, 'stream', scale, sub)
            if r.returncode != 0:
                raise AssertionError(f'stream {scale} {sub}: {r.stdout[-400:]}')
            native, shadow, precise, stale = last_line(r).split()
            outs[(scale, sub)] = (native, shadow, int(precise), int(stale))
    return outs


def stream_ok(outs):
    """'' or why the stream property fails."""
    natives = {v[0] for v in outs.values()}
    if len(natives) != 1:
        return f'native VRAM differs: {outs}'
    for scale in (2, 3, 4):
        on, off = outs[(scale, True)], outs[(scale, False)]
        if on[2] == 0 or off[2] != 0:
            return f'{scale}x: precise polygons on {on[2]} off {off[2]}'
        if on[1] == off[1]:
            return f'{scale}x: the shadow is the same with and without fractions'
    if outs[(1, True)][2]:
        return 'fractions used at 1x'
    stale = {k: v[3] for k, v in outs.items() if v[3]}
    if stale:
        return f'stale shadow blocks (edges shown at 1x): {stale}'
    return ''


def jitter(binary, scale, subpixel):
    r = run_mode(binary, 'jitter', scale, subpixel)
    if r.returncode != 0:
        raise AssertionError(f'jitter {scale} {subpixel}: {r.stdout[-400:]}')
    parts = last_line(r).split()
    return float(parts[1]), float(parts[3]), int(parts[5])


def jitter_ok(binary):
    """'' or why sub-pixel precision does not remove the wobble."""
    for scale in (2, 4):
        off, slope_off, _ = jitter(binary, scale, False)
        on, slope_on, precise = jitter(binary, scale, True)
        if precise < 160 * 7:
            return f'{scale}x: only {precise} precise polygons'
        # 1/sqrt(12) = 0.289: a uniform error over one grid step
        if not (off > 0.25 and on < 0.36 / scale):
            return f'{scale}x: rms {on:.4f} with fractions vs {off:.4f} without'
        if abs(slope_on - 0.15) > 0.01:
            return f'{scale}x: slope {slope_on:.4f} with fractions'
    return ''


class PgxpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = build(cls.temp.name)

    def test_native_vram_is_unchanged_and_the_shadow_uses_fractions(self):
        self.assertEqual(stream_ok(stream_outputs(self.binary)), '')

    def test_slow_motion_follows_its_path(self):
        why = jitter_ok(self.binary)
        self.assertEqual(why, '')
        for scale in (2, 4):
            off, _, _ = jitter(self.binary, scale, False)
            on, _, _ = jitter(self.binary, scale, True)
            print(f'\n[pgxp] {scale}x vertex rms: {off:.4f} px without, {on:.4f} px with')

    def test_rules(self):
        r = run_mode(self.binary, 'rules', 4)
        self.assertEqual(r.returncode, 0, r.stdout[-800:])
        self.assertIn('rules ok', r.stdout)

    def test_profile_and_setting_gate_it(self):
        cases = (('parity', 4, True, None, 'scale=1 pgxp=0'),
                 ('parity', 4, True, PARITY_HIRES, 'scale=4 pgxp=0'),
                 ('enhanced', 4, True, None, 'scale=4 pgxp=1'),
                 ('enhanced', 2, True, None, 'scale=2 pgxp=1'),
                 ('enhanced', 4, False, None, 'scale=4 pgxp=0'),
                 ('enhanced', 1, True, None, 'scale=1 pgxp=0'))
        for profile, scale, sub, extra, want in cases:
            r = run_mode(self.binary, 'config', scale, sub, profile, extra)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn(want, r.stdout, (profile, scale, sub, extra))


def mutation_caught(binary, mode):
    if mode == 'stream':
        return stream_ok(stream_outputs(binary)) != ''
    if mode == 'jitter':
        return jitter_ok(binary) != ''
    if mode == 'jitter_single':
        # the primitive-triple table still answers: the single-word table's
        # own guard is rule 0 (every projection must be found by its word)
        r = run_mode(binary, 'rules', 4)
        return r.returncode != 0
    if mode == 'rules':
        r = run_mode(binary, 'rules', 4)
        return r.returncode != 0
    if mode == 'config':
        r = run_mode(binary, 'config', 4, True, 'parity', PARITY_HIRES)
        return 'pgxp=1' in r.stdout
    raise ValueError(mode)


class PgxpNegativeControls(unittest.TestCase):
    """Each mechanism broken in a copy of its source: its test must fail."""

    def test_each_mutation_is_caught(self):
        for mutation in MUTATIONS:
            name, _, _, _, mode = mutation
            with self.subTest(mutation=name), tempfile.TemporaryDirectory() as d:
                binary = build(d, mutation)
                self.assertTrue(mutation_caught(binary, mode), f'{name}: not caught')


if __name__ == '__main__':
    unittest.main()
