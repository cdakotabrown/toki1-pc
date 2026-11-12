# SPDX-License-Identifier: GPL-3.0-or-later
import io
from pathlib import Path
import struct
import tempfile
import unittest
import wave

import build_lba_map as iso
import xa_extract as xa
import tim_tool as tim
from asset_limits import read_asset, check_pixels


class AssetResourceTests(unittest.TestCase):
    def test_xa_streaming_pcm_matches_sound_group_decoder(self):
        for ci in (0, 1, 4, 5):
            with self.subTest(ci=ci), tempfile.TemporaryDirectory() as d:
                p = Path(d)
                sector = bytearray(xa.SECTOR_SIZE)
                sector[:8] = bytes([1, 2, 4, ci] * 2)
                # Include nonzero ADPCM payload and carry predictor history
                # through all groups/sectors, just as the original decoder.
                for i in range(8, len(sector) - 4):
                    sector[i] = (i * 13) & 255
                (p / 'in.XA').write_bytes(sector * 2)
                self.assertEqual(xa.decode_xa(p / 'in.XA', p / 'out.wav'), 2)
                pcm = bytearray()
                left_state = right_state = (0, 0)
                for _ in range(2):
                    for g in range(xa.GROUPS_PER_SECTOR):
                        off = 8 + g * xa.SOUND_GROUP_SIZE
                        left, right, left_state, right_state = xa.decode_sound_group(
                            sector[off:off + xa.SOUND_GROUP_SIZE],
                            left_state, right_state, stereo=bool(ci & 1))
                        if ci & 1:
                            for lval, rval in zip(left, right):
                                pcm += struct.pack('<hh', lval, rval)
                        else:
                            for val in left:
                                pcm += struct.pack('<h', val)
                with wave.open(str(p / 'out.wav')) as w:
                    self.assertEqual(w.getframerate(), 18900 if ci & 4 else 37800)
                    self.assertEqual(w.readframes(w.getnframes()), bytes(pcm))

    def test_xa_rejects_partial_sector_preserving_output(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'in.XA').write_bytes(b'x')
            (p / 'out.wav').write_bytes(b'preserve')
            with self.assertRaises(ValueError):
                xa.decode_xa(p / 'in.XA', p / 'out.wav')
            self.assertEqual((p / 'out.wav').read_bytes(), b'preserve')

    def test_allocation_budgets_precede_reads_and_image_buffers(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'large'
            with p.open('wb') as f:
                f.truncate(100 * 1024 * 1024)
            with self.assertRaises(ValueError):
                read_asset(p)
        for w, h in ((65535, 65535), (-1, 512), (0, 1)):
            with self.assertRaises(ValueError):
                check_pixels(w, h)
        check_pixels(1024, 512)

    def test_tim_truncation_is_rejected_before_zero_padding(self):
        block = struct.pack('<IHHHH', 12, 0, 0, 16, 16)
        with self.assertRaises(ValueError):
            tim.parse_pixels(block, 0, tim.PMODE_8BPP)

    def test_iso_directory_bounds_and_cycles(self):
        with self.assertRaises(ValueError):
            list(iso.read_dir(io.BytesIO(b''), 0, 1 << 30))
        with self.assertRaises(ValueError):
            iso.walk(io.BytesIO(b''), 0, 2048, seen={0})
        with self.assertRaises(ValueError):
            iso.read_sector_user(io.BytesIO(b'short'), 0)
        disc = bytearray(2352)
        disc[24] = 1
        with self.assertRaises(ValueError):
            list(iso.read_dir(io.BytesIO(disc), 0, 2048))


if __name__ == '__main__':
    unittest.main()
