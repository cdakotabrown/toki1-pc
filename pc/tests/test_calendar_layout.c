/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise both four-byte pointer levels and calendar trailer copies. */
#include <sys/mman.h>
#include <string.h>
#define cal_font_load test_cal_loader
#define cal_font_load_tbl0 test_cal_table
#define screen_w test_cal_state
#define load_image_rect test_cal_upload
#define func_800B4840 test_cal_copy
#include "../../src/main/pc-port/cal_font_load.c"
#undef screen_w

Save test_cal_state;
u32 test_cal_table;
static int uploads, bad_upload;
static s32 *first_picture;
static s32 *second_picture;

void *test_cal_copy(void *dest, const void *src, u32 n)
{
    return memcpy(dest, src, n);
}

void test_cal_upload(s32 x, s32 y, s32 w, s32 h, s32 *data)
{
    if (x != 928 || w != 32 || h != 128 ||
        y != (uploads ? 384 : 256) ||
        data != (uploads ? second_picture : first_picture)) bad_upload = 1;
    uploads++;
}

int test_calendar_layout(void)
{
    u32 *ram;
    int result;
    ram = mmap((void *)0x84000000UL, 0x8000, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram == MAP_FAILED) return __LINE__;
    test_cal_table = (u32)(unsigned long)ram;
    first_picture = (s32 *)((u8 *)ram + 0x1000);
    second_picture = (s32 *)((u8 *)ram + 0x4000);
    ram[0] = (u32)(unsigned long)first_picture;
    ram[1] = (u32)(unsigned long)second_picture;
    second_picture[2048] = 3; second_picture[2049] = 2;
    second_picture[2050] = 11; second_picture[2051] = 22;
    second_picture[2052] = 33; second_picture[2053] = 44;
    second_picture[2054] = 55; second_picture[2055] = 66;
    memset(&test_cal_state, 0xa5, sizeof(test_cal_state));
    uploads = bad_upload = 0;
    test_cal_loader(95, 4, 1);
    result = 0;
    if (uploads != 2 || bad_upload || test_cal_state.cnt != 3 ||
        test_cal_state.cnt2 != 2 || test_cal_state.a[0] != 11 ||
        test_cal_state.a[2] != 33 || test_cal_state.b[0] != 44 ||
        test_cal_state.b[2] != 66 || test_cal_state.prev[0] != 0xa5 ||
        test_cal_state.prev[63] != 0xa5) result = __LINE__;
    munmap(ram, 0x8000);
    return result;
}
