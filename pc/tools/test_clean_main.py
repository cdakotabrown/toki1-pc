# SPDX-License-Identifier: GPL-3.0-or-later
"""Clean-main relocation isolation and failed-cache recovery regressions."""
import argparse
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from contextlib import redirect_stderr
import io
import multiprocessing
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP  # noqa: E402
sys.path.insert(0, str(DECOMP / 'tools/build'))   # toki1-decomp's build tools
import build_clean_main as clean  # noqa: E402
import build_level3_c as level3  # noqa: E402

BARRIER = None


def fake_compile(source, name, address, **kwargs):
    # A real relocation-bearing self-jal: only its own function address
    # can resolve it. Force overlapping metadata publication deterministically.
    level3.LAST_OBJ_INFO = {
        'func_addr': address, 'strip': 0,
        'relocs': {'.text': [[0, level3.R_MIPS_26, '.text', 'section', '.text', 0]]},
    }
    BARRIER.wait(timeout=10)
    return struct.pack('<I', 0x0c000000)


def install_fake(barrier):
    global BARRIER
    BARRIER = barrier
    clean.compile_c_function = fake_compile


class CleanMainTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        source = self.directory / 'fixture.c'
        source.write_text('void fixture(void) { }\n')
        self.exe = bytes(clean.FILE_BASE)
        self.entries = []
        for index, name in enumerate(('first', 'second')):
            address = clean.TEXT_BASE + index * 4
            self.entries.append(dict(name=name, address=hex(address), size=4, source=str(source)))
            self.exe += struct.pack('<I', 0x0c000000 | ((address >> 2) & 0x3ffffff))
        self.items = [(self.exe, entry, 'fixture-key', True, False) for entry in self.entries]

    def test_all_cores_default_and_positive_job_count(self):
        class Parsed(Exception):
            pass

        original = argparse.ArgumentParser.parse_args
        seen = []

        def capture(parser):
            seen.append(original(parser, []).jobs)
            raise Parsed()

        with patch.object(argparse.ArgumentParser, 'parse_args', capture):
            with self.assertRaises(Parsed):
                clean.main()
        self.assertEqual(seen, [os.cpu_count() or 2])
        with patch.object(sys, 'argv', ['clean-main', '--jobs', '0']), \
                redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as result:
                clean.main()
        self.assertEqual(result.exception.code, 2)

    def test_process_workers_keep_each_functions_relocation_metadata(self):
        context = multiprocessing.get_context('spawn')
        with ProcessPoolExecutor(max_workers=2, mp_context=context,
                                 initializer=install_fake, initargs=(context.Barrier(2),)) as pool:
            results = list(pool.map(clean.produce_cached_c, self.items))
        self.assertEqual([data for _, data in results],
                         [self.exe[clean.FILE_BASE:clean.FILE_BASE + 4], self.exe[-4:]])

    def test_old_threaded_handoff_is_a_negative_control(self):
        with patch.object(clean, 'compile_c_function', fake_compile), \
                patch(__name__ + '.BARRIER', threading.Barrier(2)), \
                patch.object(level3, 'LAST_OBJ_INFO'), patch.object(level3, 'RELOC_ERRORS', []):
            with ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(clean.produce_cached_c, self.items))
        self.assertEqual(sum(data is None for _, data in results), 1)

    def test_failed_cache_is_retried_and_old_version_invalidated(self):
        entry = self.entries[0]
        key = clean.cache_key(entry, self.exe)
        with patch.object(clean, 'CACHE_VERSION', '1'):
            self.assertNotEqual(clean.cache_key(entry, self.exe), key)
        with patch.object(clean, 'CACHE_DIR', self.directory):
            clean.cache_store(entry['name'], key, None)
            self.assertEqual(clean.cache_load(entry['name'], key), (None, False))
            clean.cache_store(entry['name'], key, b'valid')
            self.assertEqual(clean.cache_load(entry['name'], key), (b'valid', True))


if __name__ == '__main__':
    unittest.main()
