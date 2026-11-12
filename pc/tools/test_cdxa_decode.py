# SPDX-License-Identifier: GPL-3.0-or-later
"""Asset-independent XA interpolation controls through the public decoder API."""
import ctypes
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


def sector_fixture(stereo, eight_bit, half_rate):
    """Filter-zero XA blocks alternate valid positive/negative full-scale codes."""
    coding = int(stereo) | (0x10 if eight_bit else 0) | (4 if half_rate else 0)
    sector = bytearray(2336)
    sector[:8] = bytes((1, 0, 0x64, coding)) * 2
    for group in range(18):
        for sample in range(28):
            codes = []
            for channel in range(4 if eight_bit else 8):
                positive = (sample + (channel if stereo else 0)) % 2 == 0
                codes.append((127 if positive else 128) if eight_bit
                             else (7 if positive else 8))
            if not eight_bit:
                codes = [codes[i] | (codes[i + 1] << 4) for i in range(0, 8, 2)]
            offset = 8 + group * 128 + 16 + sample * 4
            sector[offset:offset + 4] = bytes(codes)
    return bytes(sector)


def expected_pcm(stereo, eight_bit, half_rate):
    """Exact integers for the existing Q16 linear path, not a PS1 filter oracle."""
    frames = 18 * (4 if eight_bit else 8) * 28 // (2 if stereo else 1)
    step = ((18900 if half_rate else 37800) << 16) // 44100
    high = 32512 if eight_bit else 28672
    low = -32768
    result = []
    position = 0
    while position >> 16 < frames:
        source = position >> 16
        fraction = position & 65535
        following = min(source + 1, frames - 1)
        for channel in range(2):
            lane = channel if stereo else 0
            first = high if (source + lane) % 2 == 0 else low
            second = high if (following + lane) % 2 == 0 else low
            result.append(first + (((second - first) * fraction) >> 16))
        position += step
    return result


class CdxaInterpolationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        binary = cls.path / 'cdxa.so'
        run(['gcc', '-std=c89', '-O2', '-shared', '-fPIC', '-Ipc/include',
             '-Ipc/include/game', 'pc/hal/cdxa_decode.c', '-o', str(binary)],
            cwd=ROOT, timeout=30).require_ok()
        cls.decoder = ctypes.CDLL(str(binary))
        cls.decoder.cdxa_open.argtypes = [ctypes.c_char_p]
        cls.decoder.cdxa_open.restype = ctypes.c_void_p
        cls.decoder.cdxa_decode.argtypes = [ctypes.c_void_p,
                                           ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
        cls.decoder.cdxa_decode.restype = ctypes.c_int
        cls.decoder.cdxa_rewind.argtypes = [ctypes.c_void_p]
        cls.decoder.cdxa_close.argtypes = [ctypes.c_void_p]

    def check_format(self, stereo, eight_bit, half_rate):
        source = self.path / 'synthetic.xa'
        source.write_bytes(sector_fixture(stereo, eight_bit, half_rate))
        expected = expected_pcm(stereo, eight_bit, half_rate)
        stream = self.decoder.cdxa_open(str(source).encode())
        self.assertTrue(stream)
        try:
            for chunk in (len(expected) + 32, 66):
                self.decoder.cdxa_rewind(stream)
                buffer = (ctypes.c_int16 * chunk)()
                decoded = []
                while True:
                    count = self.decoder.cdxa_decode(stream, buffer, chunk)
                    if not count:
                        break
                    decoded.extend(buffer[:count])
                self.assertEqual(len(decoded), len(expected))
                mismatch = next((index for index, pair in enumerate(zip(decoded, expected))
                                 if pair[0] != pair[1]), None)
                self.assertIsNone(mismatch, f'first wrong sample {mismatch}: '
                                  f'{decoded[mismatch]} != {expected[mismatch]}'
                                  if mismatch is not None else '')
        finally:
            self.decoder.cdxa_close(stream)

    def test_stereo_4bit_all_rates(self):
        for half_rate in (False, True):
            with self.subTest(half_rate=half_rate):
                self.check_format(True, False, half_rate)

    def test_stereo_8bit_all_rates(self):
        for half_rate in (False, True):
            with self.subTest(half_rate=half_rate):
                self.check_format(True, True, half_rate)

    def test_mono_4bit_all_rates(self):
        for half_rate in (False, True):
            with self.subTest(half_rate=half_rate):
                self.check_format(False, False, half_rate)

    def test_mono_8bit_all_rates(self):
        for half_rate in (False, True):
            with self.subTest(half_rate=half_rate):
                self.check_format(False, True, half_rate)


if __name__ == '__main__':
    unittest.main()
