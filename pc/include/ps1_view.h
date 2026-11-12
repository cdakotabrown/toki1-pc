/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ps1_view.h -- the address of a PS1 global, as the PS1 code sees it.
 *
 * Game code reads PS1 RAM by address: a function that indexes one symbol's
 * records walks into the next symbol's bytes when the records continue
 * there, and on the console that is simply the next word of RAM.  GCC
 * instead assumes that two distinct declared objects never overlap (the
 * base-decl rule of its alias oracle, independent of -fstrict-aliasing), so
 * an access through symbol A at an offset past A's size may be reordered
 * around a store to symbol B at that very address.  Measured: TAIIKU
 * taiiku_slot adds to D_80149BA4[2] and then get_taiiku_seg (inlined) reads
 * the same halfword as D_80149BA2[3].lo; GCC hoisted the read above the
 * add, so the port saw last frame's position and drew rand() one frame
 * after the console (the ball race's path depended on the build).
 *
 * ps1_view_addr() returns its argument through an empty asm, so the
 * compiler cannot tell which object the result points into: an access
 * through it may alias any global, as every access may on the console.
 * The generated overlay modules (pc/tools/generate_overlay_modules.py)
 * spell every typed view of a global through it.
 */
#ifndef PS1_VIEW_H
#define PS1_VIEW_H

static __inline__ __attribute__((always_inline)) void *ps1_view_addr(void *p)
{
    __asm__("" : "+r"(p));
    return p;
}

/* &name for a typed view: `(*(T *)PS1_VIEW(name))` */
#define PS1_VIEW(name) ps1_view_addr((void *)&(name))

#endif
