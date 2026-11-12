/*
 * GsTMDfastTG3NL — PC-port shadow (GTE-bound inner sort loop)
 * @DivergesFrom: semi-matched stub is also empty; this shadow returns
 *   the packet pointer (a3) unchanged so callers that consume the
 *   return value (v0) don't corrupt their OT append cursor. The real
 *   function runs 7 GTE ops per triangle — cannot be portably matched
 *   without a full GTE shadow. See semi-matched header for details.
 *
 * Behavior: return 0 triangles processed, packet cursor unadvanced.
 * OT bucket list stays untouched; scene just renders fewer polys via
 * this particular sort path. Any KCET code paths that depended on the
 * OT entries being populated here will silently drop triangles — this
 * is acceptable for PC-port AUTOPLAY/title flow (no GTE-sorted meshes
 * are driven through this routine in tested scenes).
 */
#include "types.h"

void *GsTMDfastTG3NL(void *a0, void *a1, void *a2, void *a3) {
    (void)a0; (void)a1; (void)a2;
    return a3;
}
