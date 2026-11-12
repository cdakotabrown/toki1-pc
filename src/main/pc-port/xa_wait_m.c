/*
 * xa_wait_m -- PC override of src/main/matched/xa_wait_m.c.
 *
 * Statement for statement the matched function; the divergence is the
 * pointer WIDTH.  xa_m_tbl / xa_m_tbl2 are 4-byte PS1 cells at 0x8012DC44 /
 * 0x8012DC48 that xa_set_m stores as s32 PS1 addresses.  The matched C
 * declares them `Pair *`, an 8-byte load on LP64: xa_m_tbl read as
 * (xa_m_tbl2 << 32) | xa_m_tbl, a non-canonical pointer, as soon as a
 * voiced line reaches XA mode 1 with an index (xa_m_idx != -1).  Here they
 * are read as u32 and widened, as the console reads them.
 *
 * @DivergesFrom: matched xa_wait_m -- pointer read width only
 * (pc/tools/audit_ps1_wide_access.py).
 */
#include <stdint.h>
#include "types.h"

typedef struct {
    s32 a;
    s32 b;
} Pair;

typedef struct {
    s16 x;
    s16 y;
} Pos;

extern s32 get_now_k();
extern s32 k_disp_inc();
extern void k_disp_switch();
extern Pos *get_k_work();
extern u32 get_CD_State();
extern s32 *get_XAsector();
extern void set_XAsector();
extern void device_cmd_send();
extern void cd_command_issue();
extern void mes_arrow_anim();
extern void yazirushi_disp_switch();
extern void k_disp_goto_line_end();

typedef struct {
    u8 prev;    /* now_girl */
    u8 max;     /* now_girl.max */
    u8 top;     /* now_girl.top */
    u8 anim;    /* now_girl.anim */
} FanList;
extern FanList now_girl;
extern u8 voice_level;
extern u8 yazirushi_cw_disp;
extern u8 step_mode;
extern s32 pad_trig;
extern s32 xa_m_mode;
extern u32 xa_m_tbl;      /* PS1 pointer (xa_set_m stores it as s32) */
extern u32 xa_m_tbl2;     /* PS1 pointer */
#define XA_M_TBL ((Pair *)(uintptr_t)xa_m_tbl)
#define XA_M_TBL2 ((Pair *)(uintptr_t)xa_m_tbl2)
extern s32 xa_m_idx;

void xa_wait_m()
{
    s32 unused0, unused1;   /* declared, never referenced: frame 64 */
    s32 ok;
    Pos *a;
    Pos *b;
    s32 *cur;
    s32 n;
    s32 x;
    s32 y;

    ok = 0;
    n = get_now_k();
    if (now_girl.anim & 0x80) {
        if (now_girl.top + 2 >= n) {
            ok = k_disp_inc();
            n = get_now_k();
            if (now_girl.top + 3 == n && now_girl.max >= now_girl.top + 3) {
                k_disp_switch(n, 0);
            }
        } else if ((now_girl.anim & 0x7F) == 0) {
            k_disp_switch(now_girl.top, 0);
            a = get_k_work(now_girl.top + 1);
            b = get_k_work(now_girl.top + 2);
            a->y -= 2;
            b->y -= 2;
            now_girl.anim++;
        } else if ((now_girl.anim & 0x7F) > 0) {
            a = get_k_work(now_girl.top + 1);
            b = get_k_work(now_girl.top + 2);
            a->y -= 2;
            b->y -= 2;
            if (a->y <= 50) {
                a->y = 50;
                b->y = 66;
                now_girl.anim--;
                now_girl.top++;
                k_disp_switch(now_girl.top + 2, 1);
            }
        }
    } else {
        if ((voice_level == 1 && xa_m_mode != 0) ||
            (voice_level == 2 && xa_m_mode != 0)) {
            ok = k_disp_inc();
        }
        if (voice_level == 0) {
            ok = k_disp_inc();
        }
        get_now_k();
    }
    if (xa_m_mode == 0 && (get_CD_State() & 4)) {
        xa_m_mode = 1;
    }
    cur = get_XAsector();
    if (xa_m_mode == 1 && (get_CD_State() & 2)) {
        if (xa_m_idx != -1) {
            x = XA_M_TBL[xa_m_idx].a;
            y = XA_M_TBL2[xa_m_idx].a;
            if (x != y && x == cur[2]) {
                set_XAsector(y, XA_M_TBL2[xa_m_idx].b, now_girl.prev);
                device_cmd_send(0x300);
                xa_m_mode = 0;
            } else {
                xa_m_mode = 2;
            }
        } else {
            xa_m_mode = 2;
        }
    }
    if (ok == 1 && xa_m_mode == 2) {
        if (yazirushi_cw_disp & 0x80) {
            mes_arrow_anim();
        } else {
            yazirushi_disp_switch(1);
        }
    }
    if (ok == 1 && voice_level == 0) {
        if (yazirushi_cw_disp & 0x80) {
            mes_arrow_anim();
        } else {
            yazirushi_disp_switch(1);
        }
    }
    if (pad_trig & 0x40) {
        if (ok == 1) {
            if (voice_level == 2) {
                xa_m_mode = 2;
                device_cmd_send(0x74);
                cd_command_issue(9, 0, 0);
            }
            now_girl.anim = 0;
        } else {
            k_disp_goto_line_end();
        }
    }
    if (pad_trig & 0x60) {
        if (ok == 1) {
            if (voice_level == 1) {
                if (xa_m_mode == 2) {
                    yazirushi_disp_switch(0);
                    step_mode++;
                }
                now_girl.anim = 0;
            } else if (voice_level == 2) {
                if (xa_m_mode == 2) {
                    yazirushi_disp_switch(0);
                    step_mode++;
                }
                now_girl.anim = 0;
            } else {
                yazirushi_disp_switch(0);
                step_mode++;
                now_girl.anim = 0;
            }
        } else {
            k_disp_goto_line_end();
        }
    }
}
