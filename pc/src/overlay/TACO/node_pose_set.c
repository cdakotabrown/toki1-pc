/* SPDX-License-Identifier: GPL-3.0-or-later */
/* node_pose_set -- PC override of src/overlay/TACO/node_pose_set.c.
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/basyo_init.c doctrine).  The matched
 * Node16 declares the node's record handle as `Rec *f4`: 4 bytes on the
 * console, 8 bytes (8-aligned) on LP64, where the record is 24 bytes, so
 * &taco_obj[idx] strides 24 and f4 is read from host offset +8 (the
 * node's f8 word fused with fc).  The writer, taco_actor_alloc, declares
 * the same table as four s32 (16 bytes on both) and stores
 * `taco_obj[i].f4 = (s32)&taco_coord[i]`, the PS1 address of an 80-byte
 * pose record; GsLinkObject4 is handed &taco_obj[i] at that 16-byte
 * stride.  Here the node is the 16-byte PS1 record with f4 a u32 handle,
 * dereferenced as the PS1 address it holds.  Rec/Mtx have no pointers and
 * keep their layout.
 *
 * @DivergesFrom: src/overlay/TACO/node_pose_set.c -- record field widths
 * only.  Test: pc/tools/test_ps1_handle_records.py.
 */
#include "types.h"

typedef struct {
    s16 m[3][3];
    s32 t[3];
} Mtx;

typedef struct {
    s32 flag;
    Mtx mat;
    u8 rest[44];
} Rec;

typedef struct {
    s32 f0;
    u32 f4;     /* PS1 handle: Rec */
    s32 f8;
    s32 fc;
} Node16;

/* C89 compile-time layout check: the console's 16-byte node. */
typedef char node_pose_set_node_is_16[(sizeof(Node16) == 16) ? 1 : -1];

extern Node16 taco_obj[];
extern u8 taco_pose_align_fmt[];

extern void printf();

s32 node_pose_set(s32 idx, s32 n, s16 *pA, s16 *pB, u8 f4, u8 f5)
{
    Mtx m;
    Node16 *node;
    s32 i;
    s32 j;

    if (((u32)pA & 3) != 0 || ((u32)pB & 3) != 0) {
        printf(taco_pose_align_fmt, pA, pB);
        return -1;
    }

    if (f4 == 0) {
        pA += n * 3;
    }
    if (f5 == 0) {
        pB += n * 9;
    }

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            m.m[i][j] = *pB;
            pB++;
        }
        m.t[i] = *pA;
        pA++;
    }

    node = &taco_obj[idx];
    ((Rec *)(unsigned long)node->f4)->mat = m;
    ((Rec *)(unsigned long)node->f4)->flag = 0;
}
