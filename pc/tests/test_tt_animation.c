/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#define tt_anim test_tt_anim
#include "../src/overlay/TT/tt_anim.c"

int test_tt_animation(void)
{
    u8 *script;
    TtAnim anim;
    u8 x, y;
    int result;
    if (sizeof(TtAnim) != 12 || offsetof(TtAnim, pattern) != 4 ||
        offsetof(TtAnim, frame) != 6 || offsetof(TtAnim, flags) != 8 ||
        offsetof(TtAnim, timer) != 9) return __LINE__;
    script = mmap((void *)0x85000000UL, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (script == MAP_FAILED) return __LINE__;
    script[0] = 2; script[1] = 0x12; script[2] = 7;
    script[3] = 254;
    memset(&anim, 0, sizeof(anim));
    anim.script_addr = (u32)(unsigned long)script;
    anim.flags = 0x80;
    test_tt_anim(&anim, &x, &y);
    result = 0;
    if (anim.frame != 0 || anim.timer != 2 || anim.pattern != 7 ||
        x != 160 || y != 32) { result = __LINE__; goto done; }
    test_tt_anim(&anim, &x, &y);
    if (anim.frame != 0 || anim.timer != 1) { result = __LINE__; goto done; }
    test_tt_anim(&anim, &x, &y);
    if (anim.frame != 0 || anim.timer != 2 || !(anim.flags & 1)) {
        result = __LINE__; goto done;
    }
    anim.flags = 0x40;
    test_tt_anim(&anim, &x, &y);
    if (anim.timer != 2) result = __LINE__;
done:
    munmap(script, 4096);
    return result;
}
