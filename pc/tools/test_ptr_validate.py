# SPDX-License-Identifier: GPL-3.0-or-later
"""Host transfer guards must validate the complete mapped range, not 64 pages."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int pc_ptr_is_mapped(const void *pointer, size_t bytes);

int main(int argc, char **argv) {
    long page_size;
    size_t length;
    unsigned char *memory;
    int result;

    (void)argc;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 2;
    length = (size_t)page_size * 66;
    memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 2;
    memory[0] = 99;
    if (!strcmp(argv[1], "hole")) {
        if (munmap(memory + (size_t)page_size * 64,
                   (size_t)page_size)) return 2;
        result = pc_ptr_is_mapped(memory, length) == 0;
    } else if (!strcmp(argv[1], "overflow")) {
        result = pc_ptr_is_mapped(memory, (size_t)-1) == 0;
    } else if (!strcmp(argv[1], "null")) {
        result = !pc_ptr_is_mapped(NULL, 1) &&
                  pc_ptr_is_mapped(memory, 0);
    } else {
        result = pc_ptr_is_mapped(memory + 1, length - 1) &&
                  pc_ptr_is_mapped(memory, (size_t)page_size * 64);
    }
    if (memory[0] != 99) return 3;
    /* This process owns the entire mapping, including the deliberate hole. */
    if (munmap(memory, length)) return 2;
    return result ? 0 : 1;
}
'''


class PointerRangeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = directory / 'range.c'
        source.write_text(HARNESS)
        cls.binary = directory / 'range'
        run(['cc', '-std=c89', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
             str(source), 'pc/hal/ptr_validate.c', '-o', str(cls.binary)],
            cwd=ROOT, timeout=30).require_ok()

    def check_case(self, case):
        run([str(self.binary), case], timeout=10).require_ok()

    def test_hole_after_page_64_is_rejected(self):
        self.check_case('hole')

    def test_overflowing_pointer_range_is_rejected(self):
        self.check_case('overflow')

    def test_full_unaligned_mapping_is_accepted(self):
        self.check_case('mapped')

    def test_null_is_rejected_and_zero_size_keeps_point_query(self):
        self.check_case('null')


if __name__ == '__main__':
    unittest.main()
