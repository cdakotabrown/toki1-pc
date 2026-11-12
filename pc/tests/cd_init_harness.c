/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual controller scheduler and matched public CdInit retry wrapper. */
#include "file_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#ifndef CD_SOURCE
#define CD_SOURCE "../hal/cd.c"
#endif
#include CD_SOURCE

#ifndef BOOT_LBA
#define BOOT_LBA 24u
#endif
#ifndef BOOT_SIZE
#define BOOT_SIZE 690176u
#endif
#ifndef BOOT_PATH
#define BOOT_PATH "PSX.EXE"
#endif
const FileLbaEntry g_file_lba_table[] = {{BOOT_PATH, BOOT_LBA, BOOT_SIZE}};
const u32 g_file_lba_table_count = 1;
const FileLbaEntry *file_id_lookup_lba(u32 n) { (void)n; return 0; }
void hal_overlay_set_loaded(const char *p) { (void)p; }
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {
    (void)p; (void)d; (void)a; (void)n;
}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) { assert(0); }
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) { assert(0); }
s32 libcd_cdinit_fail_msg;
static int edges, resets, prints, errors, attempts, which, sync_events;
static int ready_events;
void hal_vblank_interrupt(void) { edges++; }
void ResetCallback(void) { resets++; }
s32 func_800B4850(const char *format, ...) {
    if ((uintptr_t)format == 0x800B9050u) prints++;
    else { assert((uintptr_t)format == 0x800B8C40u); errors++; }
    return 0;
}
void DeliverEvent(u32 cls, s32 spec) {
    assert(cls == 0xf0000003u);
    if (spec == 32) sync_events++;
    else { assert(spec == 64); ready_events++; }
}
extern void def_cbsync(void), def_cbready(void), def_cbread(void);
extern s32 __real_CD_init(void);
s32 __wrap_CD_init(void) {
    attempts++;
    if ((which == 20 && attempts < 5) || which == 21 ||
            (which == 23 && attempts < 3)) return -1;
    if (which == 22) return 0;
    return __real_CD_init();
}
static void user_callback(void) { assert(0); }

/* Mode 30: the five reference coordinates, in the reference order.  A fresh
 * native boot hands off the executable extent, then one warm init; the other
 * three cases set mode/position first, exactly like the retail probe. */
static void probe_case(const char *name, int mode_input, s32 seek_lba)
{
    u8 mode[8] = {0}, loc[8] = {0};
    u64 start;
    u32 vblank_start;
    s32 result;
    if (mode_input >= 0) {
        mode[0] = (u8)mode_input;
        CdControlB(CMD_SETMODE, mode, 0);
        CdIntToPos(seek_lba, loc);
        CdControlB(CMD_SEEKL, loc, 0);
    }
    start = hal_clock_ticks();
    vblank_start = hal_clock_vblanks();
    result = CdInit();
    printf("CASE %s %d %d %llu %u %u %d\n", name, mode_input, seek_lba,
           (unsigned long long)(hal_clock_ticks() - start), vblank_start,
           hal_clock_vblanks(), result);
}

int main(int argc, char **argv) {
    u8 mode[8] = {0}, loc[8] = {0};
    u64 before, elapsed;
    s32 result;
    (void)argc;
    setbuf(stdout, 0);
    which = atoi(argv[1]);
    if (which == 30) {
        assert(cd_hal_boot_handoff() == 1);
        probe_case("boot", -1, -1);
        probe_case("warm", -1, -1);
        probe_case("single_near", 0, 100);
        probe_case("double_near", 128, 100);
        probe_case("double_far", 128, 500);
        return 0;
    }
    if (which == 3) {
        assert(cd_hal_boot_handoff() == 1);
        assert(g_cur_lba == 360 && g_subq == 360 && g_mode == 0x80);
        assert(g_double_speed == 1 && hal_clock_ticks() == 0);
    } else if (which == 4 || which == 5) {
        mode[0] = which == 4 ? 0x80 : 0;
        CdControlB(CMD_SETMODE, mode, 0);
        CdIntToPos(100, loc);
        CdControlB(CMD_SEEKL, loc, 0);
        assert(g_cur_lba == 100 && g_drive == DRIVE_IDLE);
    } else if (which == 6) {
        hal_cpu_spend(1);
        assert(cd_hal_boot_handoff() == 0 && g_mode == 0);
        return 0;
    } else if (which == 7) {
        assert(cd_hal_boot_handoff() == 1);
        assert(cd_hal_boot_handoff() == 0);
        assert(g_cur_lba == 360 && g_double_speed == 1);
        return 0;
    } else if (which == 8) {
        assert(cd_hal_boot_handoff() == 0 && g_cur_lba == 0);
    } else if (which == 9) {
        CdControlB(CMD_READS, 0, 0);
    } else if (which == 10) {
        CdControlF(CMD_INIT, 0);
    }
    CdSyncCallback(user_callback);
    CdReadyCallback(user_callback);
    CdReadCallback(user_callback);
    CdDataCallback(user_callback);
    before = hal_clock_ticks();
    puts("BEGIN_INIT");
    result = which == 0 || which == 11 ? CD_init() : CdInit();
    puts("END_INIT");
    elapsed = hal_clock_ticks() - before;
    if (which == 21) {
        assert(result == 0 && attempts == 5 && errors == 1);
        assert(CdSyncCallback(0) == user_callback);
        assert(resets == 0 && prints == 0 && elapsed == 0);
        return 0;
    }
    if (which == 0 || which == 11) {
        assert(result == 0 && CdSyncCallback(0) == 0);
        assert(CdReadyCallback(0) == 0);
        assert(CdReadCallback(0) == user_callback);
    } else {
        assert(result == 1 && CdSyncCallback(0) == def_cbsync);
        assert(CdReadyCallback(0) == def_cbready);
        assert(CdReadCallback(0) == def_cbread);
    }
    assert(CdDataCallback(0) == user_callback);
    assert(errors == 0);
    if (which == 22) {
        assert(attempts == 1 && resets == 0 && prints == 0 && elapsed == 0);
        return 0;
    }
    assert(resets == 1 && prints == 1);
    assert(g_drive == DRIVE_IDLE && g_cur_lba == 0 && g_subq == 0);
    assert(!g_double_speed && g_mode == 0x20 && !g_setloc_pending);
    assert(!g_cmd_pending && !g_second_cmd && !g_xfer_active);
    assert(CdSync(1, 0) == 2);
    if (which == 3) assert(elapsed > 27000000u && elapsed < 27500000u);
    else if (which == 4) assert(elapsed > 25000000u && elapsed < 26000000u);
    else if (which == 10) assert(elapsed > 4000000u);
    /* 4,130,000: Init's 4,000,000 second response after the 80,000-tick
     * Init and 25,000-tick GetStat acknowledges, plus Demute's 25,000.
     * 8,920: libcd's CPU per CD_cw (pc/hal/cd.c) -- three 440-tick register
     * writes and the 3,800-tick acknowledge/exit after GetStat and Demute
     * (Init's overlaps its own 4,000,000-tick wait).
     * 4,616: the CPU leaves Init's completion (INT2) interrupt that long
     * after the second response, before CD_sync sees it. */
    else assert(elapsed == 4130000u + 8920u + 4616u);
    if (which == 2) {
        before = hal_clock_ticks();
        assert(CdInit() == 1 && hal_clock_ticks() - before == 4130000u + 8920u + 4616u);
        assert(prints == 2 && resets == 2);
    }
    if (which == 9) {
        hal_cpu_spend(3u * TICKS_PER_FRAME);
        assert(g_drive == DRIVE_IDLE && g_cur_lba == 0);
    }
    if (which == 12) {
        def_cbsync(); def_cbready(); def_cbread();
        assert(sync_events == 1 && ready_events == 2);
    }
    if (which == 20) assert(attempts == 5);
    if (which == 23) assert(attempts == 3);
    printf("RESULT ticks=%llu edges=%d attempts=%d\n",
           (unsigned long long)elapsed, edges, attempts);
    return 0;
}
