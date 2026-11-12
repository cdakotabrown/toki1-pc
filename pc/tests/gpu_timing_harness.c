/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real clear decoder, queue and shared clock; instrument only IRQ/devices. */
#define GPU_REAL_CLOCK
#define main gpu_reset_fixture_main
#include "gpu_reset_harness.c"
#undef main
#include "file_id.h"

extern u64 hal_clock_ticks(void);
extern u32 hal_clock_vblanks(void), hal_clock_ticks_per_frame(void);
extern void hal_cpu_spend(u32 ticks);
extern void cd_hal_vblank_tick(void);
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { (void)n; return 0; }
void hal_overlay_set_loaded(const char *p) { (void)p; }
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {
    (void)p; (void)d; (void)a; (void)n;
}
/* Same one-line public wrapper as system.c; no replacement GPU behavior. */
void ResetGraph(s32 mode) { gpu_reset_graph(mode); }
static u64 event_at = (u64)-1;
static int event_mode, events, interrupts, dispatches, busy_at_dispatch;
static int reset_in_interrupt;
static int check_alarm_irq;
static void dispatch(void);
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) { assert(0); }
void memcard_vblank_tick(void) {}
u64 spu_transfer_next_event(void) { return event_at; }
void spu_transfer_complete_event(void) {
    event_at = (u64)-1;
    events++;
    if (event_mode == 1) {
        ResetGraph(1);
        gpu_queue_work(1000u);
    } else if (event_mode == 2) {
        assert(DrawSync(0) == 0);
    } else if (event_mode == 3) {
        assert(queue_word(8) == 0 && g_vram[0][0] == 0);
        assert(libgpu_alarm_deadline_vcount == 12 && libgpu_alarm_spin_count == 19);
        assert(DrawSync(1) == 0);
    } else if (event_mode == 4) {
        assert(queue_word(8) == 0 && g_vram[0][0] == 0);
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count == 0);
        ResetGraph(0);
        DrawSyncCallback(dispatch);
    } else if (event_mode == 5) {
        assert(libgpu_alarm_deadline_vcount == 12 && libgpu_alarm_spin_count == 19);
        event_at = 150u; event_mode = 6;
    } else if (event_mode == 6) {
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count == 0);
    } else if (event_mode == 7) {
        assert(libgpu_alarm_spin_count == 1);
        libgpu_alarm_deadline_vcount = -1;
    } else if (event_mode == 8) {
        libgpu_alarm_spin_count = 0x780001;
    } else if (event_mode == 9) {
        assert(libgpu_alarm_spin_count == 0);
        event_at = 310u; event_mode = 10;
    } else if (event_mode == 10) {
        assert(libgpu_alarm_spin_count == 1);
    } else assert(queue_word(8) == 1);
}
void hal_vblank_interrupt(void) {
    RECT rect;
    interrupts++;
    if (check_alarm_irq == 1) {
        assert(queue_word(8) == 0);
        assert(libgpu_alarm_deadline_vcount == 12 && libgpu_alarm_spin_count == 19);
    } else if (check_alarm_irq == 2) {
        assert(queue_word(8) == 0);
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count == 0);
    }
    if (reset_in_interrupt && interrupts == 1) {
        ResetGraph(1);
        hal_cpu_spend(1000u);
        rect.x = rect.y = 0; rect.w = 512; rect.h = 511;
        ClearImage(&rect, 248, 0, 0);
    }
}
static void dispatch(void) {
    dispatches++;
    assert(g_vram[0][0] == 31);
    busy_at_dispatch = DrawSync(1);
}
static void clear(s16 x, s16 width, s16 height) {
    RECT rect;
    rect.x = x; rect.y = 0; rect.w = width; rect.h = height;
    ClearImage(&rect, 248, 0, 0);
}
int main(int argc, char **argv) {
    u64 elapsed;
    u32 frame;
    int mode;
    (void)argc;
    mode = atoi(argv[1]);
    frame = hal_clock_ticks_per_frame();
    ResetGraph(0);
    if (mode == 0) {
        DrawSyncCallback(dispatch);
        clear(0, 512, 511);
        assert(dispatches == 1 && busy_at_dispatch == 1);
        assert(DrawSync(1) == 1);
        assert(DrawSync(0) == 0 && DrawSync(1) == 0);
        assert(dispatches == 1);
        assert(hal_clock_ticks() >= 245u + 18675u);
        assert(hal_clock_ticks() < 245u + 19150u);
    } else if (mode == 1) {
        clear(0, 1024, 512);
        assert(DrawSync(1) == 0);
        assert(hal_clock_ticks() == 245u + 158u);
        clear(3, 17, 8);
        assert(DrawSync(1) == 0);
    } else if (mode == 2) {
        clear(0, 63, 511);
        assert(DrawSync(1) == 1);
        assert(DrawSync(0) == 0);
        assert(hal_clock_ticks() >= 245u + 16108u);
        assert(hal_clock_ticks() < 245u + 16400u);
    } else if (mode == 3) {
        clear(0, 512, 511);
        hal_cpu_spend(1000u);
        clear(0, 512, 511);
        assert(DrawSync(0) == 0);
        assert(hal_clock_ticks() >= 245u + 37350u);
        assert(hal_clock_ticks() < 245u + 37600u);
    } else if (mode == 4) {
        clear(0, 512, 511);
        ResetGraph(1);
        assert(DrawSync(1) == 0 && g_vram[0][0] == 31);
        clear(0, 512, 511);
        ResetGraph(0);
        assert(DrawSync(1) == 0 && g_vram[0][0] == 31);
    } else if (mode == 5) {
        gpu_queue_work(240u);
        assert(DrawSync(1) == 1);
        assert(hal_clock_ticks() == 169u);
        assert(DrawSync(1) == 0);
    } else if (mode == 6) {
        gpu_queue_work(1u); gpu_queue_work(1u); gpu_queue_work(1u);
        hal_cpu_spend(1u);
        gpu_queue_work(228u);
        assert(DrawSync(1) == 1);
    } else if (mode == 7) {
        clear(0, 512, 511);
        event_mode = 1; event_at = hal_clock_ticks() + 100u;
        assert(DrawSync(0) == 0 && events == 1);
        assert(hal_clock_ticks() >= 245u + 600u);
        assert(hal_clock_ticks() < 245u + 850u);
    } else if (mode == 8) {
        reset_in_interrupt = 1;
        gpu_queue_work(2u * (frame + 50000u));
        assert(DrawSync(0) == 0 && interrupts == 1);
        assert(hal_clock_ticks() >= frame + 1245u + 18675u);
        assert(hal_clock_ticks() < frame + 1245u + 19000u);
    } else if (mode == 9) {
        clear(0, 512, 511);
        event_mode = 2; event_at = hal_clock_ticks() + 100u;
        assert(DrawSync(0) == 0 && events == 1);
        assert(hal_clock_ticks() >= 245u + 18675u);
        assert(hal_clock_ticks() < 245u + 19150u);
    } else if (mode == 10) {
        gpu_queue_work(2u * frame * 242u);
        assert(DrawSync(0) == -1);
        assert(hal_clock_vblanks() == 241u);
        assert(DrawSync(1) == 0);
    } else if (mode == 11) {
        /* Return-path IRQ starts work after an idle sample. The poll still
         * returns the observation taken before that interrupt. */
        event_mode = 1; event_at = 130u;
        assert(DrawSync(1) == 0 && events == 1);
        assert(DrawSync(1) == 1);
        assert(DrawSync(0) == 0);
    } else if (mode == 12) {
        sdk_work[1] = 0;
        event_at = 345u;
        clear(0, 512, 511);
        assert(hal_clock_ticks() == 468u && events == 1);
        clear(0, 512, 511);
        assert(hal_clock_ticks() >= 19143u);
        assert(hal_clock_ticks() < 19400u);
        assert(DrawSync(1) == 1);
        assert(DrawSync(0) == 0);
        assert(hal_clock_ticks() >= 37818u);
    } else if (mode == 13) {
        /* Retail still charges row/setup work for a zero-width fill. */
        clear(0, 0, 128);
        assert(g_vram[0][0] == 0);
        assert(DrawSync(1) == 1);
        assert(DrawSync(0) == 0);
        assert(hal_clock_ticks() >= 245u + 600u);
        assert(hal_clock_ticks() < 245u + 850u);
    } else if (mode == 14) {
        /* _addque2 still invokes its worker when the pre-fence fails. */
        gpu_queue_work(2u * frame * 242u);
        sdk_work[1] = 0;
        clear(0, 512, 511);
        assert(hal_clock_vblanks() == 241u);
        assert(g_vram[0][0] == 31 && DrawSync(1) == 1);
        assert(DrawSync(0) == 0);
    } else if (mode == 15) {
        libgpu_alarm_deadline_vcount = 12; libgpu_alarm_spin_count = 19;
        event_at = 100u; event_mode = 3;
        clear(0, 512, 511);
        assert(events == 1 && hal_clock_ticks() == 403u);
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count == 0);
        assert(queue_word(8) == 1 && g_vram[0][0] == 31);
    } else if (mode == 16) {
        event_at = 220u; event_mode = 4;
        clear(0, 512, 511);
        assert(events == 1 && dispatches == 1 && busy_at_dispatch == 1);
    } else if (mode == 17) {
        hal_cpu_spend(frame - 100u);
        libgpu_alarm_deadline_vcount = 12; libgpu_alarm_spin_count = 19;
        check_alarm_irq = 1;
        clear(0, 512, 511);
        assert(interrupts == 1 && hal_clock_ticks() == frame + 145u);
        assert(libgpu_alarm_deadline_vcount == 241 && libgpu_alarm_spin_count == 0);
    } else if (mode == 18) {
        libgpu_alarm_deadline_vcount = 12; libgpu_alarm_spin_count = 19;
        event_at = 100u; event_mode = 5;
        assert(DrawSync(0) == 0);
        assert(events == 2 && hal_clock_ticks() == 223u);
    } else if (mode == 19) {
        libgpu_alarm_deadline_vcount = 12; libgpu_alarm_spin_count = 19;
        assert(DrawSync(-1) == 0);
        assert(libgpu_alarm_deadline_vcount == 12 && libgpu_alarm_spin_count == 19);
    } else if (mode == 20) {
        gpu_queue_work(5000u);
        event_at = 260u; event_mode = 9;
        assert(DrawSync(0) == 0 && events == 2);
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count > 1);
    } else if (mode == 21) {
        gpu_queue_work(5000u);
        event_at = 400u; event_mode = 7;
        assert(DrawSync(0) == -1 && libgpu_alarm_spin_count == 1);
        assert(hal_clock_ticks() < 1000u);
    } else if (mode == 22) {
        gpu_queue_work(5000u);
        event_at = 250u; event_mode = 8;
        assert(DrawSync(0) == -1 && libgpu_alarm_spin_count == 0x780002);
    } else if (mode == 23) {
        libgpu_alarm_deadline_vcount = 12; libgpu_alarm_spin_count = 19;
        ResetGraph(1);
        assert(libgpu_alarm_deadline_vcount == 12 && libgpu_alarm_spin_count == 19);
        ResetGraph(0);
        assert(libgpu_alarm_deadline_vcount == 12 && libgpu_alarm_spin_count == 19);
    } else if (mode == 24) {
        hal_cpu_spend(frame - 220u);
        libgpu_alarm_deadline_vcount = 12; libgpu_alarm_spin_count = 19;
        check_alarm_irq = 2;
        clear(0, 512, 511);
        assert(interrupts == 1 && hal_clock_ticks() == frame + 25u);
        assert(libgpu_alarm_deadline_vcount == 240 && libgpu_alarm_spin_count == 0);
    } else if (mode == 25) {
        /* DRAWSYNC_BUSY_POLL: a 64 x 128 fill (2,222 work ticks plus the
         * packet's state words, from 245) seen busy at the reads 426, 602,
         * 756, 906, 1056, 1206, 1356 and idle at 1506; the caller's next
         * call 88 later (the legacy phases: reads every 150 from 426, idle
         * at 1476, return 42 later) */
        clear(0, 64, 128);
        assert(DrawSync(0) == 0);
        assert(hal_clock_ticks() == (getenv("DRAWSYNC_BUSY_POLL") ? 1518u : 1594u));
    } else assert(0);
    elapsed = hal_clock_ticks();
    printf("timing=%d ticks=%llu irq=%d events=%d\n", mode,
           (unsigned long long)elapsed, interrupts, events);
    return 0;
}
