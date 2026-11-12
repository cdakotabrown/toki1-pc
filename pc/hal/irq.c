/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Functional VBlank interrupt controller, not a CPU/BIOS cycle model.
 * Two delivery levels, as retail shows them: the CPU-visible VBlank
 * exception dispatches the kernel's VBlank events (it only needs the CPU
 * enable), and libetc's handler chain needs the I_MASK VBlank bit as well.
 * I_MASK and the CPU enable are independent. Multiple masked edges latch
 * one pending bit. The handler owns acknowledgment, as in trapIntr.
 * Only VBlank is routed here; CD/SPU/card completion IRQs remain separate
 * (pc/hal/cd.c times their exceptions and asks hal_irq_accepts whether one
 * can be taken, and hal_irq_vblank_in_exception for a VBlank it absorbs).
 * Native bootstrap starts SDK-ready (I_MASK=0x000d), not at BIOS reset.
 */
#include "pc_irq.h"

#define VBLANK_IRQ 1u
#define IRQ_BITS 0x7ffu
static u16 mask = 0x000du;
static u16 pending;
static int cpu_enabled = 1;
static int delivering;
static int in_edge;
static u16 saved_mask;
static int saved_cpu;
static u16 saved_edge_mask;
static int saved_edge_cpu;
static HalIrqHandler edge_handler;
static HalIrqHandler vblank_handler;

static void service(void)
{
    HalIrqHandler handler;
    if (delivering || in_edge) return;
    while (cpu_enabled && (mask & pending & VBLANK_IRQ) &&
            vblank_handler) {
        saved_mask = mask;
        saved_cpu = cpu_enabled;
        handler = vblank_handler;
        delivering = 1;
        cpu_enabled = 0;
        handler();
        cpu_enabled = saved_cpu;
        delivering = 0;
    }
}

u32 SetIntrMask(s32 value)
{
    u16 old;
    old = mask;
    mask = (u16)value & IRQ_BITS;
    service();
    return old;
}

u32 GetIntrMask(void)
{
    return mask;
}

s32 hal_irq_enter_critical(void)
{
    int previous;
    previous = cpu_enabled;
    cpu_enabled = 0;
    return previous;
}

void hal_irq_exit_critical(void)
{
    /* BIOS ExitCriticalSection enables interrupts, not a nesting count. */
    cpu_enabled = 1;
    service();
}

void hal_irq_edge(HalIrqHandler handler)
{
    edge_handler = handler;
}

void hal_irq_vblank(HalIrqHandler handler)
{
    vblank_handler = handler;
    pending |= VBLANK_IRQ;
    /* No exception is taken inside a critical section, so neither level
     * runs there. Otherwise the kernel's VBlank events run on the exception
     * itself (with the CPU disabled, as on hardware) and only the libetc
     * chain waits for the I_MASK VBlank bit. */
    if (!cpu_enabled) return;
    if (edge_handler && !in_edge) {
        saved_edge_cpu = cpu_enabled;
        saved_edge_mask = mask;
        cpu_enabled = 0;
        in_edge = 1;
        edge_handler();
        in_edge = 0;
        cpu_enabled = saved_edge_cpu;
    }
    service();
}

/* May an interrupt with I_STAT bit(s) `bits` start an exception now?  It
 * needs the CPU enable and its I_MASK bit, and the CPU must not already be
 * inside a native handler (an interrupt raised there is dispatched by the
 * same trapIntr loop, not by a new exception). */
int hal_irq_accepts(u16 bits)
{
    return cpu_enabled && !delivering && !in_edge && (mask & bits) != 0;
}

/* A VBlank raised while the CPU is inside another interrupt's exception,
 * after the kernel's VBlank verifier has looked at I_STAT: libetc's
 * trapIntr loop finds the bit, acknowledges it and dispatches
 * trapIntrVSync inside that same exception, so the kernel level (its
 * VBlank events and pad poll) never sees this edge.  Returns 0 when the
 * I_MASK VBlank bit is clear: trapIntr leaves the bit latched and a later
 * exception delivers both levels. */
int hal_irq_vblank_in_exception(HalIrqHandler handler)
{
    int previous_cpu;
    if (!(mask & VBLANK_IRQ) || delivering || in_edge) return 0;
    pending |= VBLANK_IRQ;
    previous_cpu = cpu_enabled;
    saved_mask = mask;
    saved_cpu = cpu_enabled;
    delivering = 1;
    cpu_enabled = 0;
    handler();
    cpu_enabled = previous_cpu;
    delivering = 0;
    return 1;
}

/* Is the CPU running a native interrupt handler (either level)?  Game code
 * called from there is exception time, not CPU work (pc/hal/cpu_cost.c). */
int hal_irq_in_handler(void)
{
    return delivering || in_edge;
}

u16 hal_irq_status(void)
{
    return pending;
}

void hal_irq_ack(u16 bits)
{
    pending &= (u16)~bits;
}

void hal_irq_abort(void)
{
    /* Host recovery abandons an IRQ callback before longjmp. Restore the
     * dispatch-entry gates, drop that abandoned latch, and release the
     * reentrancy guards without calling back into the damaged scene. */
    if (delivering) {
        mask = saved_mask;
        cpu_enabled = saved_cpu;
        pending &= (u16)~VBLANK_IRQ;
        delivering = 0;
    }
    if (in_edge) {
        mask = saved_edge_mask;
        cpu_enabled = saved_edge_cpu;
        pending &= (u16)~VBLANK_IRQ;
        in_edge = 0;
    }
}
