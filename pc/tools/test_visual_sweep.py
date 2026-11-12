#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_visual_sweep.py -- the English visual gate (docs/translation/VISUAL-SWEEP.md).

The drawn-text audit (tools/patch/audit_drawn_text.py) sees every string the
engine draws but not text baked into pictures; the Options sub-lists
(OPTION.NBN sprite frames) stayed Japanese in English mode until
2026-11-02 for that reason.  This gate looks at the screen:

  * for every route of config/patch/visual_reference.json, the playthrough
    runs with TOKI_LANG=en and --shots, and every listed shot must show no
    Japanese glyph (tools/patch/visual_sweep.py Detector: 12x12 windows of
    single-colour ink compared with the retail font's kana and kanji, read
    from the player's executable at run time);
  * negative control: the same route with TOKI_LANG=ja must show Japanese
    on its `ja_hits` shots: the detector does see the text the English
    build replaces (the retail OPTION.NBN frames);
  * art on a route or an art scenario (`art` entries: lettering the
    detector cannot read, the banners and Rei Ijuin's name card): the
    cell's English picture shows on some sweep picture of the English run
    and its retail picture on none, and the other way round in Japanese;
  * the sweep capture (pc/hal/sweep_shot.c): PC_SWEEP_DIR writes one PNG per
    distinct picture and an index line for each; without it nothing is
    written.

Needs the PC build with an English pack and the player's assets; skipped
otherwise.  Screenshots live in a temporary directory and are deleted.
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import ENGLISH, disc_input, english_path  # noqa: E402
# The detector (tools/patch/visual_sweep.py) and the reference routes
# (config/patch/visual_reference.json) are the English patch's
# (toki1-english): without it the gate is skipped, by name.
EN = str(ENGLISH) if ENGLISH is not None else None
if EN:
    sys.path.insert(0, str(english_path('tools/patch')))
BIN = os.path.join(ROOT, 'pc', 'build', 'tokimeki-pc')
REF = str(english_path('config/patch/visual_reference.json')) if EN else None
EXE = str(disc_input('disks/SLPS_000.65'))
PACK = os.path.join(ROOT, 'pc', 'build', 'translation', 'en.tktp')


def run_route(script, lang, out, extra=()):
    cmd = [sys.executable, os.path.join(ROOT, 'pc/tools/playthrough.py'), script,
           '--out', out, '--shots', '--set', 'TOKI_LANG=' + lang] + list(extra)
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=900)
    return r


def shot(out, label):
    hits = glob.glob(os.path.join(out, 'shots', '*', label + '.ppm'))
    return hits[0] if hits else None


@unittest.skipIf(EN is None, 'toki1-english is absent (pc/tools/pc_paths.py): '
                 'the English visual gate needs the English patch')
@unittest.skipUnless(os.path.exists(BIN) and os.path.exists(PACK) and os.path.exists(EXE)
                     and os.path.isdir(os.path.join(ROOT, 'pc', 'assets', 'raw')),
                     'needs the PC build with an English pack and the player\'s assets')
class VisualGateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PIL import Image  # noqa: F401  (the detector needs Pillow + numpy)
        import visual_sweep
        cls.vs = visual_sweep
        cls.det = visual_sweep.Detector(EXE)
        cls.ref = json.load(open(REF, encoding='utf-8'))
        cls.tmp = tempfile.mkdtemp(prefix='visual-gate-', dir=os.path.join(ROOT, 'build')
                                   if os.path.isdir(os.path.join(ROOT, 'build')) else None)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def hits(self, path):
        from PIL import Image
        return self.det.scan(Image.open(path))

    def test_reference_screens_english_and_control(self):
        mh = int(self.ref.get('min_hits', 2))
        for i, route in enumerate(self.ref['routes']):
            script = os.path.join(ROOT, route['script'])
            en = os.path.join(self.tmp, 'en%d' % i)
            sweep = os.path.join(self.tmp, 'sweep%d' % i)
            r = run_route(script, 'en', en, ['--set', 'PC_SWEEP_DIR=' + sweep])
            self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
            bad = {}
            for label in route['shots']:
                p = shot(en, label)
                self.assertIsNotNone(p, 'no shot %s' % label)
                h = self.hits(p)
                if len(h) >= mh:
                    bad[label] = ''.join(self.vs.sjis_char(x[2]) for x in h)
            self.assertEqual(bad, {}, 'Japanese on English reference screens')
            # the sweep capture wrote one PNG per index line, all distinct
            runs = glob.glob(os.path.join(sweep, 'run-*'))
            self.assertEqual(len(runs), 1)
            lines = open(os.path.join(runs[0], 'index.tsv')).read().splitlines()
            pngs = glob.glob(os.path.join(runs[0], '*.png'))
            self.assertGreater(len(pngs), 10)
            self.assertEqual(len(lines), len(pngs))
            self.assertEqual(len({l.split('\t')[6] for l in lines}), len(lines))
            # negative controls
            ja = os.path.join(self.tmp, 'ja%d' % i)
            sweep_ja = os.path.join(self.tmp, 'sweep_ja%d' % i)
            # an English route may stop later in Japanese (english_tour
            # types Latin names); only its ja_hits shots must exist
            r = run_route(script, 'ja', ja, ['--set', 'PC_SWEEP_DIR=' + sweep_ja])
            for label in route.get('ja_hits', []):
                self.assertIsNotNone(shot(ja, label), 'Japanese run: no shot %s (%s)' % (label, r.stdout[-1500:]))
                self.assertGreaterEqual(len(self.hits(shot(ja, label))), mh,
                                        'the detector misses the Japanese of %s' % label)
            self.assertFalse(glob.glob(os.path.join(ja, 'sweep*')))
            # art on the route (a logotype the detector cannot read: Rei
            # Ijuin's name card on the tour): the same presence check as
            # the art scenarios, on the two runs' sweep pictures
            if route.get('art'):
                self.check_art(route['art'], sorted(pngs),
                               sorted(glob.glob(os.path.join(sweep_ja, 'run-*', '*.png'))))

    def run_scenario(self, scen, lang, sweep, every):
        cmd = [sys.executable, os.path.join(ROOT, 'pc/tools/scenario.py'), os.path.join(ROOT, scen),
               '--out', os.path.join(self.tmp, 'scen'), '--set', 'TOKI_LANG=' + lang,
               '--set', 'PC_SWEEP_DIR=' + sweep, '--set', 'PC_SWEEP_EVERY=%d' % every]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=1500)
        pngs = sorted(glob.glob(os.path.join(sweep, 'run-*', '*.png')))
        self.assertTrue(pngs, 'no sweep pictures: ' + r.stdout[-1500:] + r.stderr[-1500:])
        return pngs

    def art_seen(self, pngs, pic):
        from PIL import Image
        for p in pngs:
            if self.vs.picture_at(Image.open(p), pic):
                return p
        return None

    def check_art(self, art, en, ja):
        """every listed manifest cell's English lettering shows on some
        picture of the English run (`en`, PNG paths) and its retail
        lettering on none; the Japanese run (`ja`, the negative control)
        shows the retail lettering and never ours.  `region` [x, y, w, h]
        restricts a cell to the part of it a sprite in front leaves visible
        (Ijuin stands over the middle of his name card)."""
        erase_only = {(a['manifest'], a['cell']) for a in art if a.get('erase_only')}
        for a in art:
            key = (a['manifest'], a['cell'])
            pic_en, pic_ja = self.vs.cell_pictures(a['manifest'], a['cell'], a.get('region'))
            if key not in erase_only:   # an erased word (a dropped counter) has no English picture
                self.assertIsNotNone(self.art_seen(en, pic_en), 'English %s not on screen' % (key,))
            self.assertIsNone(self.art_seen(en, pic_ja), 'retail %s on an English screen' % (key,))
            self.assertIsNotNone(self.art_seen(ja, pic_ja),
                                 'control: the retail %s is not seen in Japanese' % (key,))
            if key not in erase_only:
                self.assertIsNone(self.art_seen(ja, pic_en), 'English %s in the Japanese run' % (key,))

    def test_art_scenarios_english_and_control(self):
        """banners and logos (lettering the glyph detector cannot read) on
        the art scenarios: check_art on the English and Japanese runs"""
        for i, route in enumerate(self.ref.get('art_routes', [])):
            every = int(route.get('sweep_every', 5))
            en = self.run_scenario(route['scenario'], 'en', os.path.join(self.tmp, 'art_en%d' % i), every)
            ja = self.run_scenario(route['scenario'], 'ja', os.path.join(self.tmp, 'art_ja%d' % i), every)
            self.check_art(route['art'], en, ja)

    def test_sweep_off_writes_nothing(self):
        out = os.path.join(self.tmp, 'off')
        script = os.path.join(ROOT, 'pc/tools/playthrough/english_options.txt')
        before = set(os.listdir(self.tmp))
        r = run_route(script, 'en', out)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:])
        self.assertFalse(glob.glob(os.path.join(out, '**', 'index.tsv'), recursive=True))
        self.assertEqual(set(os.listdir(self.tmp)) - before, {'off'})


if __name__ == '__main__':
    unittest.main()
