/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in parity capture. All functions are no-ops without PC_PARITY_DIR. */
#ifndef PC_PARITY_H
#define PC_PARITY_H
#include "types.h"
void parity_init(void);
int parity_enabled(void);
void parity_anchor(void);
void parity_vblank(void);
/* PC_SHOT_DIR/<label>.ppm of the current display (PC_INPUT `shot`). */
int parity_shot(const char *label);
/* PC_FRAME_HASH=PATH: per-frame VRAM / scanout hashes (hal_frame_end). */
void parity_frame_hash(void);
/* Clear the host-stack residue in a copy of the 2 MB PS1 RAM block
 * (PC_FRAME_HASH_RAM=game; pc/tools/ps1_residue.py is the same rule). */
void parity_mask_host_residue(unsigned char *ram);
/* SPU adapter: paused SDL device, same mixer as normal playback. */
void spu_parity_mix(s16 *samples, int frames);
#endif
