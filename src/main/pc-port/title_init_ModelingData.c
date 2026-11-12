/* title_init_ModelingData: keep the retail 16-byte object records on x64.
 * @DivergesFrom: src/main/matched/title_init_ModelingData.c -- the coordinate
 * pointer at +4 is stored as a u32 handle; object stride remains 16 bytes.
 * Calls, iteration order, flags and random selection follow the matched C.
 */
#include "types.h"
extern s32 func_800B4870(void);

typedef struct {
    s32 a;
    u32 b;
    s32 c;
    s32 d;
} Rec;

typedef struct {
    u8 x[80];
} Buf;

extern Rec object[];
extern Buf coord_buf[];

void title_init_ModelingData()
{
    s32 *p;
    s32 i;
    s32 r;

    p = (s32 *)0x801F0004;
    GsMapModelingData(p);
    p++;
    if (*p == 13U) {
        p++;
        for (i = 0; i < 16; i++) {
            GsLinkObject4(p, &object[i], 0);
            object[i].b = (u32)(unsigned long)&coord_buf[i];
            object[i].a = 0;
        }
        r = func_800B4870();
        for (i = 0; i < 13; i++) {
            GsLinkObject4(p, &object[i], (r + i) % 13);
            object[i].b = (u32)(unsigned long)&coord_buf[i];
            object[i].a = 0;
        }
    }
}
