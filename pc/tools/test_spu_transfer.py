# SPDX-License-Identifier: GPL-3.0-or-later
"""SPU RAM visibility and BIOS completion through the real shared clock."""
from pathlib import Path
import os
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "hal.h"
#include "spu_transfer.h"
#include "file_id.h"
u32 _spu_EVdma, _spu_transferCallback, _SpuMemList;
u32 _spu_rev_startaddr[10];
s32 _spu_inTransfer, _spu_trans_mode, _spu_AllocBlockNum;
s32 spu_transMode;
u16 spu_tsa;
unsigned int g_pc_debug_flags;
int g_pc_debug_initialized;
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_overlay_set_loaded(const char *p) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
void *hal_ps1_ptr(const void *p) { return (void *)p; }
int pc_ptr_is_mapped(const void *p, unsigned long n) { return p != 0; }
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) {}
void memcard_vblank_tick(void) {}
extern void hal_bios_vblank_tick(void);
extern u64 hal_clock_ticks(void);
extern void hal_cpu_spend(u32);
extern u32 hal_clock_vblanks(void), hal_clock_ticks_per_frame(void);
extern s32 TestEvent(s32), WaitEvent(s32), DisableEvent(s32), CloseEvent(s32);
extern s32 OpenEvent(u32, u32, u32, void (*)(void)), EnableEvent(s32);
extern void hal_bios_deliver_event(u32, u32);
static u8 ram[0x80000], data[1024];
static u32 allocation[4];
static int commits, callbacks, vblanks;
static u32 sizes[64], addresses[64];
static u64 commit_ticks[64];
void spu_transfer_commit(const u8 *p, u32 address, u32 size) {
    u32 i;
    assert(commits < 64);
    sizes[commits] = size; addresses[commits] = address;
    commit_ticks[commits++] = hal_clock_ticks();
    for (i = 0; i < size; i++) ram[(address+i)&0x7ffff] = p[i];
}
/* DMA4 reads: SPU RAM -> main RAM at the transfer's completion. */
static int readbacks;
static u64 readback_ticks[8];
void spu_transfer_readback(u8 *dst, u32 address, u32 size) {
    u32 i;
    assert(readbacks < 8);
    readback_ticks[readbacks++] = hal_clock_ticks();
    for (i = 0; i < size; i++) dst[i] = ram[(address+i)&0x7ffff];
}
static u64 deliver_tick;
static void read_callback(void) { deliver_tick = hal_clock_ticks(); }
#define IO32(a) (*(volatile u32 *)(uintptr_t)(a))
#define IO16(a) (*(volatile u16 *)(uintptr_t)(a))
/* The register writes of the matched _spu_r (src/main/matched/_spu_r.c). */
static void program_read(void *dst, u32 spu_address, u32 bytes) {
    u32 blocks = (bytes >> 6) + ((bytes & 63) != 0);
    IO16(0x1F801DAA) = (IO16(0x1F801DAA) & 0xFFCF) | 0x30;
    IO16(0x1F801DA6) = (u16)(spu_address >> 3);
    IO32(0x1F8010F4) |= 0x900000;
    IO32(0x1F8010F0) |= 0x80000;
    IO32(0x1F8010C0) = (u32)(uintptr_t)dst;
    IO32(0x1F8010C4) = (blocks << 16) | 0x10;
    IO32(0x1F8010C8) = 0x01000200;
}
static u8 readbuf[1024] __attribute__((aligned(4)));
void hal_vblank_interrupt(void) { vblanks++; hal_bios_vblank_tick(); }
static void advance_event(void) {
    u64 due = spu_transfer_next_event();
    assert(due != (u64)-1 && due >= hal_clock_ticks());
    hal_cpu_spend((u32)(due-hal_clock_ticks()));
}
static u64 callback_ticks[4];
static void callback(void) {
    callback_ticks[callbacks] = hal_clock_ticks();
    callbacks++;
    if (callbacks == 1) {
        assert(spu_transfer_address(0x3000) == 0x3000);
        assert(spu_transfer_write(data, 64) == 64);
    }
}
static s32 submit_event;
static void stop_on_vblank(void) {
    DisableEvent(submit_event);
    spu_transfer_shutdown();
}
static void submit_on_vblank(void) {
    DisableEvent(submit_event);
    callbacks++;
    assert(spu_transfer_write(data, 64) == 64);
}
static void close_on_vblank(void) { CloseEvent((s32)_spu_EVdma); }
int main(int argc, char **argv) {
    u64 start, before;
    u32 i;
    s32 ev;
    memset(ram, 0xa5, sizeof(ram));
    memset(data, 0x3c, sizeof(data));
    /* The I/O page the SDK cells address (pc/src/main.c maps it too). */
    assert(mmap((void *)0x1F801000, 0x1000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0)
           == (void *)0x1F801000);
    spu_transfer_init();
    if (argv[1][0] == '0') {
        assert(spu_transfer_address(0x1011) == 0x1018);
        assert(spu_transfer_address(0x7fff9) == 0);
        start = hal_clock_ticks();
        assert(spu_transfer_write(data, 1024) == 1024);
        assert(hal_clock_ticks() > start && commits == 0);
        before = hal_clock_ticks();
        assert(spu_transfer_write(data, 64) == 0);
        assert(hal_clock_ticks() == before);
        hal_cpu_spend((u32)(spu_transfer_next_event()-hal_clock_ticks()-1));
        assert(ram[0x1018] == 0xa5 && TestEvent(_spu_EVdma) == 0);
        hal_cpu_spend(1);
        assert(commits == 1 && ram[0x1018] == 0x3c);
        /* The completion interrupt's exception: the CPU returns 19,114
         * ticks after the DMA ends, the event delivered inside it. */
        assert(hal_clock_ticks() - commit_ticks[0] == 19114u);
        assert(spu_transfer_completed(1) == 1);
        assert(TestEvent(_spu_EVdma) == 0);
        assert(spu_transfer_completed(0) == 1);
        assert(ram[0x1017] == 0xa5 && ram[0x1418] == 0xa5);
        /* _spu_w rounds a partial block up: 63 bytes move one 64-byte block. */
        assert(spu_transfer_write(data, 63) == 63);
        assert(spu_transfer_completed(1) == 1);
        assert(sizes[commits-1] == 64 && ram[0x1018+63] == 0x3c);
        assert(spu_transfer_write(NULL, 64) == 0);
        assert(spu_transfer_write(data, 0) == 0);
        assert(spu_transfer_address(0x7fff8) == 0x7fff8);
        assert(spu_transfer_write(data, 64) == 64);
        assert(spu_transfer_completed(1) == 1);
        assert(ram[0x7fff8] == 0x3c && ram[0] == 0x3c);
    } else if (argv[1][0] == '1') {
        before = hal_clock_ticks();
        hal_bios_deliver_event(0xf0000009, 0x20);
        assert(WaitEvent(_spu_EVdma) == 1);
        assert(hal_clock_ticks() == before && TestEvent(_spu_EVdma) == 0);
        assert(WaitEvent(-1) == 0);
        DisableEvent(_spu_EVdma);
        assert(WaitEvent(_spu_EVdma) == 0);
        spu_transfer_init();
        hal_cpu_spend(hal_clock_ticks_per_frame()-1000);
        assert(spu_transfer_write(data, 64) == 64);
        assert(spu_transfer_completed(1) == 1);
        assert(vblanks == 1 && hal_clock_vblanks() == 1);
        assert(commits == 1);
        spu_transfer_init();
        ev = OpenEvent(0xf2000003, 2, 0x1000, close_on_vblank);
        EnableEvent(ev);
        assert(WaitEvent(_spu_EVdma) == 0);
        assert(vblanks == 2);
    } else if (argv[1][0] == '2') {
        _spu_rev_startaddr[3] = (0x80000-18496)/8;
        assert(spu_transfer_clear(-1) == -1);
        assert(spu_transfer_clear(10) == -1);
        _spu_rev_startaddr[4] = 0x2000f6f8u;
        assert(spu_transfer_clear(4) == -1);
        assert(commits == 0);
        allocation[0] = 0x7f000; allocation[1] = 0x1000;
        allocation[2] = 0x40000000;
        _SpuMemList = (u32)(uintptr_t)allocation; _spu_AllocBlockNum = 2;
        assert(spu_transfer_clear(3) == -1 && commits == 0);
        _SpuMemList = 0;
        assert(spu_transfer_mode(1) == 1);
        assert(spu_transfer_clear(3) == 0);
        assert(commits == 19 && sizes[18] == 64);
        for (i = 0; i < 18; i++) assert(sizes[i] == 1024);
        for (i = 0; i < 0x80000-18496; i++) assert(ram[i] == 0xa5);
        for (; i < 0x80000; i++) assert(ram[i] == 0);
        assert(addresses[0] == 0x80000-18496);
        assert(addresses[18] == 0x80000-64);
        assert(vblanks > 0 && _spu_trans_mode == 1 && spu_transMode == 0);
        assert(spu_transfer_clear(0) == 0 && commits == 20);
        assert(sizes[19] == 128 && addresses[19] == 0x7ff80);
    } else if (argv[1][0] == '3') {
        /* The callback's new submission crosses the next VBlank edge. */
        hal_cpu_spend(hal_clock_ticks_per_frame()-50000u);
        _spu_transferCallback = (u32)(uintptr_t)callback;
        assert(spu_transfer_write(data, 64) == 64);
        advance_event();
        assert(callbacks == 1 && commits == 1);
        /* _spu_interruptDMA calls back 17,619 ticks into the exception */
        assert(callback_ticks[0] - commit_ticks[0] == 17619u);
        assert(spu_transfer_completed(0) == 1);
        assert(TestEvent(_spu_EVdma) == 0);
        advance_event();
        assert(callbacks == 2 && commits == 2);
        assert(vblanks == 1 && hal_clock_vblanks() == 1);
        assert(TestEvent(_spu_EVdma) == 0);
        assert(ram[0x3000] == 0x3c);
        assert(spu_transfer_next_event() == (u64)-1);
    } else if (argv[1][0] == '4') {
        assert(spu_transfer_mode(1) == 1);
        assert(spu_transfer_write(data, 64) == 64);
        assert(commits == 1 && spu_transfer_completed(1) == 1);
        assert(TestEvent(_spu_EVdma) == 0);
        assert(spu_transfer_mode(9) == 0 && spu_transMode == 1);
        assert(spu_transfer_write(data, 64) == 64);
        before = hal_clock_ticks();
        assert(WaitEvent(_spu_EVdma) == 1);
        assert(commits == 2 && hal_clock_ticks() > before);
        /* WaitEvent polls in 128-tick steps: it returns at the first poll
         * after the exception's exit (19,114 after the DMA ends). */
        assert(hal_clock_ticks() - commit_ticks[1] >= 19114u &&
               hal_clock_ticks() - commit_ticks[1] < 19114u + 128u);
        assert(TestEvent(_spu_EVdma) == 0);
        assert(spu_transfer_write(data, 64) == 64);
        spu_transfer_shutdown();
        assert(spu_transfer_next_event() == (u64)-1);
        before = commits;
        hal_cpu_spend(hal_clock_ticks_per_frame());
        assert(commits == (int)before);
    }
    if (argv[1][0] == '5') {
        submit_event = OpenEvent(0xf2000003, 2, 0x1000, submit_on_vblank);
        EnableEvent(submit_event);
        hal_cpu_spend(hal_clock_ticks_per_frame());
        assert(callbacks == 1 && vblanks == 1);
        assert(hal_clock_vblanks() == 1);
        assert(spu_transfer_completed(1) == 1 && commits == 1);
    }
    if (argv[1][0] == '6') {
        submit_event = OpenEvent(0xf2000003, 2, 0x1000, stop_on_vblank);
        EnableEvent(submit_event);
        hal_cpu_spend(hal_clock_ticks_per_frame()-1000u);
        assert(spu_transfer_write(data, 64) == 0);
        assert(spu_transfer_next_event() == (u64)-1);
        hal_cpu_spend(hal_clock_ticks_per_frame());
        assert(commits == 0 && ram[0] == 0xa5);
    }
    if (argv[1][0] == '7') {
        for (i = 0; i < 512; i++) ram[0x400 + i] = (u8)i;
        memset(readbuf, 0xee, sizeof(readbuf));
        /* Not started while DMA4 is disabled in DPCR or the SPU is not in
         * DMA-read mode; the channel stays pending. */
        program_read(readbuf, 0x400, 512);
        IO32(0x1F8010F0) &= ~0x80000u;
        assert(spu_transfer_next_event() == (u64)-1);
        IO32(0x1F8010F0) |= 0x80000u;
        IO16(0x1F801DAA) &= 0xFFCF;
        assert(spu_transfer_next_event() == (u64)-1);
        /* SpuRead then SpuIsTransferCompleted(1): 16 ticks a halfword,
         * then _spu_interruptDMA without its two _waitfs loops. */
        program_read(readbuf, 0x400, 512);
        start = hal_clock_ticks();
        _spu_inTransfer = 0;
        assert(spu_transfer_completed(1) == 1);
        assert(readbacks == 1 && commits == 0);
        assert(readback_ticks[0] - start == 256u * 16u);
        assert(hal_clock_ticks() - readback_ticks[0] == 19114u - 2u * 7470u);
        for (i = 0; i < 512; i++) assert(readbuf[i] == (u8)i);
        assert(readbuf[512] == 0xee);
        assert(!(IO32(0x1F8010C8) & 0x01000000u));
        assert(IO32(0x1F8010C0) == (u32)(uintptr_t)readbuf + 512u);
        assert((IO32(0x1F8010C4) >> 16) == 0 && (IO16(0x1F801DAA) & 0x30) == 0);
        assert(spu_transfer_next_event() == (u64)-1);
        /* With a callback, it is called 17,619 - 2 x 7,470 into the
         * exception; a partial block moves a whole 64 bytes. */
        _spu_transferCallback = (u32)(uintptr_t)read_callback;
        program_read(readbuf, 0x400, 100);
        advance_event();
        assert(readbacks == 2 && deliver_tick - readback_ticks[1] == 17619u - 2u * 7470u);
        assert(readbuf[127] == 127 && readbuf[128] == 128);
        /* A DMA4 read cannot start over a write in flight. */
        _spu_transferCallback = 0;
        assert(spu_transfer_write(data, 64) == 64);
        before = spu_transfer_next_event();
        program_read(readbuf, 0x400, 64);
        assert(spu_transfer_next_event() == before);
        /* it starts once the write's interrupt has been handled */
        _spu_inTransfer = 0;
        assert(spu_transfer_completed(1) == 1 && commits == 1);
        assert(readbacks == 3 && readback_ticks[2] >= commit_ticks[0] + 17619u);
    }
    puts("SPU transfer contract passed");
    return 0;
}
'''


class SpuTransferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.path = Path(cls.temp.name)
        (cls.path / 'test.c').write_text(SOURCE)
        run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-O2',
             '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections',
             '-I', str(ROOT / 'pc/include/game'),
             '-I', str(ROOT / 'pc/include'), '-I', str(ROOT / 'pc/hal'),
             *shlex.split(os.environ.get('SPU_TRANSFER_CFLAGS', '')),
             str(cls.path / 'test.c'),
             str(Path(os.environ.get('SPU_TRANSFER_SOURCE',
                                     ROOT / 'pc/hal/spu_transfer.c'))),
             str(Path(os.environ.get('CD_SOURCE', ROOT / 'pc/hal/cd.c'))),
             str(ROOT / 'src/main/pc-port/bios_trampolines.c'),
             '-Wl,--gc-sections', '-lm', '-o', str(cls.path / 'test')],
            timeout=30).require_ok()

    def exercise(self, case):
        result = run([str(self.path / 'test'), str(case)], timeout=10)
        result.require_ok()
        self.assertIn('SPU transfer contract passed', result.stdout)

    def test_ram_visibility_busy_rejection_and_completion(self):
        self.exercise(0)

    def test_wait_event_consumption_vblank_and_closed_event(self):
        self.exercise(1)

    def test_reverb_transfers_clear_only_the_selected_area(self):
        self.exercise(2)

    def test_callback_can_submit_an_independent_transfer(self):
        self.exercise(3)

    def test_pio_and_dma_have_distinct_completion_contracts(self):
        self.exercise(4)

    def test_vblank_callback_can_submit_without_repeating_the_edge(self):
        self.exercise(5)

    def test_driver_stop_during_setup_cancels_the_request(self):
        self.exercise(6)

    def test_dma4_read_as_spu_r_programs_it(self):
        self.exercise(7)
