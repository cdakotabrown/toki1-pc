/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual BIOS entries; fake CPU ledger for formatting/store probes, or
 * the actual shared clock and BIOS events for interruption integration. */
#include "hal.h"
#include "bios_printf.h"
#include "file_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef REAL_CLOCK
#include <setjmp.h>
#endif

extern void *func_800B4820(void *, s32);
static u8 bytes[4];
#ifndef REAL_CLOCK
static u64 ticks;
static u32 watch, visible;
void hal_cpu_spend(u32 n) {
    u32 i;
    if (ticks < watch && ticks + n >= watch) {
        for (i = 0; i < 4; i++) if (!bytes[i]) visible++;
    }
    ticks += n;
}
u64 hal_clock_ticks(void) { return ticks; }
static u64 now(void) { return ticks; }
#else
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { (void)n; return 0; }
void hal_overlay_set_loaded(const char *p) { (void)p; }
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {
    (void)p; (void)d; (void)a; (void)n;
}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) { assert(0); }
void memcard_vblank_tick(void) {}
extern void cd_hal_vblank_tick(void);
static int wait_in_device;
static u64 deadline = (u64)-1;
u64 spu_transfer_next_event(void) { return deadline; }
void spu_transfer_complete_event(void) {
    deadline = (u64)-1;
    if (wait_in_device) cd_hal_vblank_tick();
    else assert(func_800B4850("J") == 0);
}
extern u64 hal_clock_ticks(void);
extern u32 hal_clock_ticks_per_frame(void), hal_clock_vblanks(void);
extern void cd_hal_vblank_tick(void), hal_bios_vblank_tick(void);
extern s32 OpenEvent(u32, u32, u32, void (*)(void)), EnableEvent(s32);
static int callbacks;
static jmp_buf recovery;
static int abandon;
extern void hal_cpu_abort_work(void);
static void interrupt(void) {
    callbacks++;
    if (abandon) {
        hal_cpu_abort_work();
        longjmp(recovery, 1);
    }
    if (callbacks == 1) assert(func_800B4850("I") == 0);
}
void hal_vblank_interrupt(void) { hal_bios_vblank_tick(); }
static u64 now(void) { return hal_clock_ticks(); }
#endif

int main(int argc, char **argv) {
    s32 result = -99;
    int mode = atoi(argv[1]);
    u64 start = now();
#ifndef REAL_CLOCK
    char *long_string;
    char local_format[] = "%s", local_string[] = "upper";
    int sentinel = 0x12345678;
    if (mode == 0) result = func_800B4850("AAAA");
    else if (mode == 1) result = func_800B4850("\n");
    else if (mode == 2) result = func_800B4850("%08x", 0u);
    else if (mode == 3) result = func_800B4850("%08x", 0xffffffffu);
    else if (mode == 4) result = func_800B4850("%d", (s32)0x80000000u);
    else if (mode == 5) result = func_800B4850("%04d", -1);
    else if (mode == 6) result = func_800B4850("%u", 0xffffffffu);
    else if (mode == 7) result = func_800B4850("%s", "AAAA");
    else if (mode == 8) result = func_800B4850("%c", '\n');
    else if (mode == 9) result = func_800B4850("%%");
    else if (mode == 10)
        result = func_800B4850("tag=%08x,%08x\n", 0x12345678u, 0x89abcdefu);
    else if (mode == 11) result = func_800B4850("v=%d:%s\n", -7, "ok");
    else if (mode == 12) {
        result = func_800B4850("discard%n", &sentinel);
        assert(sentinel == 0x12345678);
    } else if (mode == 13) result = func_800B4850("discard%");
    else if (mode == 14) result = func_800B4850("discard%10000000x", 1u);
    else if (mode == 15) {
        long_string = (char *)malloc(32769);
        assert(long_string);
        memset(long_string, 'A', 32768); long_string[32768] = 0;
        result = func_800B4850("%s", long_string);
        free(long_string);
    } else if (mode == 16) result = func_800B4850(0);
    else if (mode == 17) result = func_800B4850("%s", (const char *)0);
    else if (mode == 18) result = func_800B4850((const char *)1);
    else if (mode == 19) {
        assert((unsigned long)local_format > 0xfffffffful);
        assert((unsigned long)local_string > 0xfffffffful);
        result = func_800B4850(local_format, local_string);
    } else {
        memset(bytes, 0xa5, sizeof(bytes));
        watch = argc > 2 ? (u32)strtoul(argv[2], 0, 10) : 0;
        result = func_800B4820(bytes, mode == 20 ? 3 : 0) == bytes;
        assert(bytes[3] == 0xa5);
        if (mode == 20) assert(!bytes[0] && !bytes[1] && !bytes[2]);
        else assert(bytes[0] == 0xa5);
    }
    fflush(stdout);
    fprintf(stderr, "result=%d ticks=%llu visible=%u\n", result,
            (unsigned long long)(now() - start), visible);
#else
    s32 event;
    u32 frame = hal_clock_ticks_per_frame();
    event = OpenEvent(0xf2000003u, 2, 0x1000, interrupt);
    assert(event != -1 && EnableEvent(event));
    if (mode == 0) {
        hal_cpu_spend(frame - 100);
        start = now();
        result = func_800B4850("A");
        assert(now() - start == 6954 && callbacks == 1);
    } else if (mode == 1) {
        deadline = now() + 100;
        result = func_800B4850("A");
        assert(now() - start == 6954 && callbacks == 0);
    } else if (mode == 4) {
        hal_cpu_spend(frame - 300);
        start = now();
        deadline = now() + 100;
        wait_in_device = 1;
        result = func_800B4850("A");
        /* the callback waits 200 ticks to the edge plus VSync(0)'s 24,291 */
        assert(now() - start == 3477 + 200 + 24291 && callbacks == 1);
    } else if (mode == 3) {
        abandon = 1;
        if (setjmp(recovery) == 0) {
            hal_cpu_spend(frame + 100);
            assert(0);
        }
        abandon = 0;
        start = now();
        hal_cpu_spend(frame + 100);
        assert(now() - start == frame + 100 && callbacks == 2);
        result = 0;
    } else {
        hal_cpu_spend(frame - 100);
        cd_hal_vblank_tick();
        /* VSync(0) resumes when the VBlank exception has returned,
         * 24,291 ticks after the edge (pc/hal/cd.c VSYNC_RETURN_LATENCY).
         * Handler work fits inside this absolute wait. */
        assert(now() == frame + 24291 && callbacks == 1);
        result = 0;
    }
    fflush(stdout);
    fprintf(stderr, "result=%d ticks=%llu callbacks=%d\n", result,
            (unsigned long long)(now() - start), callbacks);
#endif
    return 0;
}
