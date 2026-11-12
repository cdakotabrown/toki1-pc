/*
 * GsSetFlatLight -- PC record-width adapter for matched/GsSetFlatLight.c.
 *
 * Statement-for-statement the matched body; only the two record types
 * change width.  The matched C declares Sony's
 *     MATRIX    { short m[3][3]; long t[3]; }
 *     GsF_LIGHT { long vx, vy, vz; unsigned char r, g, b, pad; }
 * and `long` is 8 bytes on the host, so it read the light direction at
 * +0/+8/+16 and the colour at +24 of a 16-byte PS1 record (vy came from
 * vz, vz and the colour from the NEXT light), and its 48-byte MATRIX
 * copies to and from GsLIGHTWSMATRIX (a 32-byte PS1 object at 0x80131740)
 * overran into _LC at 0x80131760.  Here both use the PS1 widths (s32).
 * Reached: title_init_Light, TACO, DATE, TAIIKU.
 */
#include "types.h"

typedef struct {
    s16 m[3][3];
    s32 t[3];
} PsMATRIX;

typedef struct {
    s32 vx, vy, vz;
    u8 r, g, b, pad;
} PsF_LIGHT;

extern PsMATRIX GsLIGHTWSMATRIX;
extern void gte_read_lc(PsMATRIX *m);
extern void gte_set_lc(PsMATRIX *m);
extern s32 SquareRoot0(s32 x);
extern s32 printf(const char *fmt, ...);
extern char libgs_flatlight_len0_msg[];                /* "GsSetFlatLight len = 0\n" */

void GsSetFlatLight(s32 id, PsF_LIGHT *lt)
{
    PsMATRIX l;
    PsMATRIX c;
    s32 len;
    s32 r, g, b;

    r = lt->r;
    g = lt->g;
    b = lt->b;
    l = GsLIGHTWSMATRIX;
    gte_read_lc(&c);
    len = SquareRoot0(lt->vx * lt->vx + lt->vy * lt->vy + lt->vz * lt->vz);
    if (len == 0) {
        printf(libgs_flatlight_len0_msg);
        return;
    }
    switch (id) {
    case 0:
        l.m[0][0] = -lt->vx * 4096 / len;
        l.m[0][1] = -lt->vy * 4096 / len;
        l.m[0][2] = -lt->vz * 4096 / len;
        c.m[0][0] = r * 4096 / 255;
        c.m[1][0] = g * 4096 / 255;
        c.m[2][0] = b * 4096 / 255;
        break;
    case 1:
        l.m[1][0] = -lt->vx * 4096 / len;
        l.m[1][1] = -lt->vy * 4096 / len;
        l.m[1][2] = -lt->vz * 4096 / len;
        c.m[0][1] = r * 4096 / 255;
        c.m[1][1] = g * 4096 / 255;
        c.m[2][1] = b * 4096 / 255;
        break;
    case 2:
        l.m[2][0] = -lt->vx * 4096 / len;
        l.m[2][1] = -lt->vy * 4096 / len;
        l.m[2][2] = -lt->vz * 4096 / len;
        c.m[0][2] = r * 4096 / 255;
        c.m[1][2] = g * 4096 / 255;
        c.m[2][2] = b * 4096 / 255;
        break;
    }
    GsLIGHTWSMATRIX = l;
    gte_set_lc(&c);
}
