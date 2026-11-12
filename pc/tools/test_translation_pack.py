#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests: the English translation pack, its content audit, the patch-engine
adaptation and the hook routing.

The synthetic tests need nothing.  The real-pack tests run when the build
machine has the pack (pc/build/translation/en.tktp, from
`translation_pack.py build`) and the player's extraction (pc/assets/raw);
they are skipped otherwise.  Every check has a negative control.
"""
import hashlib
import json
import os
import random
import sys
import tempfile
import unittest


def _read(path):
    with open(path, 'rb') as fh:
        return fh.read()


def _read_text(path):
    with open(path) as fh:
        return fh.read()

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import translation_pack as tp          # noqa: E402
from pc_paths import ENGLISH, MONOREPO_ENGLISH, english_path  # noqa: E402
import adapt_patch_sources as aps      # noqa: E402
import gen_lang_hooks as glh           # noqa: E402

PACK = os.path.join(ROOT, 'pc/build/translation/en.tktp')
RAW = os.path.join(ROOT, 'pc/assets/raw')
# The English patch's own tools, art and patch C (toki1-english): the tests
# of those need it and are skipped, by name, without it.
EN = str(ENGLISH) if ENGLISH is not None else None
NO_EN = 'toki1-english is absent (pc/tools/pc_paths.py): the English patch tests need it'
ART = str(english_path('src/patch/art')) if EN else None


def _pixels(n, seed):
    """Stand-in for retail art: n bytes of busy 4-bpp pixels."""
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(n))


class EncoderTest(unittest.TestCase):
    def test_round_trip_and_no_retail_nibble(self):
        r = random.Random(7)
        base = bytearray(_pixels(6000, 1))
        out = bytearray(base)
        out[100:120] = b'Hello, Tokimeki HS!!'      # our text over retail bytes
        out[3000:3400] = base[200:600]                # a relocated retail run
        for k in range(4000, 4100):                   # nibble art edits
            out[k] = (out[k] & 0xF0) | r.randrange(16)
        out[5000:5100] = b'\0' * 100                  # an erase
        ops = tp.encode(bytes(base), bytes(out))
        self.assertEqual(tp.apply_ops(ops, bytes(base), len(out)), bytes(out))
        kinds = {op[0] for op in ops}
        self.assertIn(tp.COPY, kinds)                 # the move is not stored
        pos = 0
        for op in ops:
            if op[0] == tp.MASK:
                m, v = op[1], op[2]
                for k in range(len(m)):
                    self.assertEqual(v[k] & ~m[k] & 0xFF, 0)
                    b, o = base[pos + k], out[pos + k]
                    for nib in (0xF0, 0x0F):          # only changed nibbles are written
                        self.assertEqual(bool(m[k] & nib), bool((b ^ o) & nib))
            pos += {tp.SAME: lambda o: o[1], tp.COPY: lambda o: o[2],
                    tp.MASK: lambda o: len(o[1]), tp.FILL: lambda o: o[1]}[op[0]](op)
        # negative control: a corrupted op stream does not reproduce the file
        bad = [op if op[0] != tp.COPY else (tp.COPY, op[1] + 1, op[2]) for op in ops]
        self.assertNotEqual(tp.apply_ops(bad, bytes(base), len(out)), bytes(out))

    def test_serialize_parse(self):
        base = _pixels(500, 2)
        out = bytearray(base)
        out[10:15] = b'ABCDE'
        ops = tp.encode(base, bytes(out))
        blob = tp.serialize([('CDROM/X/Y.BIN', base, bytes(out), ops)])
        (e,) = tp.parse(blob)
        self.assertEqual(e['path'], 'CDROM/X/Y.BIN')
        self.assertEqual(e['base_sha1'], hashlib.sha1(base).hexdigest())
        self.assertEqual(tp.apply_ops(e['ops'], base, e['out_size']), bytes(out))
        with self.assertRaises(ValueError):           # negative control
            tp.parse(blob + b'x')


class AuditTest(unittest.TestCase):
    """The pack-content audit on a synthetic 'disc' of two retail files."""

    def _disc(self, d):
        os.makedirs(os.path.join(d, 'raw/CDROM/ART'))
        a = _pixels(16384, 3)                           # an NBN page pair
        b = _pixels(16384, 4)
        open(os.path.join(d, 'raw/CDROM/ART/A.NBN'), 'wb').write(a)
        open(os.path.join(d, 'raw/CDROM/ART/B.NBN'), 'wb').write(b)
        return a, b

    def test_lettering_passes_and_retail_page_fails(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = self._disc(d)
            raw = os.path.join(d, 'raw')
            # ours: lettering nibbles + an erase on B, a relocated run in A
            b2 = bytearray(b)
            for k in range(2000, 2300):
                b2[k] = (b2[k] & 0xF0) | 0x3
            b2[5000:5600] = b'\x11' * 600
            a2 = bytearray(a)
            a2[9000:9400] = a[100:500]
            entries = [(p, base, bytes(o), tp.encode(base, bytes(o)))
                       for p, base, o in (('CDROM/ART/A.NBN', a, a2), ('CDROM/ART/B.NBN', b, b2))]
            problems, cats = tp.audit(tp.serialize(entries), [raw])
            self.assertEqual(problems, [])
            # NEGATIVE CONTROL 1: a retail page of A pasted into B is a literal
            # the encoder cannot copy (another file) -> the audit catches it
            b3 = bytearray(b2)
            b3[8192:8192 + 4096] = a[0:4096]
            bad = [entries[0], ('CDROM/ART/B.NBN', b, bytes(b3), tp.encode(b, bytes(b3)))]
            problems, _ = tp.audit(tp.serialize(bad), [raw])
            self.assertTrue(any('occurs in a retail file' in p for p in problems), problems)
            # NEGATIVE CONTROL 2: a whole retail page shipped as a new file
            page = a[0:8192]
            bad = entries + [('CDROM/ART/NEW.NBN', None, page, tp.encode(b'', page))]
            problems, _ = tp.audit(tp.serialize(bad), [raw])
            self.assertTrue(any('new file' in p for p in problems), problems)
            self.assertTrue(any('whole asset' in p for p in problems), problems)


@unittest.skipUnless(os.path.exists(PACK) and os.path.isdir(RAW),
                     'needs the built pack and the player extraction')
class RealPackTest(unittest.TestCase):
    def test_pack_is_ours_and_applies(self):
        blob = _read(PACK)
        # the pack carries its own audit metadata (@audit.json): no sidecar
        # is needed, so this test and `translation_pack.py audit` agree
        problems, cats = tp.audit(blob, [RAW])
        self.assertEqual(problems, [])
        self.assertEqual(tp.main(['audit', PACK]), 0)
        entries = tp.parse(blob)
        paths = {e['path'] for e in entries}
        self.assertIn('PSX.EXE', paths)
        self.assertIn('@module.syms', paths)
        self.assertIn(tp.AUDIT_META, paths)
        self.assertFalse(any(p.upper().endswith(tp.MEDIA_EXT) for p in paths))
        # every recipe applies to the player's own file and lands on its hash
        for e in entries:
            if e['path'].startswith('@'):
                continue
            base = _read(os.path.join(RAW, e['path']))
            self.assertEqual(hashlib.sha1(base).hexdigest(), e['base_sha1'], e['path'])
            out = tp.apply_ops(e['ops'], base, e['out_size'])
            self.assertEqual(hashlib.sha1(out).hexdigest(), e['out_sha1'], e['path'])

    def test_real_retail_page_is_caught(self):
        """NEGATIVE CONTROL on the real pack: one retail NBN page (from the
        player's TITLE.NBN) written as literal bytes into the FILESEL.NBN
        recipe must fail the audit."""
        blob = _read(PACK)
        entries = tp.parse(blob)
        title = _read(os.path.join(RAW, 'CDROM/FUKUTEST/TITLE.NBN'))
        page = title[0x8000:0x8000 + 0x2000]
        rebuilt = []
        for e in entries:
            base = b'' if e['path'].startswith('@') else _read(os.path.join(RAW, e['path']))
            out = tp.apply_ops(e['ops'], base, e['out_size'])
            if e['path'] == 'CDROM/FUKUTEST/FILESEL.NBN':
                out = bytearray(out)
                out[0x10000:0x10000 + len(page)] = page
                out = bytes(out)
            rebuilt.append((e['path'], base if e['base_size'] else None, out, tp.encode(base, out)))
        problems, _ = tp.audit(tp.serialize(rebuilt), [RAW])
        self.assertTrue(any(p.startswith('CDROM/FUKUTEST/FILESEL.NBN') for p in problems), problems)

    def _rebuild(self, exe_edit=None, drop_meta=False):
        """the real pack re-serialised, PSX.EXE's output edited by exe_edit"""
        rebuilt = []
        for e in tp.parse(_read(PACK)):
            if drop_meta and e['path'] == tp.AUDIT_META:
                continue
            base = b'' if e['path'].startswith('@') else _read(os.path.join(RAW, e['path']))
            out = tp.apply_ops(e['ops'], base, e['out_size'])
            if e['path'] == 'PSX.EXE' and exe_edit:
                out = exe_edit(bytearray(out))
            rebuilt.append((e['path'], base if e['base_size'] else None, out, tp.encode(base, out)))
        return tp.serialize(rebuilt)

    def _code_range(self):
        meta = tp.embedded_meta(tp.parse(_read(PACK)))
        return [r for r in meta['compiled_code'] if r[0] == 'PSX.EXE'][0][1:3]

    def _retail_code(self):
        """0x100 bytes of retail MIPS code from another file (an overlay), so
        the encoder cannot express them as a COPY from PSX.EXE itself"""
        exn = _read(os.path.join(RAW, 'CDROM/EXEDIR/OPTION.EXN'))
        return exn[0x1000:0x1100]

    def test_code_range_is_what_the_build_compiled(self):
        """NEGATIVE CONTROLS for the compiled-code exemption (2026-11-02):
        (1) without the pack's metadata the module's own IDO code trips the
        literal-run check -- the exemption is real and needed; (2) a retail
        code run written OUTSIDE the range is caught; (3) one written INSIDE
        the range is caught too: the range is bound to the build's bytes."""
        lo, hi = self._code_range()
        problems, _ = tp.audit(self._rebuild(drop_meta=True), [RAW])
        self.assertTrue(problems and all(p.startswith('PSX.EXE+') for p in problems), problems)
        self.assertTrue(all(lo <= int(p.split('+')[1].split(':')[0], 16) < hi for p in problems))
        run = self._retail_code()

        def outside(out):
            out[hi + 0x2000:hi + 0x2000 + len(run)] = run
            return bytes(out)

        def inside(out):
            out[lo + 0x100:lo + 0x100 + len(run)] = run
            return bytes(out)
        problems, _ = tp.audit(self._rebuild(outside), [RAW])
        self.assertTrue(any(p.startswith('PSX.EXE+') and 'retail file' in p for p in problems),
                        problems)
        problems, _ = tp.audit(self._rebuild(inside), [RAW])
        self.assertTrue(any('not the build' in p for p in problems), problems)


@unittest.skipIf(EN is None, NO_EN)
class ArtSourceAuditTest(unittest.TestCase):
    """src/patch/art: every PNG is lettering or a mask (few colours, mostly
    transparent or flat), never a crop of retail art."""

    MAX_COLOURS = 8
    MAX_AREA = 320 * 32
    # a logotype cell (nbn_inject `face`: Rei Ijuin's name card) spans
    # the whole plane width at ~60-px caps; such a cell is mostly
    # transparent, a crop of art is not
    MAX_LOGO_AREA = 320 * 128
    MAX_LOGO_OPAQUE = 0.5
    # banner lettering drawn in a vertical CLUT ramp (nbn_inject `fg_rows`:
    # TAMA's BALL / OUT!, SHATEKI's FINISH!) has one fill colour per row:
    # many colours overall, but each row is the fill plus the ring
    MAX_ROW_COLOURS = 3

    @staticmethod
    def verdict(im):
        rgba = im.convert('RGBA')
        cols = rgba.getcolors(1 << 16) or []
        w, h = rgba.size
        if len(cols) > ArtSourceAuditTest.MAX_COLOURS:
            px = rgba.load()
            worst = max(len({px[x, y] for x in range(w) if px[x, y][3] >= 128})
                        for y in range(h))
            if worst > ArtSourceAuditTest.MAX_ROW_COLOURS:
                return 'too many colours (%d, %d in one row): an image, not lettering' % (
                    len(cols), worst)
        if w * h > ArtSourceAuditTest.MAX_AREA:
            opaque = sum(n for n, c in cols if c[3] >= 128) / float(w * h)
            if w * h > ArtSourceAuditTest.MAX_LOGO_AREA or opaque > ArtSourceAuditTest.MAX_LOGO_OPAQUE:
                return 'too large (%dx%d, %.0f%% opaque) for a lettering cell' % (w, h, 100 * opaque)
        return None

    def test_art_pngs_are_lettering(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        n = 0
        for d in sorted(os.listdir(ART)):
            for f in sorted(os.listdir(os.path.join(ART, d))):
                if f.endswith('.png'):
                    n += 1
                    v = self.verdict(Image.open(os.path.join(ART, d, f)))
                    self.assertIsNone(v, '%s/%s: %s' % (d, f, v))
        self.assertGreater(n, 100)
        # negative control: a busy 128x128 page (what a retail crop looks like)
        page = Image.frombytes('L', (128, 128), _pixels(128 * 128, 9))
        self.assertIsNotNone(self.verdict(page))
        # a ramp-lettered cell passes, the same size of busy pixels does not
        ramp = Image.new('RGBA', (64, 40), (0, 0, 0, 0))
        for y in range(40):
            for x in range(8, 56, 3):
                ramp.putpixel((x, y), (0, 4 * y, 200, 255))
        self.assertIsNone(self.verdict(ramp))
        busy = Image.frombytes('L', (64, 40), _pixels(64 * 40, 9)).convert('RGBA')
        self.assertIsNotNone(self.verdict(busy))
        # a logotype cell: a few colours per row on a mostly transparent
        # 320x122 cell passes; the same cell half opaque (an art crop with
        # few colours) does not, nor a bigger one
        logo = Image.new('RGBA', (320, 122), (0, 0, 0, 0))
        for y in range(40, 96):
            for x in range(10, 310, 2):
                logo.putpixel((x, y), (200, 150 + y // 4, 0, 255))
        self.assertIsNone(self.verdict(logo))
        half = logo.copy()
        for y in range(0, 122):
            for x in range(0, 170):
                half.putpixel((x, y), (0, 0, 0, 255))
        self.assertIsNotNone(self.verdict(half))
        self.assertIsNotNone(self.verdict(Image.new('RGBA', (384, 128), (0, 0, 0, 0))))

    def test_art_pngs_are_font_renders(self):
        """Every lettering PNG is exactly what `nbn_inject.py --render`
        draws from its manifest text with our fonts (the injector refuses
        anything else at build time).  Negative control: a crop of the
        retail spoon-relay panel (one Japanese text line, few colours, so
        the colour/size verdict above lets it through) put in place of the
        English line is refused."""
        if not MONOREPO_ENGLISH:
            # nbn_inject --render names its outputs relative to its own
            # checkout and refuses a work directory outside it, and this
            # test never writes into a sibling checkout
            self.skipTest('needs the English patch in this checkout: nbn_inject.py --render '
                          'works only inside toki1-english (run it there or in the monorepo)')
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        sys.path.insert(0, str(english_path('tools/patch')))
        import nbn_inject as ni
        import tilemap_panel as tmp
        spsetu = os.path.join(ni.EXTRACTED, 'CDROM/TAIIKU/SPSETU.BIN')
        if not os.path.isfile(spsetu):
            self.skipTest('no retail extraction (assets/extracted_iso)')
        import shutil
        import subprocess
        n = 0
        os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)       # --render prints repo-relative paths
        with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, 'build')) as td:
            for d in sorted(os.listdir(ART)):
                dst = os.path.join(td, d)
                shutil.copytree(os.path.join(ART, d), dst)
                r = subprocess.run([sys.executable, str(english_path('tools/patch/nbn_inject.py')),
                                    '--render', os.path.join(dst, 'manifest.json')],
                                   capture_output=True, text=True, cwd=EN)
                self.assertEqual(r.returncode, 0, r.stderr[-400:])
                for f in sorted(os.listdir(os.path.join(ART, d))):
                    if f.endswith('.png'):
                        n += 1
                        a = Image.open(os.path.join(ART, d, f)).convert('RGBA')
                        b = Image.open(os.path.join(dst, f)).convert('RGBA')
                        self.assertEqual(a.tobytes(), b.tobytes(), '%s/%s is not the font render' % (d, f))
            self.assertGreater(n, 100)
            # negative control: the retail panel's first Japanese line as the PNG
            man_path = os.path.join(td, 'SPSETU', 'manifest.json')
            man = json.loads(_read_text(man_path))
            cell = next(c for c in man['cells'] if c['name'] == 'spoon_relay_line1')
            tm = tmp.Tilemap(bytearray(_read(spsetu)), man['tilemaps'][0])
            plane = tm.decode()
            clut = tmp.file_clut(_read(spsetu))
            x0, y0, w, h = cell['x'], cell['y'], cell['w'], cell['h']
            crop = Image.new('RGBA', (w, h))
            crop.putdata([clut[plane[y0 + y][x0 + x]] + (255,) for y in range(h) for x in range(w)])
            self.assertIsNone(self.verdict(crop), 'the crop should pass the colour/size test')
            crop.save(os.path.join(td, 'SPSETU', 'spoon_relay_line1.png'))
            r = subprocess.run([sys.executable, str(english_path('tools/patch/nbn_inject.py')), man_path],
                               capture_output=True, text=True, cwd=EN)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('not the font render', r.stdout + r.stderr)

    def test_face_cells_have_a_declared_source(self):
        """A `face` cell (hand-authored display lettering, tools/patch/
        artface.py) is exactly the render of its declared face file, whose
        SHA-1 the manifest records.  Negative controls on the CUT_10 name
        card: (1) the PNG with one pixel changed is refused; (2) a crop of
        the retail kanji (same box, same CLUT) put in its place is refused;
        (3) the face file edited by one coordinate (hash no longer the
        declared one) is refused, in --render too; (4) a face file without
        the authorship header is refused."""
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        sys.path.insert(0, str(english_path('tools/patch')))
        import nbn_inject as ni
        import artface
        src = os.path.join(ni.EXTRACTED, 'CDROM/PROLODIR/CUT_10.NBN')
        if not os.path.isfile(src):
            self.skipTest('no retail extraction (assets/extracted_iso)')
        import shutil
        import subprocess
        man_src = os.path.join(ART, 'CUT_10', 'manifest.json')
        man0 = json.loads(_read_text(man_src))
        cell = next(c for c in man0['cells'] if c['name'] == 'name_card')
        self.assertEqual(cell['face_sha1'], artface.sha1(cell['face']))
        inject = [sys.executable, str(english_path('tools/patch/nbn_inject.py'))]
        os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
        with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, 'build')) as td:
            dst = os.path.join(td, 'CUT_10')
            shutil.copytree(os.path.join(ART, 'CUT_10'), dst)
            man_path = os.path.join(dst, 'manifest.json')
            png = os.path.join(dst, 'name_card.png')
            r = subprocess.run(inject + [man_path], capture_output=True, text=True, cwd=EN)
            self.assertEqual(r.returncode, 0, r.stderr[-400:])
            # (1) one pixel of the lettering changed
            im = Image.open(png).convert('RGBA')
            good = im.tobytes()
            x, y = next((x, y) for y in range(im.height) for x in range(im.width) if im.getpixel((x, y))[3])
            im.putpixel((x, y), (1, 2, 3, 255))
            im.save(png)
            r = subprocess.run(inject + [man_path], capture_output=True, text=True, cwd=EN)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('not the render of its declared face', r.stdout + r.stderr)
            # (2) the retail kanji cut from the player's file, in the cell's CLUT
            retail = _read(src)
            clut = ni.load_clut(man0['clut'], retail, 36)
            surf = ni.cell_surface(bytearray(retail), man0, cell)
            x0, y0, w, h = ni.cell_box(cell)
            er = ni.erase_set(cell)
            crop = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            for yy in range(h):
                for xx in range(w):
                    v = surf.get(x0 + xx, y0 + yy)
                    if v in er:
                        crop.putpixel((xx, yy), clut[v] + (255,))
            # (the colour/size verdict above refuses this crop on its own: the
            # retail ramp has 18 colours; the injector must refuse it too)
            crop.save(png)
            r = subprocess.run(inject + [man_path], capture_output=True, text=True, cwd=EN)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('not the render of its declared face', r.stdout + r.stderr)
            Image.frombytes('RGBA', (w, h), good).save(png)
            r = subprocess.run(inject + [man_path], capture_output=True, text=True, cwd=EN)
            self.assertEqual(r.returncode, 0, r.stderr[-400:])
            # (3) the face file edited: the manifest's hash no longer matches
            face_copy = os.path.join(td, 'display_caps.txt')
            text = _read_text(artface.face_path(cell['face']))
            self.assertIn('rect 0,0 22,100', text)
            with open(face_copy, 'w') as fh:
                fh.write(text.replace('rect 0,0 22,100', 'rect 0,0 23,100', 1))
            man = json.loads(_read_text(man_path))
            man['cells'][0]['face'] = os.path.relpath(face_copy, EN)
            with open(man_path, 'w') as fh:
                json.dump(man, fh)
            for extra in ([], ['--render']):
                r = subprocess.run(inject + extra + [man_path], capture_output=True, text=True, cwd=EN)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('the source changed', r.stdout + r.stderr)
            # (4) the right hash but no authorship header
            with open(face_copy, 'w') as fh:
                fh.write(text.replace('AUTHORED FOR THIS PROJECT', 'authored elsewhere'))
            man['cells'][0]['face_sha1'] = hashlib.sha1(_read(face_copy)).hexdigest()
            with open(man_path, 'w') as fh:
                json.dump(man, fh)
            r = subprocess.run(inject + [man_path], capture_output=True, text=True, cwd=EN)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('AUTHORED FOR THIS PROJECT', r.stdout + r.stderr)


@unittest.skipIf(EN is None, NO_EN)
class TilemapPanelTest(unittest.TestCase):
    """tools/patch/tilemap_panel.py: an identity re-tile changes no byte,
    an edit decodes back exactly, a corrupted cell is seen, a picture that
    cannot fit is refused (its self-test, on the retail spoon-relay panel)."""

    def test_self_test(self):
        sys.path.insert(0, str(english_path('tools/patch')))
        import tilemap_panel as tmp
        if not os.path.isfile(os.path.join(tmp.EXTRACTED, 'CDROM/TAIIKU/SPSETU.BIN')):
            self.skipTest('no retail extraction (assets/extracted_iso)')
        self.assertIn('blocks re-tiled', tmp.self_test('CDROM/TAIIKU/SPSETU.BIN', tmp.SPSETU))


@unittest.skipIf(EN is None, NO_EN)
class AdaptAndRoutingTest(unittest.TestCase):
    def test_every_rule_applies_to_the_patch_sources(self):
        for name in aps.RULES:
            text = _read_text(str(english_path('src/patch/main') / name))
            aps.adapt(name, text)
        # negative control: a changed source fails loudly
        with self.assertRaises(SystemExit):
            aps.adapt('text_misc.c', 'int x;')

    def test_every_patch_hook_has_a_pc_route(self):
        profile = json.loads(_read_text(glh.PROFILE))
        rs = glh.routes(profile, glh.load_registry())
        n_hooks = (len(profile['main']['module']['hooks']) + len(profile['main']['callsites'])
                   + sum(len(o.get('hooks', [])) + len(o.get('callsites', []))
                         for o in profile['overlays'].values()))
        self.assertEqual(len(rs), n_hooks)
        self.assertEqual([r for r in rs if r[4] == 'pending'], [])
        for ov, hooks in glh.ovl_hooks(profile).items():
            for sym, addr, target in hooks:
                self.assertTrue(sym.startswith(ov + '_'))
        # negative control: a hook with no route stops the generator
        bad = json.loads(json.dumps(profile))
        bad['main']['module']['hooks'].append({'function': 'nope', 'addr': '0x80041000'})
        with self.assertRaises(SystemExit):
            glh.routes(bad, glh.load_registry())

    def test_overlay_filter_keeps_the_definition(self):
        src = ('s32 NAME_ENT_name_ent_hira_disp();\n'
               '#define NAME_ENT_name_ent_hira_disp__c1 ((s32 (*)())NAME_ENT_name_ent_hira_disp)\n'
               's32 NAME_ENT_name_ent_hira_disp(u32 title)\n{\n    return 0;\n}\n'
               'void f(void) { NAME_ENT_name_ent_hira_disp(3); }\n')
        profile = {'overlays': {'NAME_ENT': {'hooks': [
            {'function': 'name_ent_hira_disp', 'addr': '0x801330E8', 'target': 'pt_name_page_latin'}]}}}
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'overlay_NAME_ENT.c')
            open(p, 'w').write(src)
            glh.ovl_filter(p, profile)
            out = _read_text(p)
        self.assertIn('s32 NAME_ENT_name_ent_hira_disp(u32 title)\n{', out)     # definition kept
        self.assertIn('NAME_ENT_name_ent_hira_disp__langhook(3)', out)        # call routed
        self.assertIn('())NAME_ENT_name_ent_hira_disp__langhook)', out)       # pointer routed
        self.assertIn('s32 NAME_ENT_name_ent_hira_disp__langhook();', out)    # declared
        self.assertNotIn('NAME_ENT_name_ent_hira_disp(3)', out)               # negative


if __name__ == '__main__':
    unittest.main()
