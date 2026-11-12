/*
 * data_load_exit — PC port loose translation from ASM
 * Address: 0x80068A14
 * Size: 660 bytes (165 instructions)
 * TODO: verify logic, get byte-perfect match later
 */
#include "types.h"

/* External globals */
extern u32 cal_font_load_tbl0[];  /* Lookup table for sprite setup data */
extern u8 screen_w[];   /* Save/load data buffer 1 */
extern u8 cal_day_kind[];   /* Save/load data buffer 2 */
extern u8 cal_days_may[];   /* SPU data buffer */

/* External function declarations */
extern void load_image_rect(s32 a0, s32 a1, s32 a2, s32 a3, u32 arg4);
extern void func_800B4840(u8 *src, u8 *dst, s32 len);
extern void func_800B4860(u8 *data);

typedef struct {
    u32 field0;
    u32 field4;
    u32 field8;
} SPUData;

/*
 * Data load exit handler with switch on combined index.
 * Hidden v0 parameter (combined index from thunk).
 * Switch on v0-1144 with 35 cases. Manages save/load data
 * buffers screen_w/cal_day_kind/cal_days_may with cal_font_load_tbl0 lookup.
 */
void cal_font_load(u32 v0_hidden, s32 a1, u32 a2)
{
    s32 index;
    u32 *lookup_entry;
    u8 *src1, *src2;
    SPUData *spu;
    s32 byte_count, i, j;
    s32 offset;
    u8 *dst;

    /* Extract index: v0 - 1144 */
    index = v0_hidden - 1144;

    /* Bounds check: must be < 35 */
    if (index >= 35) {
        return;
    }

    /* Load lookup table entry */
    lookup_entry = &cal_font_load_tbl0[index];
    offset = index << 2;

    /* Call load_image_rect twice for sprite setup */
    load_image_rect(
        928,      /* a0 */
        256,      /* a1 */
        32,       /* a2 */
        128,      /* a3 */
        lookup_entry[0]  /* arg4 */
    );

    load_image_rect(
        928,      /* a0 */
        384,      /* a1 */
        32,       /* a2 */
        128,      /* a3 */
        lookup_entry[1]  /* arg4 (offset +4) */
    );

    /* Check a2 flag */
    if (a2 == 0) {
        return;
    }

    /* Copy buffer data */
    func_800B4840(cal_day_kind, screen_w, 64);

    /* Load SPU data */
    spu = (SPUData *)lookup_entry[1];
    byte_count = *(u8 *)(((u8 *)spu) + 8192);
    u8 spu_field2 = *(u8 *)(((u8 *)spu) + 8196);

    /* Store byte counts */
    cal_day_kind[0] = byte_count;
    cal_day_kind[28] = spu_field2;  /* Offset -6140 from lui base */

    if (byte_count <= 0) {
        return;
    }

    /* Process SPU data in chunks */
    dst = cal_days_may;

    /* Unaligned copy loop (byte_count & 0x3 != 0) */
    if ((byte_count & 0x3) != 0) {
        s32 remainder = byte_count & 0x3;
        u8 *src = (u8 *)spu + 8192;

        for (i = 0; i < remainder; i++) {
            dst[133 + i] = src[8200 + i * 4];
        }

        dst += remainder;
        i = remainder;

        /* Continue aligned copy */
        while (i < byte_count) {
            u8 *aligned_src = (u8 *)spu + 8192;
            dst[133] = aligned_src[8200 + i * 4];
            dst[161] = aligned_src[8204 + i * 4];
            dst[130] = aligned_src[8208 + i * 4];
            dst[162] = aligned_src[8212 + i * 4];

            dst[132] = 0;  /* padding */

            dst += 4;
            i += 4;
        }
    } else {
        /* Aligned copy loop */
        u8 *aligned_base = (u8 *)spu + 8192;
        for (i = 0; i < byte_count; i += 4) {
            dst[133] = aligned_base[8200 + i];
            dst[161] = aligned_base[8204 + i];
            dst[130] = aligned_base[8208 + i];
            dst[162] = aligned_base[8212 + i];

            dst += 4;
        }
    }
}
