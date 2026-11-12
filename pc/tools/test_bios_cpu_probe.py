# SPDX-License-Identifier: GPL-3.0-or-later
"""The diagnostic boots only authored code and uses reproducible ISO records."""
from pathlib import Path
import struct
import tempfile
import unittest
from bios_cpu_probe import build, iso_image


class ProbeTests(unittest.TestCase):
    def test_build_is_deterministic_and_has_consistent_executable_extent(self):
        with tempfile.TemporaryDirectory() as temporary:
            a, b = Path(temporary)/"a", Path(temporary)/"b"
            manifest = build(a)
            self.assertEqual(build(b), manifest)
            image = (a/"probe.iso").read_bytes()
            self.assertEqual(image, (b/"probe.iso").read_bytes())
            self.assertEqual(len(image), 1024*2048)
            self.assertEqual(image[16*2048:16*2048+7], b"\x01CD001\x01")
            self.assertIn(b"EVENT = 32\r\n", image[21*2048:22*2048])
            exe = (a/"probe.exe").read_bytes()
            self.assertEqual(image[22*2048:22*2048+len(exe)], exe)
            self.assertEqual(struct.unpack_from("<I", exe, 0x18)[0], 0x80010000)
            self.assertEqual(struct.unpack_from("<I", exe, 0x1c)[0], len(exe)-2048)
            self.assertLessEqual(len(exe)-2048, 0x8000)
            self.assertEqual(len(manifest["cases"]), 174)
            with self.assertRaises(FileExistsError):
                build(a)

    def test_invalid_kernel_capacity_or_executable_is_rejected(self):
        for events, threads in ((0, 8), (257, 8), (50, 0), (50, 33)):
            with self.assertRaises(ValueError):
                iso_image(b"PS-X EXE"+bytes(2040), events, threads)
        with self.assertRaises(ValueError):
            iso_image(b"not an executable")
