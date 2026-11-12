/* SPDX-License-Identifier: GPL-3.0-or-later */
/* types.h — PC port version (wraps decomp types.h with RECT guard) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef PC_TYPES_H
#define PC_TYPES_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned long long u64;
typedef signed long long s64;

#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif

#ifndef RECT_DEFINED
#define RECT_DEFINED
typedef struct {
    s16 x, y, w, h;
} RECT;
#endif

#endif /* PC_TYPES_H */
