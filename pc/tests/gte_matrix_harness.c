/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Matrix contracts derived from the retail LIBGTE instruction listings. */
#include "hal.h"
#include <assert.h>
#include <string.h>

extern void GsMulCoord2(s32 *, s32 *);

static MATRIX parent = {
    {{0, -4096, 0}, {4096, 0, 0}, {0, 0, 4096}}, {100, 200, 300}
};
static MATRIX child = {
    {{4096, 0, 0}, {0, 8192, 0}, {0, 0, 12288}}, {10, 20, 30}
};
static s16 product[3][3] = {
    {0, -8192, 0}, {4096, 0, 0}, {0, 0, 12288}
};

static void check_product(MATRIX *m)
{
    assert(!memcmp(m->m, product, sizeof(product)));
}

int main(int argc, char **argv)
{
    MATRIX a, b, out, loaded;
    int i, j;

    assert(argc == 2);
    InitGeom();
    a = parent;
    b = child;
    memset(&out, 0x5a, sizeof(out));
    /* A stale loaded rotation must not participate in MulMatrix*. */
    SetRotMatrix(&child);
    SetTransMatrix(&child);
    if (!strcmp(argv[1], "first")) {
        assert(MulMatrix(&a, &b) == &a);
        check_product(&a);
        assert(!memcmp(a.t, parent.t, sizeof(a.t)));
        assert(!memcmp(&b, &child, sizeof(b)));
    } else if (!strcmp(argv[1], "second")) {
        assert(MulMatrix2(&a, &b) == &b);
        check_product(&b);
        assert(!memcmp(b.t, child.t, sizeof(b.t)));
        assert(!memcmp(&a, &parent, sizeof(a)));
    } else if (!strcmp(argv[1], "separate")) {
        assert(MulMatrix0(&a, &b, &out) == &out);
        check_product(&out);
        for (i = 0; i < 3; i++) assert(out.t[i] == 0x5a5a5a5a);
        assert(!memcmp(&a, &parent, sizeof(a)));
        assert(!memcmp(&b, &child, sizeof(b)));
    } else if (!strcmp(argv[1], "self")) {
        assert(MulMatrix2(&a, &a) == &a);
        assert(a.m[0][0] == -4096 && a.m[1][1] == -4096);
        assert(a.m[2][2] == 4096);
        assert(!memcmp(a.t, parent.t, sizeof(a.t)));
    } else if (!strcmp(argv[1], "round")) {
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        for (i = 0; i < 3; i++) {
            a.m[0][i] = 1;
            b.m[i][0] = 2048;
            b.m[i][1] = -2048;
        }
        MulMatrix0(&a, &b, &out);
        assert(out.m[0][0] == 1 && out.m[0][1] == -2);
        return 0;
    } else if (!strcmp(argv[1], "saturate")) {
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                a.m[i][j] = 32767;
                b.m[i][j] = j == 0 ? -32768 : 32767;
            }
        }
        MulMatrix0(&a, &b, &out);
        for (i = 0; i < 3; i++) {
            assert(out.m[i][0] == -32768);
            assert(out.m[i][1] == 32767 && out.m[i][2] == 32767);
        }
        return 0;
    } else if (!strcmp(argv[1], "pad")) {
        /* `swc2 $11, 16(dst)`: the pad after m[2][2] is IR3's sign. */
        u16 pad;
        memset(&out, 0x5a, sizeof(out));
        a = parent;
        a.m[2][2] = -4096;
        b = child;
        MulMatrix0(&a, &b, &out);
        memcpy(&pad, (u8 *)out.m + 18, 2);
        assert(out.m[2][2] == -12288 && pad == 0xFFFF);
        for (i = 0; i < 3; i++) assert(out.t[i] == 0x5a5a5a5a);
        memset((u8 *)b.m + 18, 0x5a, 2);
        MulMatrix2(&parent, &b);
        memcpy(&pad, (u8 *)b.m + 18, 2);
        assert(b.m[2][2] == 12288 && pad == 0);
        memset((u8 *)a.m + 18, 0x5a, 2);
        MulMatrix(&a, &child);
        memcpy(&pad, (u8 *)a.m + 18, 2);
        assert(a.m[2][2] == -12288 && pad == 0xFFFF);
        return 0;
    } else if (!strcmp(argv[1], "coords")) {
        /* Run the unchanged game wrapper, then compose a second child.
         * Reusing the view must never accumulate the previous product. */
        GsMulCoord2((s32 *)&a, (s32 *)&b);
        check_product(&b);
        assert(b.t[0] == 80 && b.t[1] == 210 && b.t[2] == 330);
        assert(!memcmp(&a, &parent, sizeof(a)));
        b = child;
        GsMulCoord2((s32 *)&a, (s32 *)&b);
        check_product(&b);
        assert(b.t[0] == 80 && b.t[1] == 210 && b.t[2] == 330);
    } else {
        assert(!strcmp(argv[1], "composite"));
        assert(CompMatrix(&a, &b, &a) == &a);
        check_product(&a);
        assert(a.t[0] == 80 && a.t[1] == 210 && a.t[2] == 330);
        return 0;
    }
    ReadRotMatrix(&loaded);
    assert(!memcmp(loaded.m, parent.m, sizeof(loaded.m)));
    assert(!memcmp(loaded.t, child.t, sizeof(loaded.t)));
    return 0;
}
