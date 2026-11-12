/*
 * graph_init -- PC override of src/main/matched/graph_init.c (0x80041000).
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  ot_work0_org / ot_work1_org are the 4-byte `org` words
 * of the two PS1 GsOT headers (0x800F1154 / 0x800F1168: length, org,
 * offset, point, tag).  The matched C declares them `void *`, 8 bytes on
 * LP64, so each store also zeroed the header's `offset` word (+8) --
 * latent at boot, where it is 0 already.  Here
 * they are u32 and hold the PS1 address of ot_buff0 / ot_buff1, the value
 * the console stores and every PS1-layout reader of the header expects.
 *
 * @DivergesFrom: matched graph_init -- pointer store width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "common.h"

/* External function declarations */
extern void GsInitGraph(s32 width, s32 height, s32 x, s32 y, s32 flag);
extern void GsSetDrawBuffOffset(void);
extern void GsDefDispBuff(s32 x, s32 y, s32 w, s32 h);
extern void GsInit3D(void);
extern void InitGeom(void);
extern void GsInitVcount(void);
extern s32 GsGetActiveBuff(void);
extern void draw_and_disp_env_init(void);

/* Global variables - OT (ordering table) structures */
extern s32 ot_work;       /* OT depth/count for buffer 1 */
extern u32 ot_work0_org;   /* PS1 pointer */     /* OT pointer for buffer 1 */
extern s32 ot_work1_len;       /* OT depth/count for buffer 2 */
extern u32 ot_work1_org;   /* PS1 pointer */     /* OT pointer for buffer 2 */
extern u8 ot_buff0[1024];  /* OT buffer 1 (8 entries * 4 bytes = 32B used) */
extern u8 ot_buff1[1024];  /* OT buffer 2 (8 entries * 4 bytes = 32B used) */
extern s32 activeBuff;       /* Stored return value from GsGetActiveBuff */

void graph_init(void)
{
    /* Initialize display environment (320x240 resolution) */
    GsInitGraph(320, 240, 0, 0, 0);
    GsSetDrawBuffOffset();

    /* Initialize draw environment */
    GsDefDispBuff(0, 0, 0, 240);

    /* Set up double-buffered ordering tables */
    ot_work = 8;              /* OT depth = 8 (2^8 = 256 levels) */
    ot_work0_org = (u32)(uintptr_t)ot_buff0;     /* Point to OT buffer 1 */
    ot_work1_len = 8;              /* OT depth = 8 */
    ot_work1_org = (u32)(uintptr_t)ot_buff1;     /* Point to OT buffer 2 */

    /* Complete initialization sequence */
    GsInit3D();
    InitGeom();
    GsInitVcount();

    /* Store initialization result */
    activeBuff = GsGetActiveBuff();

    /* Call next initialization function */
    draw_and_disp_env_init();
}
