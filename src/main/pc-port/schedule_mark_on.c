/*
 * schedule_mark_on -- PC override of
 * src/main/matched/schedule_mark_on.c (0x80068030).
 *
 * Statement for statement the matched function; the divergence is the
 * RECORD WIDTH (the src/main/pc-port/bg_set.c / mv_state.h doctrine, same
 * defect as src/main/pc-port/basyo_init.c).  The matched Chr68
 * declares the class_w slot's three data handles (p12, p16, p52) as
 * native pointers: 4 bytes on the console, 8 bytes (8-aligned) on LP64.
 * There the 68-byte slot becomes an 88-byte record, so `class_w[n]`
 * strides 88 bytes, every field after +12 moves, and the writes meant
 * for slots 97..128 (the 32 calendar mark sprites)
 * landed across the wrong slots with their handles at host offsets
 * +16/+24/+56.  Measured by an LP64-vs-PS1 slot oracle: the matched code
 * leaves ~1,500 bytes of class_w different from the console image; this
 * override leaves none.
 *
 * Here every slot access goes through the PS1-width ClassSlot
 * (pc/include/game/class_slot.h), fields named by PS1 offset:
 *   b0..b7 -> f0..f7, p12 -> f12, p16 -> f16, h20 -> f20,
 *   type -> f24, x -> f38, y -> f42, p52 -> f52, w56 -> f56.
 * The handle stores zero-extend the .set-aliased PS1 addresses
 * (pc/src/ps1_addr_aliases.c), the value the console stores.  The body
 * below is the matched body with exactly those renames (generated, then
 * diffed against the matched file: no other line differs).
 *
 * @DivergesFrom: matched schedule_mark_on -- record field widths only.
 */
#include "types.h"
#include "class_slot.h"

typedef struct {
    unsigned month : 4;
    unsigned day : 5;
    unsigned : 23;
} CalDate;

typedef struct {
    unsigned : 6;
    unsigned on : 1;
    unsigned : 25;
    CalDate date;
    u8 pad[48];
} CalBirth;

typedef struct {
    unsigned : 12;
    unsigned month : 4;
    unsigned day : 5;
    unsigned : 8;
    unsigned on : 1;
    unsigned : 2;
} CalHoliday;

typedef struct {
    u8 count;
    u8 pad01[31];
    u8 wday;
    u8 sched[31];
} CalSched;

typedef struct {
    u8 year;
    u8 month;
    u8 day;
    u8 pad[5];
} CalEvent;

typedef struct {
    u8  pad000[0x3E];
    u8  year;                   /* +0x03E */
    u8  month;                  /* +0x03F */
    u8  pad040[4];
    CalSched sched[2];          /* +0x044 */
    u8  pad0C4[0x30];
    struct {
        unsigned : 12;
        unsigned month : 4;
        unsigned : 16;
    } now;                      /* +0x0F4 */
    CalDate today;              /* +0x0F8 */
    u8  pad0FC[0x1C8 - 0xFC];
    CalBirth birth[11];         /* +0x1C8 */
    u8  pad430[0x54C - 0x430];
    CalHoliday girl_menu_select[8];      /* +0x54C */
    u8  pad56C[0x762 - 0x56C];
    CalEvent event[256];
} CalBlock;

extern CalBlock screen_w;
extern ClassSlot class_w[];
extern u8 schedule_mark_on_tbl0[];
extern s32 basyo_init_tbl1;
extern s32 basyo_init_tbl2;
extern s32 _sys_default_tpage_set_tbl2;
extern s16 basyo_init_tbl3;

extern void class_clear_n(s32);
extern s16 get_calx_pos(s32, s32);
extern s16 get_caly_pos(s32, s32);

#define ADD_MARK(t, who, day) \
    class_w[cnt + 97].f24 = (t); \
    class_w[cnt + 97].f38 = get_calx_pos((who), (day)); \
    class_w[cnt + 97].f42 = get_caly_pos((who), (day)); \
    cnt++

void schedule_mark_on(void)
{
    CalBlock *cal;
    s32 i;
    s32 cnt;
    s32 rep;
    s32 mark;
    s32 m;
    s32 d;
    u32 j;
    s32 v;
    s32 k;

    cal = &screen_w;
    for (i = 0; i < 32; i++) {
        class_clear_n(i + 97);
        class_w[i + 97].f1 = 3;
        class_w[i + 97].f2 = 64;
        class_w[i + 97].f56 = 0;
        class_w[i + 97].f3 = 140;
        class_w[i + 97].f12 = CLASS_SLOT_HANDLE(&basyo_init_tbl1);
        class_w[i + 97].f16 = CLASS_SLOT_HANDLE(&basyo_init_tbl2);
        class_w[i + 97].f52 = CLASS_SLOT_HANDLE(&_sys_default_tpage_set_tbl2);
        class_w[i + 97].f20 = basyo_init_tbl3;
        class_w[i + 97].f4 = 8;
        class_w[i + 97].f5 = 1;
        class_w[i + 97].f6 = 8;
        class_w[i + 97].f24 = 98;
        class_w[i + 97].f38 = 256;
        class_w[i + 97].f42 = 256;
        class_w[i + 97].f7 = 128;
    }

    m = screen_w.today.month;
    d = screen_w.today.day;
    rep = 0;
    cnt = 0;
    if (m == screen_w.month) {
        i = d - 1;
        ADD_MARK(110, 0, i);
    }
    if (m == screen_w.month + 1) {
        i = d - 1;
        ADD_MARK(110, 1, i);
    }

    for (j = 0; j < 11; j++) {
        m = screen_w.birth[j].date.month;
        d = screen_w.birth[j].date.day;
        if (screen_w.birth[j].on) {
            if (m == screen_w.month) {
                i = d - 1;
                ADD_MARK(109, 0, i);
            }
            v = screen_w.month + 1;
            if (m == v || m + 12 == v) {
                i = d - 1;
                ADD_MARK(109, 1, i);
            }
        }
    }

    for (j = 0; j < 8; j++) {
        m = screen_w.girl_menu_select[j].month;
        d = screen_w.girl_menu_select[j].day;
        if (screen_w.girl_menu_select[j].on) {
            if (m == screen_w.month) {
                i = d - 1;
                ADD_MARK(98, 0, i);
            }
            if (m == screen_w.month % 12 + 1) {
                i = d - 1;
                ADD_MARK(98, 1, i);
            }
        }
    }

    if (screen_w.now.month >= 6 && screen_w.now.month < 11) {
        v = 21 - screen_w.sched[0].wday;
        m = 21 - screen_w.sched[1].wday;
        i = v - 1;
        ADD_MARK(111, 0, i);
        if (screen_w.year != 98 || screen_w.month != 2) {
            i = m - 1;
            ADD_MARK(111, 1, i);
        }
    }

    j = 0;
    if (screen_w.sched[0].count != 0) {
        do {
            if (screen_w.sched[0].sched[j] != 0 &&
                schedule_mark_on_tbl0[screen_w.sched[0].sched[j]] != 255) {
                if (schedule_mark_on_tbl0[screen_w.sched[0].sched[j]] == 111) {
                    if (screen_w.month == 8 && screen_w.now.month < 11) {
                        ADD_MARK(schedule_mark_on_tbl0[screen_w.sched[0].sched[j]], 0, j);
                    }
                } else {
                    ADD_MARK(schedule_mark_on_tbl0[screen_w.sched[0].sched[j]], 0, j);
                }
                if (schedule_mark_on_tbl0[screen_w.sched[0].sched[j]] == 102 && screen_w.month != 2) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[0].sched[j]];
                    rep = 6;
                } else if (schedule_mark_on_tbl0[screen_w.sched[0].sched[j]] == 108) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[0].sched[j]];
                    rep = 6;
                } else if (schedule_mark_on_tbl0[screen_w.sched[0].sched[j]] == 103) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[0].sched[j]];
                    rep = 7;
                } else if (schedule_mark_on_tbl0[screen_w.sched[0].sched[j]] == 111 &&
                           screen_w.month == 8 && screen_w.now.month < 11) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[0].sched[j]];
                    rep = 6;
                }
            } else if (rep > 0) {
                ADD_MARK(mark, 0, j);
            }
            rep--;
            if (cnt + 97 >= 160) {
                return;
            }
            j++;
        } while (j < cal->sched[0].count);
        rep = 0;
        j = 0;
    }

    j = 0;
    if (screen_w.sched[1].count != 0) {
        do {
            if (screen_w.sched[1].sched[j] != 0 &&
                schedule_mark_on_tbl0[screen_w.sched[1].sched[j]] != 255) {
                if (schedule_mark_on_tbl0[screen_w.sched[1].sched[j]] == 111) {
                    if (screen_w.month == 7 && screen_w.now.month < 11) {
                        ADD_MARK(schedule_mark_on_tbl0[screen_w.sched[1].sched[j]], 1, j);
                    }
                } else {
                    ADD_MARK(schedule_mark_on_tbl0[screen_w.sched[1].sched[j]], 1, j);
                }
                if (schedule_mark_on_tbl0[screen_w.sched[1].sched[j]] == 102 && screen_w.month != 1) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[1].sched[j]];
                    rep = 6;
                } else if (schedule_mark_on_tbl0[screen_w.sched[1].sched[j]] == 108) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[1].sched[j]];
                    rep = 6;
                } else if (schedule_mark_on_tbl0[screen_w.sched[1].sched[j]] == 103) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[1].sched[j]];
                    rep = 7;
                } else if (schedule_mark_on_tbl0[screen_w.sched[1].sched[j]] == 111 &&
                           screen_w.month == 7 && screen_w.now.month < 11) {
                    mark = schedule_mark_on_tbl0[screen_w.sched[1].sched[j]];
                    rep = 6;
                }
            } else if (rep > 0) {
                ADD_MARK(mark, 1, j);
            }
            rep--;
            if (cnt + 97 >= 160) {
                return;
            }
            j++;
        } while (j < cal->sched[1].count);
    }

    for (k = 0; k < 256; k++) {
        v = screen_w.event[k].year;
        m = screen_w.event[k].month;
        d = screen_w.event[k].day;
        if (v == screen_w.year && m == screen_w.month) {
            i = d - 1;
            ADD_MARK(98, 0, i);
        }
        if (cnt + 97 >= 160) {
            return;
        }
        if (screen_w.event[k].year == 0) {
            return;
        }
    }
}
