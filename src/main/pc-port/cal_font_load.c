/* cal_font_load: adapt the retail pointer-to-pointer through u32 loads.
 * @DivergesFrom: src/main/matched/cal_font_load.c -- the global and every
 * picture table entry occupy four bytes, not eight. Widen only at use.
 * Calendar index, picture uploads, trailer loops and BIOS copy are unchanged.
 */
#include "types.h"

extern u32 cal_font_load_tbl0;
#define FONT(i) ((s32 *)(unsigned long)((u32 *)(unsigned long)cal_font_load_tbl0)[i])
typedef struct {
    u8 pad0[68];
    u8 prev[64];    /* +68  (0x800EE7A4) */
    u8 cnt;         /* +132 (0x800EE7E4) */
    u8 a[31];       /* +133 */
    u8 cnt2;        /* +164 (0x800EE804) */
    u8 b[31];       /* +165 */
} Save;

extern Save screen_w;

extern void load_image_rect();
extern void *func_800B4840(void *, const void *, u32);

void cal_font_load(month, day, flag)
    u8 month;
    u8 day;
    s32 flag;
{
    s32 idx;
    s32 n;
    s32 i;
    Save *sv;
    s32 *data;
    s32 m;

    idx = month * 12 + day - 1144;
    if (idx >= 35) {
        return;
    }
    load_image_rect(928, 256, 32, 128, FONT(idx));
    load_image_rect(928, 384, 32, 128, FONT(idx + 1));
    if (flag == 0) {
        return;
    }
    sv = &screen_w;
    func_800B4840(screen_w.prev, &screen_w.cnt, 64);
    data = FONT(idx + 1) + 2048;
    n = data[0];
    m = data[1];
    screen_w.cnt = n;
    screen_w.cnt2 = m;
    for (i = 0; i < n; i++) {
        sv->a[i] = FONT(idx + 1)[2050 + i];
        sv->b[i] = FONT(idx + 1)[2050 + n + i];
    }
}
