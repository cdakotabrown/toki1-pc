/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SpuInit's ready-device reset sequence, from matched _spu_init/_SpuInit.
 * Each real SDK _waitfs is modeled as CPU work, not a boot-phase delay.
 * Initial PIO data becomes visible atomically after its settling loop;
 * FIFO halfword progression and stuck-status timeout paths are not modeled.
 */
#include "hal.h"
#include "spu_reset.h"
#include "profile.h"
#include "spu_transfer.h"
#include <string.h>
#include <stdlib.h>

#include "cpu_cost.h"
extern void hal_cpu_spend(u32 ticks);
extern u16 spu_tsa;
extern s32 tcount, spu_transMode, spu_addrMode;
extern u32 _spu_IRQCallback;
static u32 generation;

void spu_reset_cancel(void) { generation++; }

/* SpuInit's CPU work besides the 13 settles (the reference emulator,
 * SCPH-5500, the boot; _SsInit's entry to SpuVmInit's is 105,891 ticks, the
 * settles 13 x 7,470): _spu_setVoiceAttr's 24-voice loop 3,145, SpuStart
 * (its critical section, _spu_open, DMACallback, the SPU event's
 * OpenEvent/EnableEvent, two _spu_ioctl) and the return to _SsInit 4,715,
 * and _spu_writeByIO with the register writes between the settles, 921.
 * SPU_INIT_CPU=0 is the negative control. */
#define SPU_INIT_REGS_TICKS      921u
#define SPU_SET_VOICE_ATTR_TICKS 3145u
#define SPU_START_TICKS          4715u
int spu_start_net_on(void);
static int init_cpu(void)
{
    return hal_model_on("SPU_INIT_CPU");
}

static int settle(u32 ticket)
{
    hal_cpu_spend(SPU_WAITFS_TICKS);
    return ticket == generation;
}

int spu_reset_initialize(const u8 *silence)
{
    static const u16 zero_regs[] = {
        0x180, 0x182, 0x184, 0x186
    };
    u8 data[16];
    u32 ticket, i, channel;
    u16 control;

    silence = hal_ps1_ptr(silence);
    if (!pc_ptr_is_mapped(silence, sizeof(data))) return 0;
    memcpy(data, silence, sizeof(data));
    ticket = ++generation;
    spu_transMode = 1;
    spu_addrMode = 0;
    spu_tsa = 0;
    if (init_cpu()) hal_cpu_spend(SPU_INIT_REGS_TICKS);
    spu_reset_enable_dma();
    spu_reset_write(0x1aa, 0);
    if (!settle(ticket)) return 0;
    tcount = 0;
    spu_reset_write(0x1ac, 4);
    for (i = 0; i < sizeof(zero_regs)/sizeof(zero_regs[0]); i++)
        spu_reset_write(zero_regs[i], 0);
    spu_reset_write(0x18c, 0xffff);
    spu_reset_write(0x18e, 0xffff);
    spu_reset_write(0x198, 0);
    spu_reset_write(0x19a, 0);
    for (i = 0; i < 4; i++) spu_reset_write(0x190 + 2*i, 0);
    for (i = 0; i < 4; i++) spu_reset_write(0x1b0 + 2*i, 0);

    /* _spu_writeByIO initializes one silent block at address unit 512. */
    spu_tsa = 512;
    spu_reset_write(0x1a6, 512);
    if (!settle(ticket)) return 0;
    control = (spu_reset_read(0x1aa) & 0xffcfu) | 0x10u;
    spu_reset_write(0x1aa, control);
    if (!settle(ticket)) return 0;
    spu_transfer_commit(data, 0x1000, sizeof(data));
    if (!settle(ticket) || !settle(ticket)) return 0;
    spu_reset_write(0x1aa, spu_reset_read(0x1aa) & 0xffcfu);

    /* attr=0x1f selects volumes, pitch, start address and ADSR for all
     * voices. Repeat address and current envelope are not selected. */
    spu_addrMode = 0;
    if (init_cpu()) hal_cpu_spend(SPU_SET_VOICE_ATTR_TICKS);
    for (channel = 0; channel < 24; channel++) {
        i = channel * 16;
        spu_reset_write(i, 0);
        spu_reset_write(i + 2, 0);
        spu_reset_write(i + 4, 0x3fff);
        spu_reset_write(i + 6, 512);
        spu_reset_write(i + 8, 0);
        spu_reset_write(i + 10, 0);
    }
    spu_reset_write(0x188, spu_reset_read(0x188) | 0xffffu);
    spu_reset_write(0x18a, spu_reset_read(0x18a) | 0xffu);
    for (i = 0; i < 4; i++) if (!settle(ticket)) return 0;
    spu_reset_write(0x18c, spu_reset_read(0x18c) | 0xffffu);
    spu_reset_write(0x18e, spu_reset_read(0x18e) | 0xffu);
    for (i = 0; i < 4; i++) if (!settle(ticket)) return 0;
    spu_reset_write(0x1aa, 0xc000);
    _spu_IRQCallback = 0;
    /* SPU_START_NET: SpuInit (pc/hal/spu.c) spends this after
     * spu_transfer_init, net of what that charges. */
    if (init_cpu() && !spu_start_net_on()) hal_cpu_spend(SPU_START_TICKS);
    return 1;
}

/* SPU_START_NET: the 4,715 above is a retail span that contains SpuStart's
 * OpenEvent / EnableEvent of the SPU DMA event; the port's
 * spu_transfer_init opens and enables that event through the BIOS HAL,
 * which charges them too (316 + 62, BIOS_CPU_COST): the boot's _SsInit
 * ran 386 ticks long.  Now SpuInit spends the span less those charges.
 * SPU_START_NET=0 is the negative control. */
int spu_start_net_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("SPU_START_NET"); on = !(e && e[0] == '0'); }
    return on;
}
void spu_start_net_spend(unsigned long long mark)
{
    if (init_cpu() && spu_start_net_on()) hal_cpu_spend(hal_cost_net(SPU_START_TICKS, mark));
}
