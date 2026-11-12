/* GsLinkObject4 -- PC-port override of src/main/matched/GsLinkObject4.c
 * (Sony libgs; this file is NOT part of the PS1 matching build).
 *
 * Links object `n` of a relocated TMD into a GsDOBJ2: stores the object's
 * entry address in the object's `tmd` field and counts the primitives of
 * each type run (writing the count over the first primitive of the run).
 * The matched C reads the entry's primitive pointer as `*(u8 **)(entry + 16)`
 * -- an 8-byte load from a 4-byte TMD field on x86-64 (scene 18's 600+
 * GsLinkObject4+0x40 crashes a run).  TMD fields are 32-bit PS1 addresses
 * (pc-port GsMapModelingData writes them that way); this is the matched
 * function with 32-bit loads widened to host pointers.  The GsDOBJ2 `tmd`
 * slot is a u32 handle at PS1 +8, paired with the GsSortObject4 adapter;
 * a native pointer-array store at +16 corrupts the next PS1 object.
 *
 * Intentional divergence from the PS1: none -- pointer width only.
 */
#include "types.h"
#ifdef PC_PORT
#include "sony_bios_alias.h"   /* printf -> the BIOS A(0x3F) trampoline */
#endif
#include <stdint.h>

extern s32 printf(const char *format, ...);
extern char libgs_gpu_code_unassigned_msg[];

#define PS1_PTR(a) ((u8 *)(uintptr_t)(u32)(a))

void GsLinkObject4(u8 *tmd, u32 *obj, s32 n)
{
    u8 *entry;
    u32 primCount;
    u8 *curPrim;
    u8 *groupStart;
    u8 prevType;
    u32 curType;
    u32 i;
    s16 groupCount;

    curType = 0;
    entry = tmd + n * 28;
    *(obj + 2) = (u32)(unsigned long)entry;
    primCount = *(u32 *)(entry + 20);
    curPrim = PS1_PTR(*(u32 *)(entry + 16));
    groupCount = 0;
    i = 0;
    groupStart = curPrim;
    if (primCount == 0) {
        goto done;
    }
    do {
        prevType = curType;
        curType = curPrim[3];
        if (prevType != 0) {
            if (curType != prevType) {
                *(s16 *)groupStart = groupCount;
                groupCount = 0;
                groupStart = curPrim;
            }
        }
        switch (curType & 0xFD) {
            case 0x28:
            case 0x30:
                curPrim += 20;
                break;
            case 0x20:
            case 0x21:
            case 0x29:
                curPrim += 16;
                break;
            case 0x25:
            case 0x34:
            case 0x39:
                curPrim += 28;
                break;
            case 0x2C:
            case 0x2D:
                curPrim += 32;
                break;
            case 0x3D:
                curPrim += 44;
                break;
            case 0x35:
            case 0x3C:
                curPrim += 36;
                break;
            case 0x24:
            case 0x31:
            case 0x38:
                curPrim += 24;
                break;
            default:
                printf(libgs_gpu_code_unassigned_msg, curType);
                break;
        }
        i++;
        groupCount++;
    } while (i < primCount);
done:
    *(s16 *)groupStart = groupCount;
}
