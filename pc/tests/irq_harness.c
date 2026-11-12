/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual clock, system VBlank entry, IRQ latch and BIOS event adapter. */
#include "hal.h"
#include "pc_irq.h"
#include "file_id.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void hal_cpu_spend(u32);
extern u64 hal_clock_ticks(void);
extern u32 hal_clock_vblanks(void), hal_clock_ticks_per_frame(void);
extern void hal_cpu_abort_work(void);
extern void cd_hal_vblank_tick(void);
extern s32 OpenEvent(u32, u32, u32, void (*)(void)), EnableEvent(s32);
extern s32 DeliverEvent(u32, u32), EnterCriticalSection(void);
extern void ExitCriticalSection(void);
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { (void)n; return 0; }
void hal_overlay_set_loaded(const char *p) { (void)p; }
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {
    (void)p; (void)d; (void)a; (void)n;
}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) { assert(0); }
u16 libapi_intr_env, libapi_intr_in_intr_flag, libapi_vsync_intr_enabled, libapi_dma_intr_enabled;
s32 Vcount;
void trapIntrDMA(void) { assert(0); }
static int cards, devices, edges, observations, pads, calls, mode;
static int sdk_calls;
static int depth, maximum_depth, direct_calls;
static u64 device_at = (u64)-1;
static jmp_buf recovery;
static char order[64];
static int order_len;
static void note(char c) {
    if (order_len < (int)sizeof(order) - 1) order[order_len++] = c;
    order[order_len] = 0;
}
void memcard_vblank_tick(void) { cards++; }
u64 spu_transfer_next_event(void) { return device_at; }
void spu_transfer_complete_event(void) { device_at = (u64)-1; devices++; }
void pad_update_buffers(void) { pads++; }
void trace_vblank_begin(void) {
    edges++;
    assert((u32)edges == hal_clock_vblanks());
}
void trace_vblank(void) { observations++; }

/* libetc delivery: one trapIntrVSync entry per delivered VBlank. */
static void sdk_vblank(void) { sdk_calls++; note('S'); }

static void callback(void) {
    u32 frame;
    frame = hal_clock_ticks_per_frame();
    calls++;
    note('E');
    depth++;
    if (depth > maximum_depth) maximum_depth = depth;
    /* Exception entry disables CPU delivery. Explicit software delivery
     * in case 13 is already in a critical section as well. */
    assert(EnterCriticalSection() == 0);
    if (mode == 8 && calls == 1) {
        ExitCriticalSection();
        hal_cpu_spend(frame + 100u);
        assert(calls == 1);
    }
    if (mode == 9 && calls == 1) assert(SetIntrMask(0) == 13);
    if (mode == 12 && calls == 1) {
        assert(SetIntrMask(0) == 13);
        hal_irq_abort();
        hal_cpu_abort_work();
        depth = 0;
        longjmp(recovery, 1);
    }
    depth--;
}

static void reassert(void) {
    direct_calls++;
    depth++;
    if (depth > maximum_depth) maximum_depth = depth;
    hal_irq_ack(1);
    if (direct_calls == 1) hal_irq_vblank(reassert);
    depth--;
}

int main(int argc, char **argv) {
    u32 frame;
    u16 old;
    u64 before;
    s32 event;
    (void)argc;
    mode = atoi(argv[1]);
    frame = hal_clock_ticks_per_frame();
    event = OpenEvent(0xf2000003u, 2, 0x1000, callback);
    assert(event != -1 && EnableEvent(event) == 1);
    ResetCallback();
    assert(VSyncCallback(sdk_vblank) == 0);
    assert(GetIntrMask() == 13);
    if (mode == 0) {
        hal_cpu_spend(3u * frame + 1u);
        assert(edges == 3 && calls == 3 && pads == 3 && cards == 3);
        assert(sdk_calls == 3);
        /* Kernel VBlank events run inside the exception, libetc after. */
        assert(strcmp(order, "ESESES") == 0);
    } else if (mode == 1 || mode == 2 || mode == 3 || mode == 14) {
        old = SetIntrMask(mode == 2 ? 12 : 0);
        assert(old == 13);
        if (mode == 2) device_at = frame / 2;
        hal_cpu_spend(3u * frame + 1u);
        /* A clear I_MASK VBlank bit keeps only the libetc chain away: the
         * kernel's VBlank events and the pad poll still run on each edge. */
        assert(edges == 3 && calls == 3 && pads == 3 && cards == 3);
        assert(sdk_calls == 0);
        assert(hal_irq_status() == 1);
        if (mode == 2) assert(devices == 1);
        if (mode == 3) hal_irq_ack(1);
        if (mode == 14) {
            assert(SetIntrMask(12) == 0 && hal_irq_status() == 1);
            assert(SetIntrMask(0) == 12 && hal_irq_status() == 1);
            hal_irq_ack(0);
            assert(hal_irq_status() == 1);
        }
        before = hal_clock_ticks();
        assert(SetIntrMask(old) == (mode == 2 ? 12 : 0));
        /* Restoration delivers the latched edge to libetc once; the kernel
         * events already ran on the physical edge, so nothing repeats. */
        assert(hal_clock_ticks() == before && edges == 3);
        assert(calls == 3 && pads == 3 && cards == 3);
        assert(sdk_calls == (mode == 3 ? 0 : 1));
        assert(hal_irq_status() == 0);
        hal_cpu_spend(frame);
        assert(sdk_calls == (mode == 3 ? 1 : 2));
        assert(calls == 4 && cards == 4);
    } else if (mode == 4) {
        assert(EnterCriticalSection() == 1);
        assert(EnterCriticalSection() == 0);
        hal_cpu_spend(3u * frame + 1u);
        assert(calls == 0 && edges == 3 && GetIntrMask() == 13);
        assert(cards == 0 && pads == 0 && sdk_calls == 0);
        ExitCriticalSection();
        assert(sdk_calls == 1 && calls == 0 && hal_irq_status() == 0);
        ExitCriticalSection();
        assert(sdk_calls == 1);
    } else if (mode == 5 || mode == 6) {
        assert(EnterCriticalSection() == 1);
        SetIntrMask(0);
        hal_cpu_spend(3u * frame + 1u);
        assert(cards == 0 && pads == 0 && sdk_calls == 0);
        if (mode == 5) {
            SetIntrMask(13);
            assert(sdk_calls == 0);
            ExitCriticalSection();
        } else {
            ExitCriticalSection();
            assert(sdk_calls == 0);
            SetIntrMask(13);
        }
        assert(sdk_calls == 1 && hal_irq_status() == 0);
    } else if (mode == 7) {
        assert(SetIntrMask(65535) == 13 && GetIntrMask() == 2047);
        assert(SetIntrMask(0x1234000c) == 2047 && GetIntrMask() == 12);
        assert(SetIntrMask(-1) == 12 && GetIntrMask() == 2047);
    } else if (mode == 8) {
        hal_cpu_spend(frame + 1u);
        /* The edge that arrives inside the handler waits for it to return. */
        assert(edges == 2 && calls == 1 && maximum_depth == 1);
        assert(cards == 1 && pads == 1 && sdk_calls == 1);
        /* plus the edge's libetc exception time (system.c, 24,449) */
        assert(hal_clock_ticks() == 2u * frame + 101u + 24449u);
        assert(hal_irq_status() == 0);
    } else if (mode == 9) {
        hal_cpu_spend(3u * frame + 1u);
        /* The callback clears the mask inside the first edge, so all three
         * kernel events run and no libetc delivery happens yet. */
        assert(edges == 3 && calls == 3 && GetIntrMask() == 0);
        assert(sdk_calls == 0);
        assert(SetIntrMask(13) == 0 && sdk_calls == 1);
        assert(hal_irq_status() == 0);
    } else if (mode == 10) {
        hal_irq_vblank(reassert);
        assert(direct_calls == 2 && maximum_depth == 1);
        assert(hal_irq_status() == 0 && edges == 0);
    } else if (mode == 11) {
        assert(EnterCriticalSection() == 1);
        hal_irq_vblank(reassert);
        hal_irq_vblank(reassert);
        hal_irq_ack(1);
        ExitCriticalSection();
        assert(direct_calls == 0 && edges == 0);
    } else if (mode == 12) {
        if (setjmp(recovery) == 0) {
            hal_cpu_spend(frame + 1u);
            assert(0);
        }
        assert(calls == 1 && GetIntrMask() == 13);
        assert(hal_irq_status() == 0);
        hal_cpu_spend(frame + 1u);
        assert(calls == 2 && pads == 1 && maximum_depth == 1);
        assert(sdk_calls == 1);
    } else if (mode == 13) {
        EnterCriticalSection();
        assert(DeliverEvent(0xf2000003u, 2) == 0 && calls == 1);
        assert(hal_irq_status() == 0 && edges == 0);
        ExitCriticalSection();
    } else if (mode == 15) {
        /* CPU work across one delivered edge loses the exception's time. */
        hal_cpu_spend(frame + 1000u);
        assert(edges == 1 && sdk_calls == 1);
        assert(hal_clock_ticks() == frame + 1000u + 24449u);
    } else if (mode == 16) {
        /* Kernel-only exceptions (I_MASK VBlank bit clear) are unmeasured
         * and not charged; the latched delivery on restore is not timed. */
        SetIntrMask(0);
        hal_cpu_spend(frame + 1000u);
        assert(edges == 1 && calls == 1 && sdk_calls == 0);
        assert(hal_clock_ticks() == frame + 1000u);
        SetIntrMask(13);
        assert(sdk_calls == 1 && hal_clock_ticks() == frame + 1000u);
    } else if (mode == 17) {
        /* No exception inside a critical section, so nothing is charged. */
        EnterCriticalSection();
        hal_cpu_spend(frame + 1000u);
        assert(edges == 1 && sdk_calls == 0);
        assert(hal_clock_ticks() == frame + 1000u);
        ExitCriticalSection();
        assert(sdk_calls == 1 && hal_clock_ticks() == frame + 1000u);
    } else if (mode == 18) {
        /* Absolute waits do not take the occupancy. */
        hal_cpu_spend(30000u);   /* past the clock-origin frame's return */
        cd_hal_vblank_tick();
        assert(edges == 1 && sdk_calls == 1);
        assert(hal_clock_ticks() == frame + 24291u);
    } else assert(0);
    /* Recovery abandons the interrupted edge's post-IRQ observation. */
    assert(edges == observations + (mode == 12 ? 1 : 0));
    printf("mode=%d edges=%d callbacks=%d pads=%d cards=%d sdk=%d ticks=%llu\n",
           mode, edges, calls, pads, cards, sdk_calls,
           (unsigned long long)hal_clock_ticks());
    return 0;
}
