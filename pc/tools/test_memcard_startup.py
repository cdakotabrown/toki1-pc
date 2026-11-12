# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise card requests through the real shared clock and BIOS events."""
from pathlib import Path
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]

SOURCE = r'''
#include <assert.h>
#include "pc/hal/memcard.c"
#include "file_id.h"
unsigned int g_pc_debug_flags;
int g_pc_debug_initialized;
u8 D_800FF869;
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
void *hal_ps1_ptr(const void *p) { return (void *)p; }
int pc_ptr_is_mapped(const void *p, unsigned long n) { return p != 0; }
extern void hal_bios_vblank_tick(void);
extern s32 OpenEvent(u32, u32, u32, void (*)(void));
extern s32 EnableEvent(s32), DisableEvent(s32), CloseEvent(s32), TestEvent(s32);
extern s32 _card_auto(s32), _card_load(s32);
extern s32 delete(const char *);
extern void cd_hal_vblank_tick(void);
extern u32 hal_clock_vblanks(void);
static int callbacks, hw_events, sw_events, absent_events;
static u32 mask_value = 13;
static u32 mask_writes[8];
static int mask_writes_n, acknowledged;
static void vblank(void) { callbacks++; }
static void hw(void) { hw_events++; }
static void sw(void) { sw_events++; }
static void absent(void) { absent_events++; }
static void resubmit(void) {
    assert(memcard_card_info(0) == 1);
}
void hal_vblank_interrupt(void) { hal_bios_vblank_tick(); }
/* The VBlank mask window `_bu_init` opens on retail. */
u32 GetIntrMask(void) { return mask_value; }
u32 SetIntrMask(s32 value) {
    u32 old = mask_value;
    if (mask_writes_n < 8) mask_writes[mask_writes_n++] = (u32)value;
    mask_value = (u32)value & 0x7ffu;
    return old;
}
void hal_irq_ack(u16 bits) { (void)bits; acknowledged++; }
static s32 listen(u32 cls, u32 spec, void (*fn)(void)) {
    s32 ev;
    ev = OpenEvent(cls, spec, fn ? 0x1000 : 0x2000, fn);
    EnableEvent(ev);
    return ev;
}
static void finish(void) {
    int i;
    for (i = 0; i < 100 &&
         (g_requests[0].operation || g_requests[1].operation); i++)
        cd_hal_vblank_tick();
    assert(i < 100);
}
/* _card_status(0) once per frame, 100k ticks after each VBlank (as the
 * game's card_status_wait polls), until it reads ready. */
static void poll(int *statuses, int *n) {
    *n = 0;
    do {
        hal_cpu_spend(hal_cpu_ticks_to_vblank());
        hal_cpu_spend(100000);
        statuses[(*n)++] = (int)memcard_card_status(0);
    } while (statuses[*n - 1] != 1 && *n < 60);
}
static u8 page[384], back[384];
int main(int argc, char **argv) {
    u8 data[128], copy[128], backup[128];
    u64 before;
    s32 ev;
    int fd, i, n, entry, statuses[64];
    listen(0xF2000003u, 2, vblank);
    listen(CARD_HW_EVENT, CARD_IOE, hw);
    listen(CARD_SW_EVENT, CARD_IOE, sw);
    listen(CARD_SW_EVENT, CARD_TIMEOUT, absent);
    memcard_card_init();
    memcard_card_start();
    if (argv[1][0] == '0') {
        memcpy(backup, dir_frame(0), 128);
        memset(dir_frame(63), 0x5a, 128);
        mask_writes_n = 0;
        acknowledged = 0;
        assert(memcard_bu_init() == 0);
        /* Retail runs `_bu_init` with the I_MASK VBlank bit clear and
         * acknowledges the latched edge before restoring the mask. */
        assert(mask_writes_n == 2);
        assert(mask_writes[0] == 12 && mask_writes[1] == 13);
        assert(acknowledged == 1 && mask_value == 13);
        assert(hw_events == 37 && sw_events == 1 && absent_events == 1);
        assert(callbacks == 76 && hal_clock_vblanks() == 76);
        assert(memcmp(backup, dir_frame(63), 128) == 0);
        assert(memcmp(g_requests[0].directory[35], dir_frame(35), 128) == 0);
        assert(memcard_card_status(0) == 1);
        assert(memcard_card_status(1) == 0x11);
        assert(D_800FF869 == 0);  /* BIOS must not write a gameplay flag */
        before = hal_clock_ticks();
        assert(_card_auto(0) == 1 && _card_auto(1) == 0);
        assert(hal_clock_ticks() == before && hw_events == 37);
        assert(memcard_file_open("bu10:ABSENT", 0x10202) == -1);
        /* FNBLOCK (0x4) is not FCREAT (0x200): no file appears */
        assert(memcard_file_open("bu00:NOCREATE", 6) == -1);
        /* FCREAT asking for two blocks: unsupported, refused */
        assert(memcard_file_open("bu00:TWO", 0x20200) == -1);
        /* card_file_make: FCREAT, one block; then a write open */
        fd = memcard_file_open("bu00:TEST", 0x10200);
        assert(fd >= 0 && memcard_file_close(fd) == 0);
        fd = memcard_file_open("bu00:TEST", 0x8002);
        assert(fd == 2);                    /* retail: the first file is 2 */
        entry = dir_find("TEST", 1);
        for (i = 0; i < 384; i++) page[i] = (u8)(i * 7 + 1);
        hw_events = sw_events = 0;
        /* FASYNC write: returns at once, the card takes it sector by sector */
        assert(memcard_file_write(fd, page, 384) == 384);
        assert(memcard_card_status(0) == 4);
        assert(block_data(entry)[0] != page[0]);
        assert(memcard_file_close(fd) == 0);  /* the game closes on every poll */
        poll(statuses, &n);
        /* one sector per slot-0 turn (every second VBlank): 4 while the
         * three sectors go, 8 for the closing request, then ready */
        assert(statuses[n - 1] == 1 && statuses[n - 2] == 8);
        for (i = 0; i < n - 2; i++) assert(statuses[i] == 4);
        assert(n >= 6 && n <= 8);           /* 3 sectors + close, 2 VBlanks each */
        assert(memcmp(block_data(entry), page, 384) == 0);
        assert(hw_events == 4 && sw_events == 1);
        /* FASYNC read: status 2, no closing request */
        fd = memcard_file_open("bu00:TEST", 0x8001);
        memset(back, 0, sizeof(back));
        assert(memcard_file_read(fd, back, 256) == 256);
        assert(memcard_card_status(0) == 2 && back[0] == 0);
        /* a second request while the slot is busy is refused */
        assert(memcard_file_read(fd, back + 256, 128) == -1);
        poll(statuses, &n);
        assert(statuses[n - 1] == 1 && statuses[n - 2] == 2);
        assert(memcmp(back, page, 256) == 0 && sw_events == 2);
        assert(memcard_file_close(fd) == 0);
        /* without FASYNC the call returns when the bytes are there */
        fd = memcard_file_open("bu00:TEST", 0x0001);
        before = hal_clock_ticks();
        memset(back, 0, sizeof(back));
        assert(memcard_file_read(fd, back, 128) == 128);
        assert(memcmp(back, page, 128) == 0 && memcard_card_status(0) == 1);
        assert(hal_clock_ticks() - before > 228000);
        /* B(0x45) FileDelete: refused while the file is open; then the
         * frame turns 0xA1 (deleted, the name kept), the file no longer
         * opens and its block is free for the next create */
        assert(delete("bu00:TEST") == 0);
        assert(memcard_file_close(fd) == 0);
        assert(delete("bu00:NOSUCH") == 0);
        assert(delete("bu00:TEST") == 1);
        assert(dir_frame(entry)[0] == 0xA1 && strcmp(de_name(entry), "TEST") == 0);
        assert(xor_checksum(dir_frame(entry), 127) == dir_frame(entry)[127]);
        assert(dir_find("TEST", 1) < 0 && dir_find("TEST", 0) < 0);
        assert(memcard_file_open("bu00:TEST", 0x0001) == -1);
        assert(delete("bu00:TEST") == 0);
        fd = memcard_file_open("bu00:OTHER", 0x10200);
        assert(fd >= 0 && dir_find("OTHER", 1) == entry);
        assert(memcard_file_close(fd) == 0);
    } else if (argv[1][0] == '1') {
        memcard_new_card();
        memset(copy, 0xcc, 128);
        before = hal_clock_ticks();
        assert(memcard_card_read(0, 0, copy) == 1);
        assert(memcard_card_read(0, 1, data) == 0);
        assert(memcard_card_read(1, 0, data) == 0);
        assert(memcard_card_read(0, 1024, data) == 0);
        assert(memcard_card_read(0, -1, data) == 0);
        assert(hal_clock_ticks() == before && copy[0] == 0xcc);
        hal_cpu_spend(hal_cpu_ticks_to_vblank());  /* other slot */
        assert(hw_events == 0 && copy[0] == 0xcc);
        hal_cpu_spend(hal_cpu_ticks_to_vblank());  /* inserted slot begins */
        assert(hw_events == 0 && copy[0] == 0xcc);
        assert(memcard_next_event() > hal_clock_ticks());
        hal_cpu_spend(227999);
        assert(hw_events == 0 && copy[0] == 0xcc);
        hal_cpu_spend(1);
        assert(hw_events == 1 && copy[0] == 'M' && copy[1] == 'C');
        memset(data, 0x35, 128);
        memcard_new_card();
        assert(memcard_card_write(0, 64, data) == 1);
        assert(dir_frame(64)[0] != 0x35);
        finish();
        assert(dir_frame(64)[0] == 0x35);
        memset(copy, 0, 128);
        assert(memcard_card_read(0, 64, copy) == 1);
        for (i = 0; i < 100000 && !(memcard_card_status(0) & 1); i++) {}
        assert(i < 100000 && memcmp(data, copy, 128) == 0);
        assert(memcard_card_read(0x10, 0, copy) == 1);
        finish();
        assert(memcard_card_status(1) == 0x11);
    } else if (argv[1][0] == '2') {
        /* A corrupt directory must terminate with ERROR, not a false load. */
        ev = listen(CARD_SW_EVENT, CARD_ERROR, NULL);
        dir_frame(3)[20] ^= 1;
        assert(_card_load(0) == 1 && TestEvent(ev) == 0);
        finish();
        assert(hw_events == 5 && sw_events == 0);
        assert(memcard_card_status(0) == 0x21);
        assert(TestEvent(ev) == 1 && TestEvent(ev) == 0);
    } else if (argv[1][0] == '3') {
        ev = listen(CARD_HW_EVENT, CARD_IOE, NULL);
        hal_bios_deliver_event(CARD_HW_EVENT, CARD_IOE);
        assert(TestEvent(ev) == 1 && TestEvent(ev) == 0);
        DisableEvent(ev);
        hal_bios_deliver_event(CARD_HW_EVENT, CARD_IOE);
        assert(TestEvent(ev) == 0);
        EnableEvent(ev);
        assert(TestEvent(ev) == 0);
        CloseEvent(ev);
        hal_bios_deliver_event(CARD_HW_EVENT, CARD_IOE);
        assert(TestEvent(ev) == 0 && TestEvent(-1) == 0);
        ev = listen(CARD_SW_EVENT, CARD_NEW, NULL);
        assert(memcard_card_info(0) == 1 && TestEvent(ev) == 0);
        finish();
        assert(TestEvent(ev) == 1 && TestEvent(ev) == 0);
        ev = listen(CARD_SW_EVENT, CARD_TIMEOUT, NULL);
        assert(memcard_card_info(0x10) == 1 && TestEvent(ev) == 0);
        while (g_card_active_slot < 0)
            hal_cpu_spend(hal_cpu_ticks_to_vblank());
        hal_cpu_spend(hal_cpu_ticks_to_vblank());
        assert(TestEvent(ev) == 0 && absent_events == 0);
        hal_cpu_spend(19999);
        assert(TestEvent(ev) == 0);
        hal_cpu_spend(1);
        assert(TestEvent(ev) == 1 && absent_events == 1);
    } else {
        /* A completion callback may register the next asynchronous load.
         * That new request must not inherit the previous request's event. */
        memcard_bu_init();
        ev = listen(CARD_HW_EVENT, CARD_IOE, resubmit);
        memset(copy, 0, 128);
        assert(memcard_card_read(0, 0, copy) == 1);
        while (copy[0] != 'M')
            hal_cpu_spend(hal_cpu_ticks_to_vblank());
        assert(sw_events == 1 && g_requests[0].operation == 8);
        DisableEvent(ev);
        finish();
        assert(sw_events == 2 && g_requests[0].operation == 0);
    }
    puts("card contract passed");
    return 0;
}
'''


class MemcardStartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.temp.name)
        (cls.path / 'test.c').write_text(SOURCE)
        run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-O2', '-fno-pie', '-no-pie',
             '-ffunction-sections', '-fdata-sections', '-I', str(ROOT),
             '-I', str(ROOT / 'pc/include/game'), '-I', str(ROOT / 'pc/include'),
             '-I', str(ROOT / 'pc/hal'),
             str(cls.path / 'test.c'), str(ROOT / 'pc/hal/cd.c'),
             str(ROOT / 'src/main/pc-port/bios_trampolines.c'),
             '-Wl,--gc-sections', '-lm', '-o', str(cls.path / 'test')],
            timeout=30).require_ok()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def exercise(self, number):
        with tempfile.TemporaryDirectory() as work:
            result = run([str(self.path / 'test'), str(number)], cwd=work,
                         timeout=10)
            result.require_ok()
            self.assertIn('card contract passed', result.stdout)

    def test_startup_transfers_metadata_and_probes_absent_slot(self):
        self.exercise(0)

    def test_async_requests_transfer_only_at_completion(self):
        self.exercise(1)

    def test_corrupt_directory_reports_error(self):
        self.exercise(2)

    def test_event_delivery_and_connection_tests(self):
        self.exercise(3)

    def test_callback_can_register_the_next_request(self):
        self.exercise(4)


if __name__ == '__main__':
    unittest.main()
