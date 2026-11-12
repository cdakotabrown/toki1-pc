#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The scene-list oracle: the matrix must only sweep steps game_main dispatches,
and it must tell "the scene ran" apart from "the game bounced to the title".

No game assets required; it reads the tracked matched C directly.
"""
import unittest

import scene_matrix as matrix


class EnterableStepsTests(unittest.TestCase):
    def test_enterable_set_matches_game_main_switch(self):
        steps = matrix.enterable_steps()
        # The steps the matrix has always been asked about, by hand, must be in
        # the parsed set -- a regression here means the parser is wrong.
        for want in (18, 3, 16, 48, 49, 52, 64, 65, 66, 68, 70,
                     80, 90, 99, 112, 118, 128, 130, 144, 145, 208, 254, 255):
            self.assertIn(want, steps)
        # Internal sub-steps the old 0-210 default swept (and that only ever
        # measured the title) must NOT be enterable.
        for bad in (4, 5, 6, 7, 10, 40, 41, 100, 150, 200):
            self.assertNotIn(bad, steps)
        # The title is always enterable; the redirect list is a subset of it.
        self.assertIn(matrix.TITLE_STEP, steps)
        self.assertEqual(set(matrix.redirect_steps()) - set(steps), set())

    def test_scene_steps_drop_the_non_scene_steps(self):
        scenes = matrix.scene_steps()
        for s in sorted(matrix.NOT_A_SCENE):
            self.assertNotIn(s, scenes)
        self.assertGreater(len(scenes), 40)
        # Every scene step is enterable.
        enterable = set(matrix.enterable_steps())
        self.assertEqual(set(scenes) - enterable, set())

    def test_redirect_scene_is_enterable(self):
        # 94 goes through step 2 (the overlay-load step) and then into the scene;
        # it must be in both the redirect list and the scene list.
        self.assertIn(94, matrix.redirect_steps())
        self.assertIn(94, matrix.scene_steps())


class ClassifyEntryTests(unittest.TestCase):
    def test_entered(self):
        self.assertEqual(matrix.classify_entry(94, '94/2/2', True), 'entered')

    def test_bounced_never_dispatched(self):
        self.assertEqual(matrix.classify_entry(5, '18/1/0', False), 'bounced')

    def test_returned_entered_then_left(self):
        # 91 logs SCENE_CHANGE 2->91 then the scene's own logic leaves for the
        # title: that is "returned", a different work item than "bounced".
        self.assertEqual(matrix.classify_entry(91, '18/1/0', True), 'returned')

    def test_advanced(self):
        self.assertEqual(matrix.classify_entry(16, '0/0/0', True), 'advanced')
        self.assertEqual(matrix.classify_entry(48, '64/1/0', True), 'advanced')

    def test_unknown_end(self):
        self.assertEqual(matrix.classify_entry(94, '?', False), '?')
        self.assertEqual(matrix.classify_entry(94, None, False), '?')


class BounceExitTests(unittest.TestCase):
    """The whole point of the oracle: a scene that never ran is not a pass."""

    def run_report(self, rows, allow_bounced=False):
        from contextlib import redirect_stdout
        from types import SimpleNamespace
        from unittest.mock import patch
        import io
        import tempfile
        from pathlib import Path
        matrix.STOP.clear()
        self.addCleanup(matrix.STOP.clear)
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'matrix.txt'
            args = SimpleNamespace(out=str(out), jobs=1, frames=2,
                                   clock='vblank', timeout=1,
                                   allow_bounced=allow_bounced)
            with patch.object(matrix, 'run_scene',
                              side_effect=lambda scene, *unused: rows[scene]):
                with redirect_stdout(io.StringIO()):
                    return matrix.run_matrix(args, range(len(rows)), 'synthetic')

    def row(self, entry):
        return dict(scene=0, end='0/0/0', frames=2, crashes=0,
                    first='', status='OK', entry=entry)

    def test_bounced_row_fails(self):
        self.assertEqual(self.run_report([self.row('bounced')]), 1)

    def test_allow_bounced_downgrades(self):
        self.assertEqual(self.run_report([self.row('bounced')], allow_bounced=True), 0)

    def test_returned_and_advanced_are_not_failures(self):
        self.assertEqual(self.run_report([self.row('returned'), self.row('advanced')]), 0)


if __name__ == '__main__':
    unittest.main()

