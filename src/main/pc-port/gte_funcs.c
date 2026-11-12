/* gte_funcs.c — PC-port index for the 65 GTE-blocker batch
 *
 * Each of the 65 PS1-library functions listed below is a heavy user
 * of MIPS coprocessor-2 (GTE) opcodes (ctc2/cfc2/mtc2/mfc2/lwc2/swc2
 * plus c2 pipeline commands such as RTPT, NCLIP, AVSZ3). None of
 * those operations is expressible in portable C.
 *
 * Track A (decomp): stubs live in src/main/semi-matched/func_XXX.c
 *   with BLOCKER annotations describing the GTE pipeline that cannot
 *   be re-expressed. Those stubs are admitted as semi-matched because
 *   their behavior is understood but byte-perfect C is impossible.
 *
 * Track B (PC port): per-function wrappers live in
 *   src/main/pc-port/func_XXX.c. Each wrapper shadows the semi-matched
 *   stub via pc/Makefile's PCPORT_NAMES filter. Today the wrappers
 *   return a safe pass-through value; as pc/hal/gte.c grows real
 *   per-vertex transform primitives, the wrappers should forward to
 *   HAL calls (InitGeom / SetRotMatrix / RotTransPers / NormalClip
 *   already exist; we need RotAverage3 / equivalent).
 *
 * This file itself holds no code — it is documentation plus a build
 * anchor so that `make` records the batch in dep output.
 *
 * Batch inputs:
 *   /tmp/blocker_batches/gte_00.txt (30 functions)
 *   /tmp/blocker_batches/gte_01.txt (30 functions)
 *   /tmp/blocker_batches/gte_02.txt ( 5 functions)
 */
static const char gte_funcs_batch_tag[] = "gte-2trk-65";
