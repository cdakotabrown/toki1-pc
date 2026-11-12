/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Keep object initialization stores at +0/+4/+8 in 16-byte records. */
#include <sys/mman.h>
#include <string.h>
#include "types.h"

u32 object[64];
u8 coord_buf[16 * 80];
static int links, maps;

void GsMapModelingData(u32 *p) { (void)p; maps++; }
void GsLinkObject4(u32 *table, u32 *obj, s32 index)
{
    (void)table;
    obj[2] = 0xa0000000u + (u32)index;
    links++;
}
s32 func_800B4870(void) { return 1; }

extern void title_init_ModelingData(void);
extern void initModelingData_init(s32 *);
extern s32 model_slot_set(s32 *, s32, u32);

int test_model_layout(void)
{
    u32 *ram;
    s32 data[32];
    int i, result;
    ram = mmap((void *)0x801f0000UL, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram == MAP_FAILED) return __LINE__;
    result = 0;
    ram[2] = 13;
    memset(object, 0x5a, sizeof(object));
    links = maps = 0;
    title_init_ModelingData();
    if (maps != 1 || links != 29) { result = __LINE__; goto done; }
    for (i = 0; i < 16; i++) {
        if (object[i * 4] != 0 ||
            object[i * 4 + 1] != (u32)(unsigned long)(coord_buf + i * 80) ||
            object[i * 4 + 2] != 0xa0000000u + (i < 13 ? (i + 1) % 13 : 0) ||
            object[i * 4 + 3] != 0x5a5a5a5a) {
            result = __LINE__; goto done;
        }
    }
    memset(data, 0, sizeof(data));
    initModelingData_init(data);
    for (i = 0; i < 16; i++) {
        if (object[i * 4] != 0x80000000 ||
            object[i * 4 + 1] != (u32)(unsigned long)(coord_buf + i * 80) ||
            object[i * 4 + 2] != 0xa0000000u) {
            result = __LINE__; goto done;
        }
    }
    data[2] = 13;
    if (model_slot_set(data, 4, 3) != 1 || object[18] != 0xa0000003u ||
        object[17] != (u32)(unsigned long)(coord_buf + 4 * 80) ||
        model_slot_set(data, 4, 13) != -1) result = __LINE__;
done:
    munmap(ram, 4096);
    return result;
}
