/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPU register bridge and core-timing regressions (pc/tools/test_spu_mmio.py).
 *
 * Links the actual pc/hal/spu.c (SPU_SOURCE) and the actual SPU core archive
 * (pc/3rd/spu-core/spu_psx.a, pc/3rd/SPU-CORE.md).  Only the SDL device lock and the
 * pointer-map query are spied; the 0x1F801C00 register page is a real fixed
 * mapping, as in pc/src/main.c.  No assets, game code or audio device.
 *
 * Cases (argv[1]):
 *   reads    -- register reads must not advance the SPU: a voice keyed on,
 *               then read back 2,000 times, renders the same first samples
 *               as one that was not read.
 *   bridge   -- writes made through the page reach the core in flush order;
 *               KON/KOFF are consumed (read back 0) and the same mask keys
 *               the voice again; hardware-owned registers are read back.
 *   reverb   -- the reverb registers the matched libspu writes (mBASE, the
 *               0x1C0 block, vLOUT/vROUT, EON) and SPUCNT's reverb enable
 *               reach the core; SPUCNT's transfer mode stays the HAL's; an
 *               EON voice fills the work area only with the enable set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef SPU_SOURCE
#define SPU_SOURCE "../hal/spu.c"
#endif
#include SPU_SOURCE

void SDLCALL SDL_LockAudioDevice(SDL_AudioDeviceID device) { (void)device; }
void SDLCALL SDL_UnlockAudioDevice(SDL_AudioDeviceID device) { (void)device; }
int pc_ptr_is_mapped(const void *p, unsigned long n)
{
    unsigned long a = (unsigned long)p;
    return a >= 0x1F801000ul && a + n <= 0x1F802000ul;
}

#define PAGE ((volatile u16 *)0x1F801C00ul)
#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); return 1; } while (0)

/* One looping ADPCM sample at SPU RAM 0x2000: a loop-start block of a
 * constant non-zero nibble pattern, then an end+repeat block. */
static void load_sample(void)
{
    u8 blocks[32];
    int i;
    memset(blocks, 0, sizeof(blocks));
    blocks[0] = 0x00; blocks[1] = 0x04;              /* shift 0, loop start */
    for (i = 2; i < 16; i++) blocks[i] = 0x77;
    blocks[16] = 0x00; blocks[17] = 0x03;            /* end + repeat */
    for (i = 18; i < 32; i++) blocks[i] = 0x77;
    spu_psx_dma_write(blocks, 0x2000, sizeof(blocks));
}

static void voice0(void (*write)(u32, u16))
{
    write(0x00, 0x3FFF);                              /* volume L/R */
    write(0x02, 0x3FFF);
    write(0x04, 0x1000);                              /* pitch 44.1 kHz */
    write(0x06, 0x2000 >> 3);                         /* start address */
    write(0x08, 0x00FF);                              /* fastest linear attack */
    write(0x0A, 0x0000);
}

static void core_write(u32 off, u16 v) { spu_psx_write_reg(0x1F801C00u + off, v); }
static void page_write(u32 off, u16 v) { PAGE[off / 2] = v; }

static int render_after(int reads, s16 *out)
{
    int i, peak = 0;
    spu_psx_reset();
    core_write(0x1AA, 0xC000);
    core_write(0x180, 0x3FFF);
    core_write(0x182, 0x3FFF);
    load_sample();
    voice0(core_write);
    core_write(0x188, 1);
    for (i = 0; i < reads; i++) (void)spu_psx_read_reg(0x1F801C0C);
    spu_psx_mix(out, 64);
    for (i = 0; i < 64 * 2; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    return peak;
}

/* libspu's Studio Medium preset (mode 3, as SpuSetReverbModeParam copies
 * it from the retail table): 0x1C0..0x1FE, work area 0x4840 bytes. */
static const u16 studio_medium[32] = {
    0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
    0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
    0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
    0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000
};
#define REVERB_BASE 0x7B7C0u

/* Configure through the page as the game does, key voice 0, mix; return
 * the number of non-zero bytes in the work area. */
static int reverb_run(u16 spucnt, s16 *out, int frames)
{
    static u8 area[0x80000 - REVERB_BASE];
    int i, nonzero = 0;
    spu_psx_reset();
    spu_psx_write_reg(0x1F801DAA, 0xC000);
    load_sample();
    spu_mmio_start();
    page_write(0x180, 0x3FFF);
    page_write(0x182, 0x3FFF);
    voice0(page_write);
    for (i = 0; i < 32; i++) page_write(0x1C0 + 2 * i, studio_medium[i]);
    page_write(0x1A2, REVERB_BASE >> 3);
    page_write(0x184, 0x3C78);
    page_write(0x186, 0x3C78);
    page_write(0x198, 1);
    page_write(0x1AA, spucnt);
    page_write(0x188, 1);
    spu_mmio_sync();
    for (i = 0; i < 16; i++) spu_psx_mix(out, frames);
    spu_psx_dma_read(area, REVERB_BASE, sizeof(area));
    for (i = 0; i < (int)sizeof(area); i++) nonzero += area[i] != 0;
    return nonzero;
}

int main(int argc, char **argv)
{
    void *page;
    if (argc != 2) return 90;
    page = mmap((void *)0x1F801000ul, 0x1000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (page == MAP_FAILED) FAIL("mmap\n");
    spu_psx_init();

    if (!strcmp(argv[1], "reads")) {
        s16 plain[64 * 2], read[64 * 2];
        if (render_after(0, plain) == 0) FAIL("voice silent without reads\n");
        render_after(2000, read);
        if (memcmp(plain, read, sizeof(plain)))
            FAIL("register reads advanced the SPU (first samples %d vs %d)\n",
                 read[0], plain[0]);
        return 0;
    }

    if (!strcmp(argv[1], "bridge")) {
        s16 out[256 * 2];
        int i, peak = 0;
        spu_psx_reset();
        spu_psx_write_reg(0x1F801DAA, 0xC000);
        load_sample();
        spu_mmio_start();
        if (!g_mmio_live) FAIL("bridge not started\n");
        page_write(0x180, 0x3FFF);
        page_write(0x182, 0x3FFF);
        voice0(page_write);
        page_write(0x188, 1);
        spu_mmio_sync();
        if (spu_psx_read_reg(0x1F801C06) != (0x2000 >> 3) ||
            spu_psx_read_reg(0x1F801C04) != 0x1000 ||
            spu_psx_read_reg(0x1F801D80) != 0x3FFF)
            FAIL("page writes did not reach the core\n");
        if (PAGE[0x188 / 2] != 0) FAIL("KON not consumed\n");
        spu_psx_mix(out, 256);
        for (i = 0; i < 256 * 2; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
        if (peak == 0) FAIL("keyed voice silent\n");
        spu_mmio_sync();
        if (PAGE[0x0C / 2] == 0 || PAGE[0x0C / 2] != spu_psx_read_reg(0x1F801C0C))
            FAIL("ADSR current volume not read back (%04x)\n", PAGE[0x0C / 2]);
        /* Key off, release to silence, then the same KON mask again. */
        page_write(0x18C, 1);
        spu_mmio_sync();
        if (PAGE[0x18C / 2] != 0) FAIL("KOFF not consumed\n");
        for (i = 0; i < 400; i++) spu_psx_mix(out, 256);
        spu_mmio_sync();
        if (PAGE[0x0C / 2] != 0) FAIL("voice did not release (%04x)\n", PAGE[0x0C / 2]);
        page_write(0x188, 1);
        spu_mmio_sync();
        spu_psx_mix(out, 256);
        spu_mmio_sync();
        if (PAGE[0x0C / 2] == 0) FAIL("repeated KON mask did not key the voice\n");
        return 0;
    }
    if (!strcmp(argv[1], "reverb")) {
        static s16 on[1024 * 2], off[1024 * 2];
        int filled;
        /* SPUCNT as _spu_ioctl 96 leaves it with a DMA read in progress
         * (_spu_r's mode 3 in bits 4-5): only the mode stays out. */
        filled = reverb_run(0xC0B0, on, 1024);
        if (spu_psx_read_reg(0x1F801DAA) != 0xC080)
            FAIL("SPUCNT %04x, want the enable without the transfer mode\n",
                 spu_psx_read_reg(0x1F801DAA));
        if (spu_psx_read_reg(0x1F801DA2) != (REVERB_BASE >> 3) ||
            spu_psx_read_reg(0x1F801DC4) != 0x70F0 ||
            spu_psx_read_reg(0x1F801DFE) != 0x8000 ||
            spu_psx_read_reg(0x1F801D84) != 0x3C78 ||
            spu_psx_read_reg(0x1F801D98) != 1)
            FAIL("reverb registers did not reach the core\n");
        if (filled == 0) FAIL("reverb enabled but the work area stayed empty\n");
        if (reverb_run(0xC000, off, 1024) != 0)
            FAIL("reverb disabled but the work area was written\n");
        if (!memcmp(on, off, sizeof(on))) FAIL("reverb did not change the output\n");
        return 0;
    }
    return 91;
}
