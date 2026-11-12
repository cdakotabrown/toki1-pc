/* initModelingData_init: keep the retail 16-byte object records on x64.
 * @DivergesFrom: src/main/matched/initModelingData_init.c -- the coordinate
 * pointer at +4 is stored as a u32 handle; object stride remains 16 bytes.
 * Calls, iteration order, flags and random selection follow the matched C.
 */
#include "types.h"

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

void initModelingData_init(buf)
    s32 *buf;
{
    s32 *p;
    s32 i;

    p = buf + 1;
    GsMapModelingData(p);
    p++;
    p++;
    for (i = 0; i < 16; i++) {
        GsLinkObject4(p, &object[i], 0);
        object[i].b = (u32)(unsigned long)&coord_buf[i];
        object[i].a = 0x80000000;
    }
}
