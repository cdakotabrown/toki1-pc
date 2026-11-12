# SPDX-License-Identifier: GPL-3.0-or-later
"""Sprite cel texture-page cache: the PC adapters share the retail tables.

The real producers run: matched tpage_buf_clear seeds the tpage_buf keys at
screen_w+0x1228, matched load_tpage_buf_lock locks a page, search_tpage(0x7FFF)
rebuilds the state table at screen_w+0x1128 each frame, and the PC adapters
search_tpage8 (8bpp) / search_tpage (4bpp) allocate from those tables. The
upload queue is recorded, not executed. TPAGE8_SOURCE / TPAGE4_SOURCE select
older sources for negative controls: the private-table search_tpage8
allocated page 0; the matched search_tpage reads the slot's key handle at the
host offset of an 8-byte pointer.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

u8 screen_w[0x2000];
u8 class_w[160 * 68];
static s32 cel_keys[4];

extern void tpage_buf_clear(void);
extern s32 search_tpage(s16, s32, u8);
extern s32 search_tpage8(s32, s32, u8);
extern void load_tpage_buf_lock(s32, s32, s32); /* load_tpage_buf_lock */

static int uploads;
static s32 last_key;
static u8 last_page, last_mode;
static u16 last_y;

void _sys_default_tpage_set(void) {}

s32 image_upload_queue_add_flags(u32 key, u8 page, u8 mode, u16 a3,
                                 u16 y, u16 w, u16 h)
{
    (void)a3;
    assert(w == 128 && h == 128);
    uploads++;
    last_key = (s32)key;
    last_page = page;
    last_mode = mode;
    last_y = y;
    return 1;
}

static void expect(const char *what, s32 got, s32 want)
{
    if (got != want) {
        fprintf(stderr, "%s: got %d, want %d\n", what, (int)got, (int)want);
        abort();
    }
}

int main(void)
{
    const s32 slot = 96;
    s32 page;

    cel_keys[0] = 0x101ce200;
    cel_keys[1] = 0x101d6200;
    cel_keys[2] = 0x101e2200;
    cel_keys[3] = 0x101e6200;
    *(u32 *)(class_w + slot * 68 + 52) = (u32)(uintptr_t)cel_keys;

    /* Title setup: clear the cache directory and lock a card page. */
    tpage_buf_clear();
    load_tpage_buf_lock(21, cel_keys[2], 1);
    expect("lock upload", uploads, 1);
    uploads = 0;

    /* Frame 1: sprite_slot_disp opens with search_tpage(0x7FFF). */
    search_tpage(0x7FFF, 0, 0);
    page = search_tpage8(0x0012, slot, 2);   /* key index 0 */
    expect("first free page (retail VRAM 320,0)", page, 5);
    expect("upload queued", uploads, 1);
    expect("upload key", last_key, cel_keys[0]);
    expect("upload page", last_page, 5);
    expect("upload mode", last_mode, 2);
    expect("upload y", last_y, 0);

    page = search_tpage8(0x0034, slot, 2);   /* same key, other cel */
    expect("same key hits", page, 5);
    expect("no second upload", uploads, 1);

    page = search_tpage8(0x0100, slot, 2);   /* key index 1 */
    expect("next free page", page, 6);
    expect("second key uploads", uploads, 2);

    page = search_tpage8(0x0200, slot, 2);   /* key index 2: locked */
    expect("locked page reused", page, 21);
    expect("locked page not uploaded", uploads, 2);

    /* Frame 2: the cache resets from the keys and allocates again. */
    search_tpage(0x7FFF, 0, 0);
    page = search_tpage8(0x0100, slot, 2);
    expect("frame 2 first free page", page, 5);
    expect("frame 2 re-upload", uploads, 3);
    expect("frame 2 key", last_key, cel_keys[1]);

    /* 4bpp cels index the same key table with id >> 6. */
    page = search_tpage(0x0080, slot, 1);    /* key index 2: locked */
    expect("4bpp locked page", page, 21);
    page = search_tpage(0x00C0, slot, 1);    /* key index 3 */
    expect("4bpp next free page", page, 6);
    expect("4bpp upload", uploads, 4);
    expect("4bpp key", last_key, cel_keys[3]);
    expect("4bpp mode", last_mode, 1);
    return 0;
}
"""


class TpageCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        harness = folder / 'tpage.c'
        harness.write_text(HARNESS)
        cls.binary = folder / 'tpage'
        adapter = Path(os.environ.get('TPAGE8_SOURCE',
                       ROOT / 'src/main/pc-port/search_tpage8.c'))
        run(['cc', '-std=gnu17', '-fpermissive', '-w', '-O2', '-fno-pie',
             '-no-pie', '-fno-strict-aliasing', '-include',
             'pc/include/game/types.h', '-Ipc/include/game', '-Ipc/include',
             '-I' + decomp('include'), *shlex.split(os.environ.get('TPAGE_CFLAGS', '')),
             str(harness), str(adapter.resolve()),
             decomp('src/main/matched/tpage_buf_clear.c'),
             str(Path(os.environ.get('TPAGE4_SOURCE',
                 ROOT / 'src/main/pc-port/search_tpage.c')).resolve()),
             decomp('src/main/matched/load_tpage_buf_lock.c'),
             '-Wl,--defsym=tpage_buf=screen_w+0x1228',
             '-o', str(cls.binary)], cwd=ROOT, timeout=60).require_ok()

    def test_allocates_from_the_shared_retail_tables(self):
        run([str(self.binary)], timeout=10).require_ok()


if __name__ == '__main__':
    unittest.main()
