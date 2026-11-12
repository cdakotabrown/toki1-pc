/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libetc VBlank callback control with native-width function pointers.
 * Mirrors startIntr/setIntr/setIntrVSync/stopIntr and uses the unchanged
 * trapIntrVSync body. The packed PS1 table and trampoline vtable cannot
 * forward native arguments or hold 64-bit callbacks. Hardware IRQs other
 * than VBlank still use their existing HAL completion paths. DMA callback
 * registration/dispatch, BIOS exception timing and raw MMIO remain open.
 */
#include "pc_irq.h"
#include "profile.h"
#include <string.h>

extern volatile s32 Vcount;
extern u16 libapi_intr_env, libapi_intr_in_intr_flag;
extern u16 libapi_vsync_intr_enabled, libapi_dma_intr_enabled;
extern void trapIntrVSync(void), trapIntrDMA(void);
/* The matched handler is compiled with libapi_vsync_callback renamed to this native
 * pointer. The retail four-byte hook slot abuts Vcount and cannot widen. */
HalIrqHandler volatile hal_sdk_vsync_hook;
static HalIrqHandler handlers[11];

HalIrqHandler InterruptCallback(s32 index, HalIrqHandler handler)
{
    HalIrqHandler old;
    u32 mask, bit;
    if (index < 0 || index >= 11) return 0;
    old = handlers[index];
    if (old == handler || !libapi_intr_env) return old;
    mask = SetIntrMask(0);
    handlers[index] = handler;
    bit = 1u << index;
    SetIntrMask(handler ? mask | bit : mask & ~bit);
    return old;
}

HalIrqHandler VSyncCallback(HalIrqHandler handler)
{
    HalIrqHandler old;
    old = hal_sdk_vsync_hook;
    if (libapi_vsync_intr_enabled) hal_sdk_vsync_hook = handler;
    return old;
}

/* The first ResetCallback (_main's, libetc startIntr's real set-up) is
 * 12,491 ticks on the console from its entry to _main's next call (CdInit):
 * EnterCriticalSection, memclr, setjmp, startIntrVSync / startIntrDMA,
 * _96_remove, HookEntryInt, ExitCriticalSection -- four syscall / BIOS
 * exceptions, the last delivering the VBlank latched during the EXE's
 * start-up (the reference emulator, SCPH-5500, the boot).  Later calls return
 * at once (24 ticks, the cost table's row).  BOOT_CPU_COST=0 is the
 * negative control. */
#define RESETCALLBACK_FIRST_TICKS 12491u
extern void hal_cpu_spend(u32 ticks) __attribute__((weak));
#include <stdlib.h>

void ResetCallback(void)
{
    if (libapi_intr_env) return;
    {
        if (hal_cpu_spend && hal_model_on("BOOT_CPU_COST"))
            hal_cpu_spend(RESETCALLBACK_FIRST_TICKS);
    }
    hal_irq_enter_critical();
    memset(handlers, 0, sizeof(handlers));
    libapi_intr_env = 1;
    libapi_intr_in_intr_flag = 0;
    hal_sdk_vsync_hook = 0;
    libapi_vsync_intr_enabled = 1;
    libapi_dma_intr_enabled = 1;
    InterruptCallback(0, trapIntrVSync);
    InterruptCallback(3, trapIntrDMA);
    hal_irq_exit_critical();
}

void StopCallback(void)
{
    hal_irq_enter_critical();
    libapi_intr_env = 0;
    libapi_intr_in_intr_flag = 0;
    memset(handlers, 0, sizeof(handlers));
    SetIntrMask(0);
    hal_irq_ack(0x7ffu);
    libapi_vsync_intr_enabled = 0;
    libapi_dma_intr_enabled = 0;
    hal_sdk_vsync_hook = 0;
    hal_irq_exit_critical();
}

void hal_sdk_irq_vblank(void)
{
    HalIrqHandler handler;
    handler = handlers[0];
    if (!libapi_intr_env || !handler) return;
    libapi_intr_in_intr_flag = 1;
    handler();
    libapi_intr_in_intr_flag = 0;
}

void hal_sdk_irq_abort(void)
{
    libapi_intr_in_intr_flag = 0;
}

s32 hal_sdk_vcount(void)
{
    return Vcount;
}
