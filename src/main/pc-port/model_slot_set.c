/* model_slot_set: preserve the coordinate handle at retail object+4.
 * @DivergesFrom: src/main/matched/model_slot_set.c -- a u32 address store
 * replaces a native pointer store that overwrites the adjacent TMD handle.
 * The matched calls, bounds, return values and flag stores are unchanged.
 */
#include "types.h"

extern u8 object[];
extern u8 coord_buf[];

extern void GsMapModelingData(s32 *hdr);
extern void GsLinkObject4(s32 *hdr, u8 *slot, u32 idx);

s32 model_slot_set(s32 *arg0, s32 arg1, u32 arg2)
{
    s32 *p;
    s32 lim;
    u8 *rec;

    p = arg0 + 1;
    GsMapModelingData(p);
    p = p + 1;
    lim = *p;
    if ((u32)lim >= 17) {
        lim = 15;
    }
    p = p + 1;
    if (arg2 < (u32)lim) {
        rec = &object[arg1 * 16];
        GsLinkObject4(p, rec, arg2);
        *(u32 *)(rec + 4) = (u32)(unsigned long)&coord_buf[arg1 * 80];
        *(s32 *)rec = 0;
        return 1;
    }
    return -1;
}
