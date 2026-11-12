/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hal_gte_cop2.h -- COP2-level operations of the GTE model (pc/hal/gte.c).
 *
 * The hand-written Sony LIBGTE TMD builders (GsTMDfast*) are sequences of
 * GTE commands, not libgte C calls: they load V0..V2, issue RTPT/RTPS,
 * branch on FLAG bit 31, NCLIP, AVSZ3/AVSZ4 and store the SXY FIFO.  The
 * port's builders (src/main/pc-port/tmd_gte_builders.c) replay those
 * listings op for op through this interface, so every intermediate the
 * retail code tests is the one the GTE would have produced.
 */
#ifndef HAL_GTE_COP2_H
#define HAL_GTE_COP2_H

#include "hal.h"

void gte_cop2_ldv(s32 slot, const SVECTOR *v);  /* VXYn/VZn, n = 0..2 */
void gte_cop2_rtps(void);                       /* V0 */
void gte_cop2_rtpt(void);                       /* V0, V1, V2 */
s32 gte_cop2_nclip(void);                       /* returns MAC0 */
s32 gte_cop2_avsz3(void);                       /* returns OTZ */
s32 gte_cop2_avsz4(void);                       /* returns OTZ */
u32 gte_cop2_flag(void);                        /* FLAG, bit 31 summarised */
u32 gte_cop2_sxy(s32 i);                        /* SXY0..SXY2, 2 = newest */
s32 gte_cop2_sz(s32 i);                         /* SZ0..SZ3, 3 = newest */
s32 gte_cop2_ir0(void);
void gte_cop2_set_rgbc(u32 rgbc);               /* mtc2 RGBC (r | g<<8 | b<<16 | code<<24) */
void gte_cop2_nccs(void);                       /* NCCS sf=1 lm=1 on V0 */
void gte_cop2_ncct(void);                       /* NCCT sf=1 lm=1 on V0..V2 */
void gte_cop2_ncdt(void);                       /* NCDT sf=1 lm=1 on V0..V2 (depth cue by IR0) */
u32 gte_cop2_rgb(s32 i);                        /* RGB0..RGB2 colour FIFO, 2 = newest */
/* The model's registers in the console's order (cop2 data 0..31, control
 * 32..63; matrices packed as ctc2 takes them) -- the layout
 * pc/tools/gte_state.py finds in a retail save state, so a
 * diagnostic can describe both sides alike.  Observation only. */
void gte_dump_regs(u32 out[64]);

#endif
