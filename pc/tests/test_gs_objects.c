/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Regression: PS1 object/TMD/OT records retain their byte layout on x64.
 * Exercise the adapters with addresses above INT32_MAX, and verify primitive
 * dispatch arguments plus OT links without requiring game assets. */
#include <stddef.h>
#include <sys/mman.h>
#include <string.h>
#include "types.h"

extern void GsSortObject(void *, s32 *, s32);
extern void GsSortObject2(void *, s32 *, s32);
extern void GsSortObject4(void *, s32, s32, s32 *);

s32 libgs_obj_attr_bits0to2, GsLMODE, GsLIGNR, GsLIOFF;
s32 libgs_obj_attr_bit7, libgs_obj_attr_bit8, GsNDIV, GsTRATE, GsTON;
s32 GsDISPON, GsLIGHT_FUNC, GsLSMATRIX;
s32 GsLIGHT_MODE, GsOUT_PACKET_P, HWD0, VWD0;
static u32 received[8];
static int draws, matrices;

void SetRotMatrix(s32 *p) { (void)p; matrices++; }
void SetTransMatrix(s32 *p) { (void)p; matrices++; }

s32 Gslight_normal(s32 a, s32 b, s32 c, s32 d)
{
    received[0] = (u32)a; received[1] = (u32)b;
    received[2] = (u32)c; received[3] = (u32)d;
    draws++;
    return 1;
}
s32 Gslight_normal_fog(s32 a, s32 b, s32 c, s32 d)
{ return Gslight_normal(a, b, c, d); }
s32 Gslight_mate(s32 a, s32 b, s32 c, s32 d)
{ return Gslight_normal(a, b, c, d); }
s32 Gslight_mate_fog(s32 a, s32 b, s32 c, s32 d)
{ return Gslight_normal(a, b, c, d); }

s32 TmdF3(u8* a0, u8* a1, u8* a2, s32 a3, u16 a4, s32 a5, s32 a6, s32 a7)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    received[7] = (u32)(unsigned long)a7;
    return a3 + 16;
}
s32 TmdTF3(u8* a0, u8* a1, u8* a2, s32 a3, u16 a4, s32 a5, s32 a6, s32 a7)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    received[7] = (u32)(unsigned long)a7;
    return a3 + 16;
}
s32 TmdF4(u8* a0, u8* a1, u8* a2, s32 a3, u16 a4, s32 a5, s32 a6, s32 a7)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    received[7] = (u32)(unsigned long)a7;
    return a3 + 16;
}
s32 TmdTF4(void) { draws++; return 0; }
s32 TmdG3(u8* a0, u8* a1, u8* a2, s32 a3, u16 a4, s32 a5, s32 a6, s32 a7)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    received[7] = (u32)(unsigned long)a7;
    return a3 + 16;
}
s32 TmdTG3(u8* a0, u8* a1, u8* a2, s32 a3, u16 a4, s32 a5, s32 a6, s32 a7)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    received[7] = (u32)(unsigned long)a7;
    return a3 + 16;
}
s32 TmdG4(void) { draws++; return 0; }
s32 TmdTG4(void) { draws++; return 0; }
s32 GsTMDfastNF3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDfastNF4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivNF3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivNF4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDfastNG3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    return a2 + 16;
}
s32 GsTMDfastTNF3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    return a2 + 16;
}
s32 GsTMDfastTNG3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    return a2 + 16;
}
s32 GsTMDfastNG4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    return a2 + 16;
}
s32 GsTMDfastTNF4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    return a2 + 16;
}
s32 GsTMDfastTNG4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    return a2 + 16;
}
s32 GsTMDdivTNF3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivNG3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivTNG3(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivNG4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivTNF4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}
s32 GsTMDdivTNG4(u8* a0, u8* a1, s32 a2, u16 a3, s32 a4, s32 a5, s32 a6)
{
    draws++;
    received[0] = (u32)(unsigned long)a0;
    received[1] = (u32)(unsigned long)a1;
    received[2] = (u32)(unsigned long)a2;
    received[3] = (u32)(unsigned long)a3;
    received[4] = (u32)(unsigned long)a4;
    received[5] = (u32)(unsigned long)a5;
    received[6] = (u32)(unsigned long)a6;
    return a2 + 16;
}

#define CHECK(c) do { if (!(c)) { result = __LINE__; goto done; } } while (0)

int test_gs_object_width(void)
{
    u32 *ram, *tmd, *prim, *ot;
    u32 object[28], small[4];
    s32 slot[3];
    int result, i;

    result = 0;
    ram = mmap((void *)0x82000000UL, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram == MAP_FAILED) return __LINE__;
    tmd = ram + 64; prim = ram + 128; ot = ram + 192;
    tmd[0] = (u32)(unsigned long)(ram + 256);
    tmd[2] = (u32)(unsigned long)(ram + 320);
    tmd[4] = (u32)(unsigned long)prim;
    tmd[5] = 1;
    slot[0] = 3; slot[1] = (s32)(unsigned long)ot; slot[2] = 2;

    /* The +0x6c and +8 object variants must read one handle, and splice
     * the packet into a 12-byte ordering-table record. */
    for (i = 0; i < 2; i++) {
        memset(object, 0, sizeof(object));
        object[i ? 2 : 27] = (u32)(unsigned long)tmd;
        prim[0] = 0x0100000a;
        ot[1] = 0x12abcdef;
        GsOUT_PACKET_P = (s32)(unsigned long)prim;
        matrices = draws = 0;
        if (i) GsSortObject2(object, slot, 3);
        else GsSortObject(object, slot, 3);
        CHECK(draws == 1 && matrices == (i ? 0 : 2));
        CHECK(received[0] == tmd[4] && received[1] == tmd[0]);
        CHECK(received[2] == tmd[2] && received[3] == 1);
        CHECK(prim[0] == 0x01abcdef);
        CHECK(ot[1] == (0x12000000 | ((u32)(unsigned long)prim & 0xffffff)));
    }

    /* +8 tmd, +16 primitive pointer and +20 count must not fuse words.
     * Also exercise the subdivided path and the hidden-object early exit. */
    memset(small, 0, sizeof(small));
    small[2] = (u32)(unsigned long)tmd;
    for (i = 0; i < 2; i++) {
        small[0] = i ? 0x200 : 0;
        prim[0] = i ? 0x21000001 : 0x20000001;
        GsOUT_PACKET_P = (s32)(unsigned long)(ram + 384);
        draws = 0;
        GsSortObject4(small, 7, 9, slot);
        CHECK(draws == 1);
        CHECK(received[0] == (u32)(unsigned long)prim);
        CHECK(received[1] == tmd[0]);
        CHECK((u32)GsOUT_PACKET_P == (u32)(unsigned long)(ram + 388));
        if (!i) {
            CHECK(received[2] == tmd[2] && received[4] == 1);
            CHECK(received[5] == 9 && received[6] == 7);
        } else {
            CHECK(received[3] == 1 && received[4] == 9);
            CHECK(received[5] == 7);
        }
    }
    small[0] = 0x80000000; small[2] = 0;
    draws = 0;
    GsSortObject4(small, 7, 9, slot);
    CHECK(draws == 0);
done:
    munmap(ram, 4096);
    return result;
}
