/*
 * GsSetRefView2 -- PC pointer-width adapter for
 * src/main/matched/GsSetRefView2.c.  Statement for statement the matched
 * body; only GsRVIEW2.super differs: on the PS1 it is a 32-bit handle at
 * +28, while the matched `void *super` is 8 bytes at +32 on the host and so
 * reads the word AFTER the view (on the title, the first light vector) as
 * the parent coordinate.  It was zero by luck there; any scene with a
 * non-zero word after its GsRVIEW2 would walk a garbage chain.
 * Intentional divergence from the PS1: none -- pointer width only.
 */
#include "types.h"

typedef struct {
    s16 m[3][3];
    s32 t[3];
} RV_MATRIX;

typedef struct {
    s32 vx, vy, vz, pad;
} RV_VECTOR;

typedef struct {
    s32 vpx, vpy, vpz;
    s32 vrx, vry, vrz;
    s32 rz;
    u32 super;          /* GsCOORDINATE2 handle, +28 */
} GsRVIEW2_PS1;

extern RV_MATRIX GsIDMATRIX2;
extern RV_MATRIX GsWSMATRIX;
extern RV_MATRIX GsWSMATRIX_ORG;

extern void gte_rotate_z_matrix(RV_MATRIX *m, s32 rz);
extern void scale_view_param(s32 *arg0, s32 *arg1);
extern s32 SquareRoot0(s32 x);
extern void Gssub_make_matrix(RV_MATRIX *m, s16 sin, s16 cos, s32 axis);
extern void MulMatrix(RV_MATRIX *m0, RV_MATRIX *m1);
extern void ApplyMatrixLV(RV_MATRIX *m, RV_VECTOR *in, RV_VECTOR *out);
extern void GsGetLw(void *chain, RV_MATRIX *m);
extern void TransposeMatrix(RV_MATRIX *in, RV_MATRIX *out);
extern void GsMulCoord2(s32 *a0, s32 *a1);

typedef char GsRVIEW2_PS1_is_32_bytes[sizeof(GsRVIEW2_PS1) == 32 ? 1 : -1];

s32 GsSetRefView2(GsRVIEW2_PS1 *view)
{
    s32 local[8];
    RV_MATRIX m;
    RV_MATRIX m2;
    RV_VECTOR v;
    s32 l;
    s32 r;
    s32 sn;

    GsWSMATRIX = GsIDMATRIX2;
    gte_rotate_z_matrix(&GsWSMATRIX, -view->rz);

    scale_view_param((s32 *)view, local);

    l = SquareRoot0((local[3] - local[0]) * (local[3] - local[0])
                     + (local[4] - local[1]) * (local[4] - local[1])
                     + (local[5] - local[2]) * (local[5] - local[2]));
    if (l == 0)
        return 1;

    r = local[1] - local[4];
    sn = (s32)((u32)r << 12) / l;
    sn = -sn;
    r = SquareRoot0((local[3] - local[0]) * (local[3] - local[0])
                     + (local[5] - local[2]) * (local[5] - local[2]));
    Gssub_make_matrix(&m, sn, (s32)((u32)r << 12) / l, 120);
    MulMatrix(&GsWSMATRIX, &m);

    if (r != 0) {
        l = r;
        r = local[3] - local[0];
        sn = (s32)((u32)r << 12) / l;
        r = local[5] - local[2];
        Gssub_make_matrix(&m, -sn, (s32)((u32)r << 12) / l, 121);
        MulMatrix(&GsWSMATRIX, &m);
    }

    v.vx = -view->vpx;
    v.vy = -view->vpy;
    v.vz = -view->vpz;
    ApplyMatrixLV(&GsWSMATRIX, &v, (RV_VECTOR *)GsWSMATRIX.t);

    if (view->super) {
        GsGetLw((void *)(unsigned long)view->super, &m);
        TransposeMatrix(&m, &m2);
        ApplyMatrixLV(&m2, (RV_VECTOR *)m.t, &v);
        m2.t[0] = -v.vx;
        m2.t[1] = -v.vy;
        m2.t[2] = -v.vz;
        GsMulCoord2((s32 *)&GsWSMATRIX, (s32 *)&m2);
        GsWSMATRIX = m2;
    }

    GsWSMATRIX_ORG = GsWSMATRIX;
    return 0;
}
