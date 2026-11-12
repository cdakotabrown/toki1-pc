# SPDX-License-Identifier: GPL-3.0-or-later
"""The native MDEC decoder computes what the console's MDEC computes.

pc/hal/mdec_native.c decodes BS stills (libpress DecDCTvlc/DecDCTout) and
movie frames.  After its VLC pass it must follow the MDEC's fixed-point
arithmetic: dequantization with 4 fraction bits, the two-pass IDCT against
libpress's uploaded scale matrix (rounding (s + 2^17) >> 18 per pass,
sign-extending the low 9 bits), the integer BT.601 conversion and the
15-bit rounding min((c + 4) >> 3, 31) (the reference emulator's MDEC, the AV
reference; PSX-SPX "MDEC").

1. Synthetic DC-only macroblocks: every pixel against the arithmetic
   restated below, for DC values and quantizer scales that straddle the
   rounding boundaries.
2. Scene 64's library picture (BS/C_01_27.BS, decoded by decdct into VRAM
   (320,416)): the 320x88 region the console shows must hash to the
   retail reference VRAM's (the reference emulator, av_parity scene 64,
   checkpoint 800, bit 15 excluded).  Skipped without pc/assets.

MDEC_SOURCE selects another decoder source for negative controls.
"""
import hashlib
import os
from pathlib import Path
import shlex
import struct
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
PICTURE = ROOT / 'pc/assets/raw/CDROM/BS/C_01_27.BS'
PICTURE_SHA256 = '8331385e3ac43edede8927aad4ecaf6c7559dcff820534e734566ede2ff90380'
TACO_BACKDROP = ROOT / 'pc/assets/raw/CDROM/TACO/3D_1.BS'
TACO_BACKDROP_SHA256 = 'c4c0f35e072b0742d6f39e9411f59e63ed91182a39584a00730d91a057b39c23'

HARNESS = r'''#include "types.h"
#include "mdec_native.h"
#include <stdio.h>
#include <stdlib.h>
/* harness IN.BS W H OUT.raw: decode, write W*H little-endian u16 */
int main(int argc, char **argv)
{
    static u8 bs[1 << 20];
    u32 w = (u32)atoi(argv[2]), h = (u32)atoi(argv[3]);
    u16 *img = calloc((size_t)w * h, 2);
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(bs, 1, sizeof bs, f);
    fclose(f);
    if (mdec_decode_bs_native(bs, (u32)n, w, h, img, 0) != 0) return 2;
    f = fopen(argv[4], "wb");
    fwrite(img, 2, (size_t)w * h, f);
    fclose(f);
    free(img);
    return 0;
}
'''

# libpress's tables (retail data 0x8009F884 and 0x8009F908)
IQ0 = 2
DC_SCALE = 23170


def bitstream(qscale, dcs):
    """one macroblock (Cr, Cb, Y0..Y3), each block DC then EOB (10)"""
    bits = ''
    for dc in dcs:
        bits += format(dc & 0x3FF, '010b') + '10'
    bits += '0' * (-len(bits) % 16)
    words = [int(bits[i:i + 16], 2) for i in range(0, len(bits), 16)]
    body = b''.join(struct.pack('<H', w) for w in words)
    return struct.pack('<HHHH', len(words), 0x3800, qscale, 2) + body + bytes(16)


def dc_only_value(dc, qscale):
    """the MDEC's output sample of a block holding only this DC"""
    if qscale & 63 == 0:
        c = dc * 32
    else:
        c = dc * IQ0 * 16 + ((8 if dc < 0 else -8) if dc else 0)
    c = max(-0x4000, min(0x3FFF, c))
    p = max(-32768, min(32767, (c * DC_SCALE + 0x20000) >> 18))
    q = max(-32768, min(32767, (p * DC_SCALE + 0x20000) >> 18)) & 0x1FF
    q = q - 0x200 if q >= 0x100 else q
    return max(-128, min(127, q))


# MPEG-1 intra DC size codes (retail tables 0x800D11D0 chroma and
# ivlc_dct_dc_size_tbl0 0x800D09D0 luma, indexed by the next 8 bits)
DC_CODES_CHROMA = ['00', '01', '10', '110', '1110', '11110', '111110', '1111110', '11111110']
DC_CODES_LUMA = ['100', '00', '01', '101', '110', '1110', '11110', '111110', '1111110']


def v3_dc_code(diff, chroma):
    """the bits of one DC differential: size code, then `size` bits
    (a negative difference is stored as diff + 2^size - 1)"""
    size = abs(diff).bit_length()
    code = (DC_CODES_CHROMA if chroma else DC_CODES_LUMA)[size]
    if size:
        code += format(diff if diff > 0 else diff + (1 << size) - 1, '0%db' % size)
    return code


def rgb555(y, cr, cb):
    def channel(c):
        v = (y + c) & 0x1FF
        v = v - 0x200 if v >= 0x100 else v
        return min(31, (max(-128, min(127, v)) + 128 + 4) >> 3)
    r = channel((359 * cr + 0x80) >> 8)
    g = channel(((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80 >> 8)
    b = channel((454 * cb + 0x80) >> 8)
    return r | g << 5 | b << 10


class MdecRetailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.folder = Path(cls.temp.name)
        (cls.folder / 'harness.c').write_text(HARNESS)
        source = Path(os.environ.get('MDEC_SOURCE', ROOT / 'pc/hal/mdec_native.c'))
        cls.binary = cls.folder / 'mdec'
        run(['cc', '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-w',
             '-Ipc/include/game', '-Ipc/include', '-Ipc/hal',
             *shlex.split(os.environ.get('MDEC_CFLAGS', '')),
             str(cls.folder / 'harness.c'), str(source), '-lm',
             '-o', str(cls.binary)], cwd=ROOT, timeout=60).require_ok()

    def decode(self, path, w, h):
        out = self.folder / 'out.raw'
        run([str(self.binary), str(path), str(w), str(h), str(out)],
            timeout=60).require_ok()
        data = out.read_bytes()
        return list(struct.unpack('<%dH' % (w * h), data))

    def test_dc_only_macroblocks_follow_the_mdec_arithmetic(self):
        cases = [(1, 0, 0, 0), (1, 10, 3, -3), (1, 37, -20, 25),
                 (1, -60, 50, -45), (1, 200, -100, 90), (2, 13, 7, -9),
                 (0, 5, -2, 1), (3, -77, 11, 64), (1, 255, -255, 127)]
        for qscale, y, cr, cb in cases:
            with self.subTest(qscale=qscale, y=y, cr=cr, cb=cb):
                bs = self.folder / 'dc.bs'
                bs.write_bytes(bitstream(qscale, [cr, cb, y, y, y, y]))
                pixels = self.decode(bs, 16, 16)
                want = rgb555(dc_only_value(y, qscale),
                              dc_only_value(cr, qscale),
                              dc_only_value(cb, qscale))
                bad = [i for i, p in enumerate(pixels) if p != want]
                self.assertEqual(bad, [], 'pixel %04x want %04x'
                                 % (pixels[bad[0]] if bad else 0, want))

    def test_version3_dc_is_coded_and_predicted(self):
        """A version-3 bitstream (TACO's 3D_0.BS / 3D_1.BS) codes each DC
        as an MPEG-1 intra DC size VLC plus differential bits, predicted
        per component (Cr, Cb, Y) from the previous block, and libpress's
        ivlc_dct_dc_size hands the predictor * 4 to the MDEC.  Two
        macroblocks: the second's DCs are differences from the first's."""
        mbs = [(10, -3, 7), (-6, 5, 1)]          # (y, cr, cb) of each macroblock, as DC / 4
        bits, pred = '', [0, 0, 0]
        for y, cr, cb in mbs:
            for comp, val in ((0, cr), (1, cb), (2, y), (2, y), (2, y), (2, y)):
                bits += v3_dc_code(val - pred[comp], comp < 2) + '10'
                pred[comp] = val
        bits += '0' * (-len(bits) % 16)
        words = [int(bits[i:i + 16], 2) for i in range(0, len(bits), 16)]
        bs = self.folder / 'v3.bs'
        bs.write_bytes(struct.pack('<HHHH', len(words), 0x3800, 1, 3) +
                       b''.join(struct.pack('<H', w) for w in words) + bytes(16))
        pixels = self.decode(bs, 16, 32)
        for k, (y, cr, cb) in enumerate(mbs):
            want = rgb555(dc_only_value(4 * y, 1), dc_only_value(4 * cr, 1),
                          dc_only_value(4 * cb, 1))
            got = pixels[k * 16 * 16:(k + 1) * 16 * 16]
            with self.subTest(macroblock=k):
                self.assertEqual([p for p in got if p != want], [],
                                 'want %04x' % want)

    def test_taco_backdrop_equals_retail_screen(self):
        """TACO's OPERATIONS backdrop 3D_1.BS (version 3, decoded by
        taco_cd_image_load -> decdct into VRAM (704,0); taco_vram_clear
        then blanks its top 20 rows): rows 20..239 must hash to the retail
        screen's (the reference emulator, game frame 5977, 40 frames into
        the second tutorial page: 0 of 70,400 pixels differ at 5 bits).
        The decoder before 2026-11-06 read every DC as a plain 10-bit
        field and failed on the first macroblock (the port's tile mosaic)."""
        if not TACO_BACKDROP.exists():
            self.skipTest('pc/assets not present')
        pixels = self.decode(TACO_BACKDROP, 320, 240)
        shown = b''.join(struct.pack('<H', p & 0x7FFF) for p in pixels[320 * 20:])
        self.assertEqual(hashlib.sha256(shown).hexdigest(), TACO_BACKDROP_SHA256)

    def test_library_picture_equals_retail_vram(self):
        if not PICTURE.exists():
            self.skipTest('pc/assets not present')
        pixels = self.decode(PICTURE, 320, 96)
        shown = b''.join(struct.pack('<H', p & 0x7FFF) for p in pixels[:320 * 88])
        self.assertEqual(hashlib.sha256(shown).hexdigest(), PICTURE_SHA256)


if __name__ == '__main__':
    unittest.main()
