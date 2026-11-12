# SPDX-License-Identifier: GPL-3.0-or-later
"""Native OT cache/raw-tag coherence against the real selected gpu.c.

GPU_SOURCE optionally selects a pre-fix or candidate gpu.c; no GPU code is
copied/extracted or replaced. The C fixture includes that translation unit.
DrawOTag runs in private child processes with a MAP_FIXED_NOREPLACE PS1 page;
there is no SDL window, game asset dependency, or alternate renderer oracle.

Run ONLY when main releases the preservation-job resource restriction:
  . .venv/bin/activate
  python pc/tools/safe_process.py --timeout 120 --max-output 30000 -- \
    flock -w 60 build/gpu-contracts-20261021/worker-tests.lock \
    python -m unittest discover -s pc/tools -p test_gpu_ot_links.py -v

Use GPU_SOURCE=/absolute/path/to/snapshot/gpu.c for a negative-control run.
A mapping failure is a failure, not a silently skipped positive rendering test.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class GpuOtLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='gpu-ot-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'gpu-ot'
        source = Path(os.environ.get('GPU_SOURCE', ROOT / 'pc/hal/gpu.c'))
        source = source.resolve(strict=True)
        # Quoted include macro is one argv element, not a shell command.
        include = '-DGPU_SOURCE="' + str(source) + '"'
        flags = shlex.split(run(['pkg-config', '--cflags', 'sdl2'],
                                timeout=10).require_ok().stdout)
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        # gpu.c itself uses the port's GNU17/legacy callback compatibility
        # mode. The added fixture uses declarations-at-block-start C style.
        # No unresolved-symbol suppression; only unreachable sections are GC'd.
        run([*compiler, '-std=gnu17', '-D_GNU_SOURCE', '-fpermissive', '-O1',
             '-ffunction-sections', '-fdata-sections', include,
             '-Ipc/include', '-Ipc/include/game', '-Ipc/hal',
             'pc/tests/gpu_ot_harness.c', 'pc/hal/gpu_texture.c',
             'pc/hal/gpu_vram.c', 'pc/hal/gpu_queue.c', '-Wl,--gc-sections', '-o', str(cls.binary), *flags],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        env = dict(os.environ, MDFN_GPU='0', GP0_TRACE='0', PC_DEBUG='0')
        run([str(self.binary), case], cwd=ROOT, env=env,
            timeout=10, max_output=16000).require_ok()

    def test_intact_cache_returns_full_native_pointer(self):
        self.check_case('native')

    def test_length_byte_change_does_not_invalidate_native_pointer(self):
        self.check_case('len')

    def test_raw_next_change_supersedes_cached_native_pointer(self):
        self.check_case('raw')

    def test_raw_terminator_supersedes_cached_native_pointer(self):
        self.check_case('terminator')

    def test_restoring_raw_token_does_not_resurrect_obsolete_native_pointer(self):
        self.check_case('restore')

    def test_restoring_token_after_terminator_cannot_resurrect_native_pointer(self):
        self.check_case('terminator-restore')

    def test_explicit_setaddr_retargets_after_raw_invalidation(self):
        self.check_case('retarget')

    def test_drawotag_cached_insertion_reaches_real_native_tile_renderer(self):
        self.check_case('draw-cached')

    def test_drawotag_raw_insertion_overrides_stale_head_and_paints_red(self):
        self.check_case('draw-raw')

    def test_drawotag_cyclic_ot_draws_the_cycle_once(self):
        """a node linked to itself (GsSortClear's packet added twice): the
        console's GPU repeats it until ResetGraph(1) aborts the DMA; the
        port draws it once and returns.  Negative control: GPU_SOURCE=
        the gpu.c before 2026-11-01 draws it 100,001 times."""
        self.check_case('draw-cycle')


if __name__ == '__main__':
    unittest.main()
