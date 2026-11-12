/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port override of psyq/libgpu.h — wraps original + 64-bit OT fixes */
#ifndef PC_LIBGPU_OVERRIDE
#define PC_LIBGPU_OVERRIDE

/* Get the original PsyQ header (next in include path) */
#include_next "psyq/libgpu.h"

/* Override OT pointer macros for 64-bit */
#undef setaddr
#undef getaddr
#undef nextPrim
#undef isendprim
#undef addPrim
#undef addPrims
#undef catPrim
#undef termPrim

void pc_ot_setaddr(void *p, void *addr);
void *pc_ot_getaddr(void *p);

#define setaddr(p, _addr) pc_ot_setaddr((void*)(p), (void*)(unsigned long)(_addr))
#define getaddr(p) ((u_long)(unsigned long)pc_ot_getaddr((void*)(p)))
#define nextPrim(p) pc_ot_getaddr((void*)(p))
#define isendprim(p) ((((P_TAG*)(p))->addr) == 0xffffff)
#define addPrim(ot, p) setaddr(p, getaddr(ot)), setaddr(ot, p)
#define addPrims(ot, p0, p1) setaddr(p1, getaddr(ot)), setaddr(ot, p0)
#define catPrim(p0, p1) setaddr(p0, p1)
#define termPrim(p) setaddr(p, 0xffffff)

#endif
