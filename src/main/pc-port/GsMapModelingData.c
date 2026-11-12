/* GsMapModelingData -- PC-port override of src/main/matched/GsMapModelingData.c
 * (Sony libgs; this file is NOT part of the PS1 matching build).
 *
 * Relocates a TMD: adds the table base to the vertex / normal / primitive
 * offsets of every object entry once (bit 0 of the header marks it done).
 * The matched C spells the TMD fields `long`, which is 4 bytes on the PS1 and
 * 8 on x86-64: it strode 56 bytes per 28-byte object and added a 64-bit host
 * address into 32-bit fields (scene 18's GsMapModelingData+0x40 crash, with
 * GsLinkObject4 failing on every frame after it).  The TMD is a 32-bit
 * structure: this is the matched function with `u32` fields and the base
 * written as the PS1 address of the table -- the CD-loaded window is mapped
 * at its PS1 address (pc/src/main.c), so the host address of `e` IS that
 * value, truncated.  GsLinkObject4 (pc-port) reads the entries back the
 * same way.
 *
 * Intentional divergence from the PS1: none -- pointer width only.
 */
#include "types.h"
#include <stdint.h>

void GsMapModelingData(u32 *p)
{
    u32 n, i;
    u32 *e;
    u32 base;

    if (p[0] & 1) {
        return;
    }
    p[0] |= 1;
    p++;
    n = *p;
    e = p + 1;
    base = (u32)(uintptr_t)e;
    for (i = 0; i < n; i++) {
        e[i * 7 + 0] += base;
        e[i * 7 + 2] += base;
        e[i * 7 + 4] += base;
    }
}
