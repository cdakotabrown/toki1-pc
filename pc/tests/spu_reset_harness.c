/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Authored register/PIO observations; optional actual vendored SPU core. */
#include "hal.h"
#include "spu_reset.h"
#include "spu_psx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u16 spu_tsa;
s32 tcount, spu_transMode, spu_addrMode;
u32 _spu_IRQCallback;
static u16 registers[256];
static u8 ram[0x1020], source[16], nested_data[16];
static u64 ticks, committed_at, keyed_at, stopped_at;
static int waits, commits, dma, cancel_at, nest_at, nested_result;
#ifdef REAL_CORE
extern void test_core_init(void);
extern u8 test_core_ram(u32 address);
#endif
void *hal_ps1_ptr(const void *p) { return (void *)p; }
int pc_ptr_is_mapped(const void *p, size_t n) { return p && n == 16; }
void spu_reset_enable_dma(void) { dma++; }
void spu_reset_write(u32 offset, u16 value) {
    assert(offset < 512 && !(offset & 1));
    registers[offset/2] = value;
    if (offset == 0x188) keyed_at = ticks;
    if (offset == 0x18c && keyed_at) stopped_at = ticks;
#ifdef REAL_CORE
    spu_psx_write_reg(0x1f801c00u + offset, value);
#endif
}
u16 spu_reset_read(u32 offset) {
#ifdef REAL_CORE
    return spu_psx_read_reg(0x1f801c00u + offset);
#else
    return registers[offset/2];
#endif
}
void spu_transfer_commit(const u8 *data, u32 address, u32 size) {
    assert(address == 0x1000 && size == 16);
    commits++; committed_at = ticks;
    memcpy(ram + address, data, size);
#ifdef REAL_CORE
    spu_psx_dma_write(data, address, size);
#endif
}
void hal_cpu_spend(u32 n) {
    assert(n == 7470);
    ticks += n; waits++;
    if (cancel_at && waits == cancel_at) spu_reset_cancel();
    if (nest_at && waits == nest_at) {
        nest_at = 0;
        nested_result = spu_reset_initialize(nested_data);
    }
}
static void check_selected_voice_registers(void) {
    int channel;
    for (channel = 0; channel < 24; channel++) {
        /* Independent retail cold/repeat/dirty snapshots agree on these
         * six selected fields. Envelope/repeat evolve in the device. */
        assert(spu_reset_read(channel*16) == 0);
        assert(spu_reset_read(channel*16+2) == 0);
        assert(spu_reset_read(channel*16+4) == 0x3fff);
        assert(spu_reset_read(channel*16+6) == 0x200);
        assert(spu_reset_read(channel*16+8) == 0);
        assert(spu_reset_read(channel*16+10) == 0);
        assert(registers[channel*8+6] == 0xa55a);
        assert(registers[channel*8+7] == 0xa55a);
    }
}
int main(int argc, char **argv) {
    int mode, i, result;
    mode = atoi(argv[1]);
#ifdef REAL_CORE
    test_core_init();
#endif
    for (i = 0; i < 256; i++) registers[i] = 0xa55a;
    memset(ram, 0xa5, sizeof(ram));
    for (i = 0; i < 16; i++) {
        source[i] = (u8)i; nested_data[i] = (u8)(100+i);
    }
    tcount = spu_addrMode = 123;
    _spu_IRQCallback = 456;
    if (mode == 1) cancel_at = 3;
    if (mode == 2) cancel_at = 8;
    if (mode == 3) nest_at = 2;
    result = spu_reset_initialize(mode == 4 ? NULL : source);
    if (mode == 0) {
        assert(result == 1 && waits == 13 && ticks == 97110);
        assert(commits == 1 && committed_at == 22410);
        assert(keyed_at == 37350 && stopped_at == 67230);
        assert(dma == 1 && spu_tsa == 512 && spu_transMode == 1);
        assert(tcount == 0 && spu_addrMode == 0 && _spu_IRQCallback == 0);
        check_selected_voice_registers();
        assert(spu_reset_read(0x180) == 0 && spu_reset_read(0x182) == 0);
        assert(spu_reset_read(0x1aa) == 0xc000);
        assert(spu_reset_read(0x1ac) == 4);
        assert(!memcmp(ram+0x1000, source, 16));
#ifdef REAL_CORE
        for (i = 0; i < 16; i++) assert(test_core_ram(0x1000+i)==source[i]);
        /* 0x000..0xfff is hardware capture-buffer RAM, not a guard. */
        assert(test_core_ram(0x1010) == 0xa5);
#endif
    } else if (mode == 1 || mode == 2) {
        assert(!result && waits == cancel_at);
        assert(commits == (mode == 2));
        assert(registers[0x1aa/2] != 0xc000);
        assert(_spu_IRQCallback == 456);
    } else if (mode == 3) {
        assert(!result && nested_result && waits == 15 && commits == 1);
        assert(!memcmp(ram+0x1000, nested_data, 16));
        assert(registers[0x1aa/2] == 0xc000);
    } else {
        assert(!result && !waits && !commits && !dma);
    }
    assert(ram[0xfff] == 0xa5 && ram[0x1010] == 0xa5);
    puts("SPU reset contract passed");
    return 0;
}
