/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Address-named LIBGTE entries that matched code calls, bound to the HAL's
 * implementation of the same Sony function.
 *
 * GsSortSprite once called ReadGeomScreen (0x800A5174) and RotTransPers4
 * (0x800A5260) by address, and this file forwarded both to pc/hal/gte.c;
 * before that the PC link took `return 0` stubs and the lesson screen's
 * scaled stat digits vanished.  Since 2026-11-10 both names are landed in
 * config/symbol_names.json and the matched C calls the HAL directly, so
 * only the overlay RotMatrix copies below remain.
 */
#include "hal.h"

/* ---- overlay copies of LIBGTE RotMatrixX/Y/Z (FGO_04/05/06) ----------
 * TACO and DATE link the assembly members verbatim (asm/overlay/<OV>/
 * RotMatrix[XYZ].s; byte-identical twins).  The overlay modules call them
 * as TACO_RotMatrixX ... (overlay-local names), which the PC link had
 * resolved to `return 0` stubs: TACO's taco_cut / taco_quad_spin /
 * taco_rec_move / taco_node_shot_spawn (10 jal sites, reached in TACO
 * gameplay) and DATE's date_angle_set got an unrotated matrix.
 * Implementation: pc/hal/gte.c gte_rot_matrix_[xyz] (from the listings). */
extern MATRIX *gte_rot_matrix_x(s32 a, MATRIX *m);
extern MATRIX *gte_rot_matrix_y(s32 a, MATRIX *m);
extern MATRIX *gte_rot_matrix_z(s32 a, MATRIX *m);

MATRIX *TACO_RotMatrixX(s32 a, MATRIX *m) { return gte_rot_matrix_x(a, m); }
MATRIX *TACO_RotMatrixY(s32 a, MATRIX *m) { return gte_rot_matrix_y(a, m); }
MATRIX *TACO_RotMatrixZ(s32 a, MATRIX *m) { return gte_rot_matrix_z(a, m); }
MATRIX *DATE_RotMatrixX(s32 a, MATRIX *m) { return gte_rot_matrix_x(a, m); }
MATRIX *DATE_RotMatrixY(s32 a, MATRIX *m) { return gte_rot_matrix_y(a, m); }
MATRIX *DATE_RotMatrixZ(s32 a, MATRIX *m) { return gte_rot_matrix_z(a, m); }

/* ---- address-named lit TMD builders the TmdG3/G4/TF4 dispatchers call -
 * Identified by their byte-perfect retail listings (asm/main/matched/):
 *   0x800A7218 GsTMDfastG3L    (TmdG3, GsLIOFF = 0 arm)
 *   0x800A8460 GsTMDfastG4NL   (TmdG4, GsLIOFF = 1)
 *   0x800A85D4 GsTMDfastG4L    (TmdG4)
 *   0x800A8B40 GsTMDfastTF4NL  (TmdTF4, GsLIOFF = 1)
 *   0x800A8CDC GsTMDfastTF4L   (TmdTF4)
 * None is reached: the builder census (pc/tools/tmd_census.py, all 63
 * scenes + scripted TACO/DATE/TAIIKU) saw no G3/G4/TF4 lit primitive, and
 * no TMD on the disc has a lit G4/TF4 primitive.  They were `return 0`
 * stubs, which would have zeroed GsOUT_PACKET_P (the 2026-10-29 NULL
 * cursor class) had a model reached them; like the other unreached lit
 * builders (pc/src/missing_funcs.c) they now pass the packet cursor (the
 * 4th argument: prim, vtx, normals, packet, count, shift, ot) through and
 * draw nothing. */
#define TMD_LIT_PASS(name)                                                  \
    s32 name(void *prim, void *vtx, void *normal, s32 packet, s32 count,     \
             s32 shift, s32 ot)                                             \
    {                                                                       \
        (void)prim; (void)vtx; (void)normal; (void)count; (void)shift;       \
        (void)ot;                                                           \
        return packet;                                                      \
    }

TMD_LIT_PASS(func_800A7218)
TMD_LIT_PASS(func_800A8460)
TMD_LIT_PASS(func_800A85D4)
TMD_LIT_PASS(func_800A8B40)
TMD_LIT_PASS(func_800A8CDC)

#undef TMD_LIT_PASS
