/*
 * GsSortSprite -- PC pointer-width adapter for matched/GsSortSprite.c.
 * Statement-for-statement matched body; only host pointer width diverges.
 * GsOUT_PACKET_P is a four-byte PS1 handle, never a native pointer cell.
 * Ordering-table records remain 12 bytes, with org at +4 and offset at +8.
 * Handles are zero-extended at use sites; packet links retain PS1 width.
 * See mv_state.h for the host-adaptation doctrine.
 */
#include "types.h"
#include "psyq/stddef.h"
#include "psyq/libgte.h"

/* GsSPRITE (include/psyq/libgs.h layout, reproduced locally -- libgs.h
 * itself pulls in GPU primitive types this TU does not need). */
typedef struct {
    u32 attribute;
    s16 x, y;
    u16 w, h;
    u16 tpage;
    u8  u, v;
    s16 cx, cy;
    u8  r, g, b;
    s16 mx, my;
    s16 scalex, scaley;
    s32 rotate;
} GsSPRITE;

/* GsOT -- opaque here; this function only forwards the pointer. */
typedef struct GsOT GsOT;

extern u32 GsOUT_PACKET_P;   /* current write cursor in the shared packet buffer */
extern s16   GsORGOFSX;   /* global screen X offset */
extern s16   GsORGOFSY;   /* global screen Y offset */
extern MATRIX GsIDMATRIX;  /* identity matrix template */

extern MATRIX *RotMatrix(SVECTOR *r, MATRIX *m);    /* RotMatrix (GTE asm_only; signature per psyq/libgte.h) */
extern s32   ReadGeomScreen(void);                        /* ReadGeomScreen (translation.z) */
/* ScaleMatrix/TransMatrix/SetRotMatrix/SetTransMatrix: prototypes from psyq/libgte.h */
extern void  RotTransPers4(SVECTOR *c0, SVECTOR *c1, SVECTOR *c2, SVECTOR *c3, /* RotTransPers4 */
                            s32 *o0, s32 *o1, s32 *o2, s32 *o3, s32 *o4, s32 *o5);
extern void *_make_packet(void *pk, GsOT *otp, s32 pri, s32 nWords); /* _make_packet */

void GsSortSprite(GsSPRITE *sp, GsOT *otp, u16 pri)
{
    u32 attr;
    u32 *pk;
    s32 fast;
    MATRIX mtx;
    SVECTOR rvec;
    s32 vec3[3];
    SVECTOR corner[4];
    s32 sxy0, sxy1, sxy2, sxy3, p, flag;
    u8 uA, uB, vA, vB;
    s32 nmx, nmy;
    s32 x, y;

    attr = sp->attribute;
    if ((s32)attr < 0 || sp->w == 0 || sp->h == 0) {
        return;
    }

    pk = (u32 *)(unsigned long)GsOUT_PACKET_P;
    fast = ((attr >> 0x1B) & 1) |
           ((*(u32 *)&sp->scalex == 0x10001000 && sp->rotate == 0) &&
            ((attr & 0xC00000) == 0));

    if (fast) {
        x = sp->x + GsORGOFSX;
        y = sp->y + GsORGOFSY;
        pk[1] = (sp->tpage & 0x1F) | 0xE1000200 | ((attr >> 0x11) & 0x180) |
                ((attr >> 0x17) & 0x60);
        pk[2] = ((attr >> 5) & 0x02000000) |
                0x64000000 | ((attr << 0x12) & 0x01000000) |
                (sp->b << 16) | (sp->g << 8) | sp->r;
        pk[3] = ((u32)(x - sp->mx) & 0xFFFF) | ((u32)(y - sp->my) << 16);
        pk[4] = sp->u | (sp->v << 8) | (sp->cy << 22) | ((sp->cx << 12) & 0x3F0000);
        pk[5] = sp->w | (sp->h << 16);

        pk = (u32 *)_make_packet(pk, otp, pri, 5);
    } else {

        if (sp->rotate == 0) {
            mtx = GsIDMATRIX;
        } else {
            rvec.vx = 0;
            rvec.vy = 0;
            rvec.vz = (s16)(sp->rotate / 360);
            RotMatrix(&rvec, &mtx);
        }

        if (*(u32 *)&sp->scalex != 0x10001000) {
            vec3[0] = sp->scalex;
            vec3[1] = sp->scaley;
            vec3[2] = 0;
            ScaleMatrix(&mtx, (VECTOR *)vec3);
        }

        vec3[0] = sp->x;
        vec3[1] = sp->y;
        vec3[2] = ReadGeomScreen();
        TransMatrix(&mtx, (VECTOR *)vec3);

        SetRotMatrix(&mtx);
        SetTransMatrix(&mtx);

        nmx = -sp->mx;
        nmy = -sp->my;
        corner[0].vx = nmx;
        corner[0].vy = nmy;
        corner[0].vz = 0;
        corner[1].vx = nmx + sp->w;
        corner[1].vy = nmy;
        corner[1].vz = 0;
        corner[2].vx = nmx;
        corner[2].vy = nmy + sp->h;
        corner[2].vz = 0;
        corner[3].vx = nmx + sp->w;
        corner[3].vy = nmy + sp->h;
        corner[3].vz = 0;
        RotTransPers4(&corner[0], &corner[1], &corner[2], &corner[3],
                      &sxy0, &sxy1, &sxy2, &sxy3, &p, &flag);

        if (attr & 0x800000) {
            uA = sp->u + sp->w - 1;
            uB = sp->u;
        } else {
            uA = sp->u;
            uB = sp->u + sp->w - 1;
        }
        if (attr & 0x400000) {
            vA = sp->v + sp->h - 1;
            vB = sp->v;
        } else {
            vA = sp->v;
            vB = sp->v + sp->h - 1;
        }

        pk[1] = ((attr >> 5) & 0x02000000) |
                0x2C000000 | ((attr << 0x12) & 0x01000000) |
                (sp->b << 16) | (sp->g << 8) | sp->r;
        pk[2] = sxy0;
        pk[3] = uA | (vA << 8) | (sp->cy << 22) | ((sp->cx << 12) & 0x3F0000);
        pk[4] = sxy1;
        pk[5] = uB | (vA << 8) | ((sp->tpage & 0x1F) << 16) |
                ((attr >> 1) & 0x1800000) | ((attr >> 7) & 0x600000);
        pk[6] = sxy2;
        pk[7] = uA | (vB << 8);
        pk[8] = sxy3;
        pk[9] = uB | (vB << 8);

        pk = (u32 *)_make_packet(pk, otp, pri, 9);
    }
    GsOUT_PACKET_P = (u32)(unsigned long)pk;
}
