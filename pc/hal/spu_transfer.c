/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPU transfer adapter. Timing is an explicit approximation of the
 * SCPH-5500 Interpreter probes.
 * Transfer, interrupt-handler work and kernel delivery are distinct stages;
 * no startup-frame target participates in scheduling.
 */
#include "hal.h"
#include "spu_transfer.h"
#include "cpu_cost.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u64 hal_clock_ticks(void);
extern void hal_cpu_spend(u32 ticks);
extern s32 OpenEvent(u32 cls, u32 spec, u32 mode, void (*fn)(void));
extern s32 EnableEvent(s32 event);
extern s32 CloseEvent(s32 event);
extern s32 TestEvent(s32 event);
extern s32 WaitEvent(s32 event);
extern void hal_bios_deliver_event(u32 cls, u32 spec);

/* These are 32-bit PS1 words, including the stored callback/list pointers. */
extern u32 _spu_EVdma, _spu_transferCallback, _SpuMemList;
extern u32 _spu_rev_startaddr[10];
extern s32 _spu_inTransfer, _spu_trans_mode, _spu_AllocBlockNum;
extern s32 spu_transMode;
extern u16 spu_tsa;

#define NO_EVENT ((u64)-1)
#define SPU_RAM_SIZE 0x80000u
#define SPU_DMA_CLASS 0xF0000009u
#define SPU_DMA_SPEC 0x20u
/* Four SDK _waitfs loops before DMA (each 240-iteration loop costs about
 * 7,470 ticks in the measured build), separately from the transfer's 15.75
 * ticks per halfword (spu_transfer_write). Costs are not cycle-exact.
 * The completion interrupt is an exception (pc/hal/cd.c
 * hal_irq_exception): measured on the reference emulator (Interpreter,
 * SCPH-5500, the 37 boot VAB/clear transfers of a 1,800-frame no-input
 * boot), _spu_interruptDMA's two _waitfs loops end in DeliverEvent(HwSPU)
 * 17,619 ticks after the exception's entry, and the CPU returns at 19,114
 * (medians; 17,5xx..17,8xx and 18,780..18,989 + ~310 for the return). */
#define DMA_SETUP_TICKS (4u * SPU_WAITFS_TICKS + 360u)
#define DMA_IRQ_DELIVER_TICKS 17619u
#define DMA_IRQ_EXIT_TICKS    19114u
/* A read's interrupt: _spu_interruptDMA runs its two _waitfs loops only
 * when spu_dma_mode == 0, which _spu_w stores and _spu_r does not (it stores
 * 1).  The read exception is the write one less those two loops (2,679 /
 * 4,174); retail scene 17's SpuReads measure 2,616..2,703 to DeliverEvent
 * and 4,176..4,291 to the return. */
#define DMA_READ_IRQ_DELIVER_TICKS (DMA_IRQ_DELIVER_TICKS - 2u * SPU_WAITFS_TICKS)
#define DMA_READ_IRQ_EXIT_TICKS    (DMA_IRQ_EXIT_TICKS - 2u * SPU_WAITFS_TICKS)
extern int hal_irq_exception(u16 bit, u32 effect_at, void (*effect)(void), u32 exit_at);

/* Round 5 (2026-10-31), measured on the reference emulator (Interpreter,
 * SCPH-5500) with probes on _spu_write, _spu_w, its DMA4 CHCR store
 * (0x80095E9C), the exception vector and _spu_interruptDMA, over the 36
 * reverb-clear chunks and the two VAB bodies of a 1,700-frame boot:
 *
 * SPU_DMA_LINEAR: the CHCR store to the completion exception is 7,629..
 *   7,639 ticks for every 1,024-byte chunk (512 halfwords), 447,557 for a
 *   56,800-byte body (28,416), 278,546 for 35,376 bytes (17,696): a line,
 *   15.766 ticks per halfword less 448 (the SPU drains its 32-halfword
 *   FIFO at 16 ticks a halfword, the SPU core's spu.cpp; the DMA's block
 *   requests set the rest), 7,624 for a chunk.  A 64-byte request fits
 *   the FIFO: 26 ticks.  The port charged 15.75 a halfword from the store
 *   for both (8,064 a chunk).  =0: the old rate.
 * SPU_WRITE_SETUP: _spu_write's entry to the CHCR store is 30,173 ticks
 *   for the clear's second and later chunks, 30,217..30,220 for its first
 *   and for a body (four _waitfs loops, the SPU control writes,
 *   _spu_setDelayW); the port spent 4 x 7,470 + 360 and a TestEvent
 *   (62), 30,302.  =0: the old constant.
 * SPU_CLEAR_CPU: SpuClearReverbWorkArea's own CPU around the chunks: entry
 *   to the first _spu_write 721 (_SpuIsInAllocateArea, three _spu_ioctl),
 *   the exception's return to the next _spu_write 266 (WaitEvent's return,
 *   _spu_ioctl), and after the last chunk to the caller's next call 368.
 *   The port charged none of it.  =0: none. */
static int spu_switch(const char *name, int *cache)
{
    if (*cache < 0) {
        const char *e = getenv(name);
        *cache = !(e && e[0] == '0');
    }
    return *cache;
}
static int spu_dma_linear_on(void) { static int c = -1; return spu_switch("SPU_DMA_LINEAR", &c); }
static int spu_write_setup_on(void) { static int c = -1; return spu_switch("SPU_WRITE_SETUP", &c); }
static int spu_clear_cpu_on(void) { static int c = -1; return spu_switch("SPU_CLEAR_CPU", &c); }
#define SPU_WRITE_TO_DMA_FIRST  30220u
#define SPU_WRITE_TO_DMA_REPEAT 30173u
#define SPU_CLEAR_TO_FIRST      721u
#define SPU_CLEAR_BETWEEN       266u
#define SPU_CLEAR_TO_RETURN     368u
static int g_clear_chunk;   /* the clear's chunk being written, 0: not a clear */
static u64 g_irq_entered;  /* the last write completion exception's entry */

/* The clear's CPU after a chunk's exception returns: measured from that
 * return, which the port's WaitEvent (polling in 128-tick steps) passes
 * by up to 127 ticks; the gap spends the rest. */
static void spu_clear_after_irq(u32 ticks)
{
    u64 target = g_irq_entered + DMA_IRQ_EXIT_TICKS + ticks;
    u64 now = hal_clock_ticks();
    hal_cpu_spend(target > now ? (u32)(target - now) : 0u);
}

static u64 spu_dma_write_ticks(u32 bytes)
{
    u32 halfwords = bytes / 2u;
    if (!spu_dma_linear_on()) return (u64)halfwords * 63u / 4u;
    if (halfwords <= 32u) return 26u;
    return ((u64)halfwords * 15766u) / 1000u - 448u;
}

typedef struct {
    u8 *data;
    u32 address, size;
    int stage;                 /* 0 idle, 1 transfer, 2 in the completion interrupt */
    u64 due;
    u8 *ram;                   /* a read's main-RAM destination (DMA4 MADR) */
} SpuTransfer;
static SpuTransfer g_transfer = { NULL, 0, 0, 0, NO_EVENT, NULL };

/* ---- The SDK's register cells ------------------------------------------
 * libspu reaches the hardware through 4-byte PS1 pointer cells, retail
 * data 0x800E3170..0x800E3188: 0x1F801C00 (_spu_RXX, the SPU block),
 * 0x1F8010C0/C4/C8 (DMA4 MADR/BCR/CHCR), 0x1F8010F0/F4 (DPCR/DICR) and
 * 0x1F801014 (SPU_DELAY).  A native 8-byte load of one cell joins it with
 * its neighbour (0x1F8010F4 | 0x1F801014 << 32 is non-canonical), so
 * _spu_r ran only through main.c's zero-page emulation and crashed on its
 * read-modify-write of DICR.  pc/Makefile (SPU_REG_USERS) renames the
 * cells to these native pointers, holding the same retail values, in the
 * matched libspu objects; they address the mapped I/O page (pc/src/main.c),
 * whose DMA4 registers spu_dma4_poll() below services. */
void *volatile hal_spu_rxx      = (void *)(unsigned long)0x1F801C00u;
void *volatile hal_spu_dma4_madr = (void *)(unsigned long)0x1F8010C0u;
void *volatile hal_spu_dma4_bcr  = (void *)(unsigned long)0x1F8010C4u;
void *volatile hal_spu_dma4_chcr = (void *)(unsigned long)0x1F8010C8u;
void *volatile hal_spu_dma_dpcr  = (void *)(unsigned long)0x1F8010F0u;
void *volatile hal_spu_dma_dicr  = (void *)(unsigned long)0x1F8010F4u;
void *volatile hal_spu_delay     = (void *)(unsigned long)0x1F801014u;

#define DMA4_MADR ((volatile u32 *)0x1F8010C0ul)
#define DMA4_BCR  ((volatile u32 *)0x1F8010C4ul)
#define DMA4_CHCR ((volatile u32 *)0x1F8010C8ul)
#define DMA_DPCR  ((volatile u32 *)0x1F8010F0ul)
#define SPU_TADDR ((volatile u16 *)0x1F801DA6ul)
#define SPU_CNT   ((volatile u16 *)0x1F801DAAul)
#define CHCR_START     0x01000000u
#define CHCR_TO_DEVICE 0x00000001u
#define DPCR_DMA4_ENABLE 0x00080000u
static int g_initialized;
static u64 g_last_poll = NO_EVENT;
static u64 g_request_serial;

void spu_transfer_init(void)
{
    g_request_serial++;
    if (g_initialized) CloseEvent((s32)_spu_EVdma);
    free(g_transfer.data);
    memset(&g_transfer, 0, sizeof(g_transfer));
    g_transfer.due = NO_EVENT;
    _spu_EVdma = (u32)OpenEvent(SPU_DMA_CLASS, SPU_DMA_SPEC, 0x2000, NULL);
    if ((s32)_spu_EVdma != -1) EnableEvent((s32)_spu_EVdma);
    _spu_inTransfer = 1;
    _spu_trans_mode = 0;
    _spu_transferCallback = 0;
    spu_transMode = 1;
    spu_tsa = 0;
    g_last_poll = NO_EVENT;
    g_initialized = (s32)_spu_EVdma != -1;
}

void spu_transfer_shutdown(void)
{
    g_request_serial++;
    if (g_initialized) CloseEvent((s32)_spu_EVdma);
    free(g_transfer.data);
    memset(&g_transfer, 0, sizeof(g_transfer));
    g_transfer.due = NO_EVENT;
    g_initialized = 0;
}

static void spu_dma4_poll(void);

/* The clock asks before every advance (pc/hal/cd.c run_until), so a DMA4
 * channel the game started by a CHCR write is seen at the emulated time of
 * that write: no clock movement happens in between. */
u64 spu_transfer_next_event(void)
{
    spu_dma4_poll();
    return g_transfer.due;
}

static void spu_dma_deliver(void);
static void spu_dma_read_deliver(void);

/* IRQ_IN_VBLANK_MERGE: a write's completion raised while the CPU is in a
 * VBlank exception (before trapIntr's last I_STAT read) is dispatched in
 * that exception's loop, not in an exception of its own.  On the
 * reference (the boot's reverb clear, frame 553: an edge 50,606 ticks
 * after a chunk's DMA started) that exception ran 39,237 ticks, against
 * 21,522 for the next edge's VBlank exception alone (same handlers, the
 * sequencer not yet running): the completion added 17,715, where its own
 * exception takes 19,114 (19,087..19,114 in the clear).  So a merged
 * completion costs 1,399 ticks less.  The port gave it a full one after
 * the VBlank's.  =0: the separate exception. */
#define DMA_IRQ_SHARED_WITH_VBLANK 1399u
extern int hal_in_vblank_exception(void) __attribute__((weak));
static int irq_merge_on(void) { static int c = -1; return spu_switch("IRQ_IN_VBLANK_MERGE", &c); }

void spu_transfer_complete_event(void)
{
    u32 shared = 0;
    if (g_transfer.stage != 1 || hal_clock_ticks() < g_transfer.due) return;
    if (g_transfer.ram) {
        /* DMA4 from the SPU: the block mode leaves MADR past the data,
         * BCR's block count at 0 and the start bit clear; the interrupt
         * (DICR has DMA4 enabled by _spu_r) is a shorter exception. */
        spu_transfer_readback(g_transfer.ram, g_transfer.address,
                              g_transfer.size);
        *DMA4_MADR += g_transfer.size;
        *DMA4_BCR &= 0xFFFFu;
        *DMA4_CHCR &= ~CHCR_START;
        g_transfer.stage = 2;
        g_transfer.due = NO_EVENT;
        hal_irq_exception(0x0008u, DMA_READ_IRQ_DELIVER_TICKS,
                          spu_dma_read_deliver, DMA_READ_IRQ_EXIT_TICKS);
        return;
    }
    spu_transfer_commit(g_transfer.data, g_transfer.address,
                        g_transfer.size);
    /* The DMA interrupt is being handled: no new request until the
     * handler has delivered the event. */
    g_transfer.stage = 2;
    g_transfer.due = NO_EVENT;
    g_irq_entered = hal_clock_ticks();
    if (irq_merge_on() && hal_in_vblank_exception && hal_in_vblank_exception())
        shared = DMA_IRQ_SHARED_WITH_VBLANK;
    g_irq_entered -= shared;
    hal_irq_exception(0x0008u, DMA_IRQ_DELIVER_TICKS - shared, spu_dma_deliver,
                      DMA_IRQ_EXIT_TICKS - shared);
}

static void spu_dma_deliver(void)
{
    u8 *data;
    u32 callback;
    /* A VBlank callback inside the handler may have stopped the driver. */
    if (g_transfer.stage != 2) return;
    /* Retire the completed request before invoking user code. A callback
     * may submit another request, which owns its own storage and deadline. */
    data = g_transfer.data;
    callback = _spu_transferCallback;
    g_transfer.data = NULL;
    g_transfer.stage = 0;
    g_transfer.due = NO_EVENT;
    free(data);
    if (callback)
        ((void (*)(void))(uintptr_t)callback)();
    else
        hal_bios_deliver_event(SPU_DMA_CLASS, SPU_DMA_SPEC);
}

/* _spu_interruptDMA for a read: clear SPUCNT's transfer mode, then the
 * callback or the DMA event, as for a write. */
static void spu_dma_read_deliver(void)
{
    u32 callback;
    if (g_transfer.stage != 2) return;
    callback = _spu_transferCallback;
    g_transfer.ram = NULL;
    g_transfer.stage = 0;
    g_transfer.due = NO_EVENT;
    *SPU_CNT &= 0xFFCFu;
    if (callback)
        ((void (*)(void))(uintptr_t)callback)();
    else
        hal_bios_deliver_event(SPU_DMA_CLASS, SPU_DMA_SPEC);
}

/* DMA channel 4 as the matched _spu_r programs it (src/main/matched/
 * _spu_r.c): SPUCNT transfer mode 3 (DMA read), the SPU transfer address
 * at 0x1F801DA6, DPCR's DMA4 enable, MADR = destination, BCR = blocks of
 * 16 words, CHCR = 0x01000200 (start, sync mode 1, from the device).
 * The transfer runs at 16 ticks per halfword (writes: 15.75); SPU RAM is
 * copied to main RAM at completion, in one piece.  To-device kicks are not
 * serviced here: SpuWrite is the HAL's (spu_transfer_write) and no linked
 * matched code programs one.  A channel that cannot run stays pending. */
static void spu_dma4_poll(void)
{
    static int reported;
    u32 chcr, bcr, words;
    u8 *dst;
    if (!g_initialized || g_transfer.stage) return;
    if (!pc_ptr_is_mapped((const void *)DMA4_MADR, 0x40)) return;
    chcr = *DMA4_CHCR;
    if (!(chcr & CHCR_START) || (chcr & CHCR_TO_DEVICE)) return;
    if (!(*DMA_DPCR & DPCR_DMA4_ENABLE)) return;
    bcr = *DMA4_BCR;
    words = (bcr & 0xFFFFu) ? (bcr & 0xFFFFu) : 0x10000u;
    words *= (bcr >> 16) ? (bcr >> 16) : 0x10000u;
    dst = (u8 *)(uintptr_t)*DMA4_MADR;
    if (((chcr >> 9) & 3u) != 1u || ((*SPU_CNT >> 4) & 3u) != 3u ||
            words > SPU_RAM_SIZE / 4u || !pc_ptr_is_mapped(dst, words * 4u)) {
        if (!reported++)
            printf("[SPU] DMA4 read not serviced: CHCR %08X BCR %08X MADR "
                   "%08X SPUCNT %04X\n", (unsigned)chcr, (unsigned)bcr,
                   (unsigned)*DMA4_MADR, (unsigned)*SPU_CNT);
        return;
    }
    g_request_serial++;
    g_transfer.data = NULL;
    g_transfer.ram = dst;
    g_transfer.address = (u32)*SPU_TADDR << 3;
    g_transfer.size = words * 4u;
    g_transfer.stage = 1;
    g_transfer.due = hal_clock_ticks() + (u64)(g_transfer.size / 2u) * 16u;
}

s32 spu_transfer_mode(s32 mode)
{
    _spu_trans_mode = mode;
    spu_transMode = mode == 1 ? 0 : 1;
    return mode == 1 ? 1 : 0;
}

u32 spu_transfer_address(u32 address)
{
    if (address >= 0x7FFF9u) return 0;
    address = (address + 7u) & ~7u;
    spu_tsa = (u16)(address >> 3);
    return address;
}

s32 spu_transfer_write(const u8 *data, u32 size)
{
    u8 *copy;
    u32 address, blocks;
    u64 serial;
    int pio, measured;
    unsigned long long mark;
    if (!g_initialized || g_transfer.stage || !data || !size) return 0;
    if (size > 0x7F000u) size = 0x7F000u;
    /* _spu_w programs the DMA with (size >> 6) + ((size & 63) != 0)
     * 64-byte blocks (src/main/matched/_spu_w.c), so a partial block
     * reads past the stated size from main RAM.  libsnd's VAB bodies are
     * such requests (e.g. 0xDDE0 bytes).  PIO (_spu_writeByIO) writes
     * exactly `size`. */
    blocks = (size >> 6) + ((size & 63u) != 0);
    pio = spu_transMode != 1;
    data = hal_ps1_ptr(data);
    if (!pc_ptr_is_mapped(data, pio ? size : blocks * 64u)) return 0;
    copy = malloc(blocks * 64u);
    if (!copy) return 0;
    memcpy(copy, data, pio ? size : blocks * 64u);
    address = (u32)spu_tsa << 3;
    g_transfer.data = copy;
    g_transfer.ram = NULL;
    g_transfer.address = address;
    g_transfer.size = pio ? size : blocks * 64u;
    g_transfer.stage = 1;
    g_transfer.due = NO_EVENT;
    serial = ++g_request_serial;
    _spu_inTransfer = _spu_transferCallback != 0;
    mark = hal_cost_mark();
    measured = spu_write_setup_on() && !pio;
    /* Reserve first, so a VBlank callback cannot submit over this request. */
    if (!measured) hal_cpu_spend(DMA_SETUP_TICKS);
    /* A VBlank callback can stop/reset the driver during SDK setup. It
     * has already retired this request's storage; do not resurrect it. */
    if (serial != g_request_serial) return 0;
    if (pio) {
        /* PIO is synchronous; it does not raise the DMA completion event.
         * Only the data contract is modelled for this unmeasured mode. */
        spu_transfer_commit(copy, address, size);
        free(copy);
        g_transfer.data = NULL;
        g_transfer.stage = 0;
        _spu_inTransfer = 1;
    } else {
        /* Transfer address remains the SDK's selected start address. */
        while (TestEvent((s32)_spu_EVdma)) {}
        if (measured) {
            hal_cpu_spend(hal_cost_net(g_clear_chunk > 1 ? SPU_WRITE_TO_DMA_REPEAT
                                                         : SPU_WRITE_TO_DMA_FIRST, mark));
            if (serial != g_request_serial) return 0;
        }
        /* Kick to interrupt, measured on the reference: 15.75 ticks per
         * halfword for the VAB bodies (14.92 for the 1,024-byte reverb
         * clears; the bodies are the long ones).  It was 16, which the
         * then-free game code hid; the boot's frame-458 VAB body lands on
         * retail's VBlank with it (2026-10-31, pc/hal/cpu_cost.c). */
        g_transfer.due = hal_clock_ticks() + spu_dma_write_ticks(g_transfer.size);
    }
    return (s32)size;
}

s32 spu_transfer_completed(s32 wait)
{
    s32 result;
    if (_spu_trans_mode == 1 || _spu_inTransfer) return 1;
    if (!wait && g_last_poll == hal_clock_ticks()) hal_cpu_spend(128u);
    g_last_poll = hal_clock_ticks();
    result = wait ? WaitEvent((s32)_spu_EVdma) : TestEvent((s32)_spu_EVdma);
    if (result) _spu_inTransfer = 1;
    return result;
}

static int reverb_allocated(u32 address)
{
    const u32 *list;
    u32 flags, start;
    int i;
    if (!_SpuMemList || _spu_AllocBlockNum <= 0) return 0;
    list = hal_ps1_ptr((void *)(uintptr_t)_SpuMemList);
    for (i = 0; i < _spu_AllocBlockNum; i++) {
        if (!pc_ptr_is_mapped(list + 2 * i, 8)) return 1;
        flags = list[2 * i];
        if (!(flags & 0x80000000u)) {
            if (flags & 0x40000000u) return 0;
            start = flags & 0x0FFFFFFFu;
            if (address <= start || address - start < list[2 * i + 1])
                return 1;
        }
    }
    return 1;                 /* reject malformed unterminated metadata */
}

s32 spu_transfer_clear(s32 mode)
{
    static const u8 zeros[1024] = { 0 };
    u32 address, remaining, amount, callback;
    s32 saved_mode, saved_complete, result;
    int cpu;
    if (mode < 0 || mode >= 10 || !g_initialized || g_transfer.stage)
        return -1;
    if (mode != 0 && _spu_rev_startaddr[mode] > 0xFFFFu) return -1;
    address = mode == 0 ? 0x7FF80u : _spu_rev_startaddr[mode] << 3;
    if (address >= SPU_RAM_SIZE || reverb_allocated(address)) return -1;
    remaining = SPU_RAM_SIZE - address;
    saved_mode = spu_transMode;
    saved_complete = _spu_inTransfer;
    callback = _spu_transferCallback;
    _spu_transferCallback = 0;
    spu_transMode = 1;
    result = 0;
    cpu = spu_clear_cpu_on();
    if (cpu) hal_cpu_spend(SPU_CLEAR_TO_FIRST);
    g_clear_chunk = 0;
    while (remaining) {
        amount = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
        spu_transfer_address(address);
        g_clear_chunk++;
        if (spu_transfer_write(zeros, amount) != (s32)amount ||
            !WaitEvent((s32)_spu_EVdma)) {
            result = -1;
            break;
        }
        address += amount;
        remaining -= amount;
        if (cpu) spu_clear_after_irq(remaining ? SPU_CLEAR_BETWEEN : SPU_CLEAR_TO_RETURN);
    }
    g_clear_chunk = 0;
    _spu_transferCallback = callback;
    spu_transMode = saved_mode;
    _spu_inTransfer = saved_complete;
    return result;
}
