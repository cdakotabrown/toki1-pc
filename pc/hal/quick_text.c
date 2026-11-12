/* SPDX-License-Identifier: GPL-3.0-or-later */
/* quick_text.c -- "press to finish the line" ([enhanced] quick_text,
 * TOKI_QUICK_TEXT; enhanced only, off under parity).  Player keys:
 * docs/pc-port/PLAYER-GUIDE.md.
 *
 * What the console does (measured on the port under parity, Shiori's
 * phone call and the date of date_save.txt, voice_level 2 -- "cut with
 * the cross button", the game's default):
 *   serifu_main (events, dates):
 *     (a) page printing: circle does nothing; while circle or cross is
 *         HELD k_disp_wait (via common_disp) prints one glyph a frame.
 *         Cross cuts the voice (device_cmd_send(180), serifu_state 2)
 *         and the page advances by itself the moment it is printed.
 *     (b) page printed, voice playing: circle does nothing; cross cuts
 *         the voice and the page advances once the drive leaves state 4.
 *     (c) voice over, arrow up: circle advances.
 *   xa_wait_m / xa_wait (the TEL overlay, now_step 0x45):
 *     (a) circle or cross: k_disp_goto_line_end -- it moves the print
 *         counter to the end of the string record it is in, which on
 *         Shiori's greeting changes nothing (the counter sits on a record
 *         boundary); the page prints on at its speed.
 *     (b) circle does nothing; cross (voice_level 2) pauses the drive
 *         (device_cmd_send(0x74), CdlPause) and advances in the same frame.
 *     (c) circle advances.
 *   voice_level 1 ("always on") has no cut at all; 0 has no voice.
 *
 * What this adds, for a CONFIRM press (circle on a pad or the keyboard;
 * the mouse button the game reads as circle, mouse_btn_mode swaps them)
 * by the PLAYER, decided on the press's first poll:
 *   1. the page is printing (check_end_k_tbl1 < check_end_k_tbl0) in a
 *      dialogue: the press never reaches the game (it is held back until
 *      released, so the game sees no trigger and no hold), and at the end
 *      of that frame (hal_frame_end, a frame boundary: no game code runs
 *      between it and the next frame) check_end_k_tbl1 -- the number of
 *      glyphs shown -- is set to the page's end.  That is the one
 *      game-state write.  It is exactly what k_disp_inc does one glyph at
 *      a time (its only effect is check_end_k_tbl1 + 1): the glyphs were
 *      all put in VRAM by load_string / vload_font when the page was set
 *      up, there is no per-glyph sound, and the engine's own logic (the
 *      arrow, the voice wait, the advance, the scroll) reads the counter
 *      as usual from the next frame on.
 *   2. the page is printed, its voice line is still playing, voice_level
 *      is 2: the press reaches the game as CROSS -- the game's own cut,
 *      (b) above.  Input only.
 *   Anything else (voice over, a menu, voice_level 0/1, no dialogue)
 *   passes through untouched.
 *
 * Dialogue: serifu_main's page (serifu_state, msg_wait or serifu_wait set)
 * or, on the TEL screen (now_step 0x45), a page with the speech layout
 * (a name string, then the opening bracket at x -86..-72 on its line --
 * the backlog's find_speaker).  Choice menus, name entry, minigames and
 * every other screen are never touched.  The TEL engines scroll a page of
 * more than three lines (k_mode bit 0x80; k_no the top line -- the same
 * bytes xa_wait_m reads as now_girl.anim / .top): there the counter goes
 * only to the end of the third visible line, and the engine scrolls and
 * prints the rest itself (another press finishes the next window).
 *
 * Machine state: the statics below are restored with the machine by a
 * quickload / rewind (pc/hal/dbg_state.c keeps only the files it lists).
 */
#include "types.h"
#include "profile.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>

extern s16 check_end_k_tbl0;        /* glyphs loaded (the page's end) */
extern s16 check_end_k_tbl1;        /* glyphs shown */
extern s16 disp_string_state;              /* string records in disp_string_tbl0 */
extern s32 serifu_state, msg_wait, serifu_wait, serifu_voice, xa_m_mode;
extern u8  voice_level, cd_state, k_no, k_mode, mouse_btn_mode;
extern u8  now_step, now_sub_step, now_sub_sub_step, step_mode;

typedef struct {
    s16 x, y;
    u8 tp, count, pal;
    s8 flag;
} QtRec;                            /* disp_string's record (backlog_capture.c TextRec) */
extern QtRec disp_string_tbl0[];

/* pc/hal/pad_device.c; weak: harnesses link this file alone */
extern int pad_device_mouse_held(int right) __attribute__((weak));
extern void pad_device_mouse_filter(int hide, int add) __attribute__((weak));
extern const PcSettings *pc_settings_get(void) __attribute__((weak));

#define CONFIRM  PC_PAD_CIRCLE
#define CANCEL   PC_PAD_CROSS
#define RECORDS  52                 /* disp_string_tbl0 */
#define TEL_STEP 0x45

enum { QT_PASS, QT_HOLD, QT_CUT };

static int g_down;                  /* the confirm input was down at the last poll */
static int g_mode;                  /* what the current press does */
static int g_pending = -1;          /* the counter value to set at the frame end */
static s16 g_pending_loaded;        /* ... while the page is still this one */
static unsigned g_completed, g_cuts;

unsigned quick_text_completed(void) { return g_completed; }
unsigned quick_text_cuts(void) { return g_cuts; }

/* TOKI_QUICK_TEXT in the environment decides (tests); otherwise the
 * enhanced profile's switch and the player's [enhanced] quick_text, never
 * in an automated run. */
int quick_text_enabled(void)
{
    static int env = -2, sw = -1;
    const PcSettings *s;
    if (env == -2) {
        const char *e = getenv("TOKI_QUICK_TEXT");
        env = e && *e ? *e != '0' : -1;
    }
    if (env >= 0) return env;
    if (sw < 0) sw = hal_feature_on("TOKI_QUICK_TEXT");
    if (!sw || !pc_settings_get) return 0;
    s = pc_settings_get();
    return !s->harness && s->quick_text;
}

static int records(void)
{
    int n = disp_string_state;
    return n < 0 ? 0 : n > RECORDS ? RECORDS : n;
}

/* a name string, then the opening bracket on its line (find_speaker) */
static int speech_layout(void)
{
    int r, n = records();
    for (r = 0; r + 1 < n; r++) {
        const QtRec *a = &disp_string_tbl0[r], *b = &disp_string_tbl0[r + 1];
        if (b->x >= -86 && b->x <= -72 && b->y == a->y && a->x < -86 && a->count > 0)
            return 1;
    }
    return 0;
}

static int serifu_page(void) { return serifu_state || msg_wait || serifu_wait; }
static int tel_page(void) { return now_step == TEL_STEP && speech_layout(); }

/* Where the counter may go: the page's end, or on a scrolling TEL page the
 * end of its third visible line (records k_no .. k_no + 2). */
static int target(int tel)
{
    int end = check_end_k_tbl0, r, n, sum = 0;
    if (!tel || !(k_mode & 0x80)) return end;
    n = records();
    for (r = 0; r < n && r <= k_no + 2; r++) sum += disp_string_tbl0[r].count;
    return sum < end ? sum : end;
}

/* The voice line of the page on screen still plays (the condition under
 * which the game ignores circle and takes cross as the cut). */
static int voice_playing(int tel)
{
    if (tel) return xa_m_mode != 2;
    return serifu_voice == 1 && cd_state == 4;
}

static int decide(void)
{
    int serifu = serifu_page(), tel = !serifu && tel_page(), t;
    if (!serifu && !tel) return QT_PASS;
    if (check_end_k_tbl1 < check_end_k_tbl0) {
        t = target(tel);
        if (t <= check_end_k_tbl1) return QT_PASS;
        g_pending = t;
        g_pending_loaded = check_end_k_tbl0;
        g_completed++;
        printf("[QUICK] page completed: %d -> %d of %d glyphs (step %02x/%02x/%02x/%02x)\n",
               check_end_k_tbl1, t, check_end_k_tbl0, now_step, now_sub_step, now_sub_sub_step,
               step_mode);
        fflush(stdout);
        return QT_HOLD;
    }
    if (check_end_k_tbl1 == check_end_k_tbl0 && voice_level == 2 && voice_playing(tel)) {
        g_cuts++;
        printf("[QUICK] voice cut: confirm sent as cross (step %02x/%02x/%02x/%02x)\n",
               now_step, now_sub_step, now_sub_sub_step, step_mode);
        fflush(stdout);
        return QT_CUT;
    }
    return QT_PASS;
}

/* One pad poll (pc/hal/pad.c, the VBlank poll), on the PLAYER's buttons
 * before any assist adds its own.  -> the buttons the game is to see; the
 * mouse's confirm button is filtered the same way in pad_device.c. */
u32 quick_text_buttons(u32 player)
{
    int right, mouse = 0, down;
    if (!quick_text_enabled()) return player;
    right = mouse_btn_mode != 0;            /* the mouse button read as circle */
    if (pad_device_mouse_held) mouse = pad_device_mouse_held(right);
    down = (player & CONFIRM) || mouse;
    if (!down) {
        g_mode = QT_PASS;
    } else if (!g_down) {
        g_mode = decide();
    }
    g_down = down;
    if (pad_device_mouse_filter) {
        int confirm = right ? 2 : 1, other = right ? 1 : 2;
        if (g_mode == QT_HOLD) pad_device_mouse_filter(confirm, 0);
        else if (g_mode == QT_CUT) pad_device_mouse_filter(confirm, mouse ? other : 0);
        else pad_device_mouse_filter(0, 0);
    }
    if (g_mode == QT_HOLD) return player & ~CONFIRM;
    if (g_mode == QT_CUT && (player & CONFIRM)) return (player & ~CONFIRM) | CANCEL;
    return player;
}

/* hal_frame_end: the frame boundary.  The page completes here, unless it
 * went away since the press (a new page loaded, the counter reset). */
void quick_text_frame_end(void)
{
    if (g_pending < 0) return;
    if (check_end_k_tbl0 == g_pending_loaded && check_end_k_tbl1 < g_pending)
        check_end_k_tbl1 = (s16)g_pending;
    g_pending = -1;
}
