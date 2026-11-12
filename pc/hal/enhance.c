/* SPDX-License-Identifier: GPL-3.0-or-later */
/* enhance.c -- the enhanced profile's play features that live at the HAL's
 * edges: fast-forward (pc/hal/system.c presents and paces through it) and
 * dialogue auto-advance (pc/hal/pad.c adds its confirm press).
 * Switches and settings: pc/hal/profile.c; user documentation:
 * docs/pc-port/ENHANCED-MODE.md.  Off under the parity profile: every entry
 * point below then returns the neutral value without touching any state.
 *
 * Nothing here writes game memory.  Auto-advance READS globals of the
 * matched text engine and answers with a pad press, the way a player would:
 *   check_end_k_tbl0/1  the page's characters to show / shown (check_end_k:
 *                       equal = the page is complete)
 *   yazirushi_cw_disp   bit 0x80: the "next page" arrow.  serifu_main sets
 *                       it once the page AND its voice line have ended
 *                       (get_CD_State() != 4); the TEL overlay's calls set
 *                       it through yazirushi_disp_switch; result screens
 *                       (MASTER, BUNKASAI) blink it, toggling every 16
 *                       frames while they wait
 *   now_step / now_sub_step / now_sub_sub_step   the screen
 * A dialogue WAIT is a complete page with the arrow seen within ARROW_GAP
 * polls on the same screen (the gap covers the blink).  TOKI_TEXT_AUTO
 * presses confirm (one press, TEXT_PRESS_TICKS polls, after at least one
 * released poll) TOKI_TEXT_AUTO_DELAY polls into a wait, and again every
 * delay while the wait lasts; each press needs a fresh sighting of the
 * arrow.  The player's own input always wins: any held button resets it.
 *
 * Skipping read dialogue ([enhanced] skip_seen, TOKI_SKIP_SEEN): while the
 * fast-forward key is held, the same press is made at the FIRST poll of a
 * wait (no delay) when the page on screen was read before
 * (backlog_page_seen, pc/hal/backlog_capture.c + seen_text.c).  An unread
 * page gets no press, so skipping stops at new text; only the arrow wait
 * is answered, never a page still printing, its voice still playing or a
 * choice menu (no arrow).
 *
 * Skipping the intro ([enhanced] skip_intro, TOKI_SKIP_INTRO): a START tap
 * (INTRO_HOLD game frames, PC_INPUT's default hold) on the screens whose
 * own code ends on a START / circle / cross trigger (pad_trig & 0x860):
 *   now_step 16          movie_play_main -> movie_main: the boot movie
 *                        polls pad_main every movie frame and stops
 *   now_step 18, sub 1   title_move (title_move_show, title_white_out,
 *                        title_white_in): the title animation; each goes
 *                        to sub-step 2, the title menu
 * The first tap starts at the first poll on such a screen; a tap the game
 * did not take (no trigger: it was not polling yet) is repeated INTRO_GAP
 * frames after it ends.  The assist ends for the session at the title menu
 * (18/2) or at any screen that is not part of the boot (a loaded state),
 * and never presses there.  Each tap is printed ([INTRO] ...) so a script
 * can replay the same input (pc/tools/test_skip_intro.py).  Boot screens
 * that ignore input (step 0, 18/0: the card probe) are not touched.
 *
 * Text speed: the text engine prints one character every k_speed + 1
 * frames (k_disp_inc), k_speed set by the OPTION screen's message speed
 * (system_config_moji_speed: 0/1/3).  serifu_main's pages print one a
 * frame while circle or cross is held (common_disp -> k_disp_wait,
 * 0x800848C8); whole pages at once are pc/hal/quick_text.c.
 */
#include "types.h"
#include "profile.h"
#include "pc_settings.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- fast-forward */
static volatile int g_ff_active;   /* read by the audio thread */
int hal_ff_active(void) { return g_ff_active; }

static int ff_enabled(void)
{
    static int on = -1;
    if (on < 0) on = hal_feature_on("TOKI_FAST_FORWARD");
    return on;
}

/* Game frames per shown frame while fast-forward is held (0: no limit).
 * TOKI_FF_SPEED (environment, then toki.cfg) wins and is read once; else
 * the player's [enhanced] fast_forward_speed (pc/hal/settings.c), read
 * every time so the settings menu changes it live; else 4. */
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
static int ff_speed(void)
{
    static long fixed = LONG_MIN + 1;    /* not read yet */
    long s;
    if (fixed == LONG_MIN + 1) fixed = hal_setting_long("TOKI_FF_SPEED", LONG_MIN);
    if (fixed != LONG_MIN) s = fixed;
    else s = pc_settings_get ? pc_settings_get()->ff_speed : 4;
    if (s < 0) s = 0;
    if (s > 64) s = 64;
    return (int)s;
}

/* Called once per frame end by hal_frame_end.  -> the speed to run at:
 * 1 normal, N > 1 run N frames per shown frame, 0 unpaced.  The input is
 * pc/hal/pad.c's (` held, or a right-stick click; weak: harnesses link
 * this file alone); TOKI_FF_HOLD=1 holds it for headless measurement.
 * Disabled (parity), nothing is sampled. */
extern int pad_fast_forward_held(void) __attribute__((weak));
int hal_ff_frames(void)
{
    static int forced = -1;
    if (!ff_enabled()) return 1;
    if (forced < 0) forced = hal_setting_long("TOKI_FF_HOLD", 0) != 0;
    g_ff_active = forced || (pad_fast_forward_held && pad_fast_forward_held());
    return g_ff_active ? ff_speed() : 1;
}

/* ---------------------------------------------------------------- auto-advance */
extern s16 check_end_k_tbl0;
extern s16 check_end_k_tbl1;
extern u8  yazirushi_cw_disp;
extern u8  now_step;
extern u8  now_sub_step;
extern u8  now_sub_sub_step;

#define TEXT_PRESS_TICKS 2
#define ARROW_GAP        20        /* the blink's off phase is 16 frames */
#define NEVER            (1 << 30)

static int g_wait;                 /* polls into the current wait */
static int g_press;                /* polls left of an auto press */
static int g_last;                 /* the previous poll's answer */
static int g_arrow_age = NEVER;    /* polls since the arrow was seen */
static u32 g_arrow_screen;         /* the screen the arrow was seen on */
static unsigned g_presses;         /* auto presses made (tests, the debug HUD) */

unsigned text_assist_presses(void) { return g_presses; }

/* pc/hal/seen_text.c, backlog_capture.c; weak: harnesses link this file alone */
extern int seen_text_skip_enabled(void) __attribute__((weak));
extern int backlog_page_seen(void) __attribute__((weak));
static unsigned g_seen_presses;
unsigned seen_skip_presses(void) { return g_seen_presses; }

/* The skip key (the fast-forward key) is held and the page was read before. */
static int seen_skip_now(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = seen_text_skip_enabled && backlog_page_seen;
    if (!enabled || !seen_text_skip_enabled()) return 0;
    static int ff_hold = -1;       /* TOKI_FF_HOLD=1: held for headless runs */
    if (ff_hold < 0) ff_hold = hal_setting_long("TOKI_FF_HOLD", 0) != 0;
    if (!ff_hold && !(pad_fast_forward_held && pad_fast_forward_held())) return 0;
    return backlog_page_seen();
}

/* One pad poll.  `player`: the buttons the player (keyboard, controller,
 * script) holds now.  -> 1: add a confirm press this poll. */
int text_assist_confirm(u32 player)
{
    static int enabled = -1, skip_on = -1;
    static long delay;
    u32 screen;
    int waiting, out = 0, skip = 0;
    if (enabled < 0) {
        enabled = hal_feature_on("TOKI_TEXT_AUTO");
        delay = hal_setting_long("TOKI_TEXT_AUTO_DELAY", 60);
        if (delay < 1) delay = 1;
    }
    if (skip_on < 0) skip_on = seen_text_skip_enabled != NULL;
    if (!enabled && !(skip_on && seen_text_skip_enabled())) return 0;
    screen = (u32)now_step << 16 | (u32)now_sub_step << 8 | now_sub_sub_step;
    if (g_arrow_age < NEVER) g_arrow_age++;
    if (yazirushi_cw_disp & 0x80) {
        g_arrow_age = 0;
        g_arrow_screen = screen;
    } else if (screen != g_arrow_screen) {
        g_arrow_age = NEVER;
    }
    waiting = check_end_k_tbl0 == check_end_k_tbl1 && g_arrow_age <= ARROW_GAP;
    if (waiting && skip_on) skip = seen_skip_now();
    if (player) {
        g_wait = 0;
        g_press = 0;
        g_arrow_age = NEVER;
    } else if (g_press > 0) {
        g_press--;
        out = 1;
    } else if (waiting && (enabled || skip)) {
        if ((skip || ++g_wait >= delay) && !g_last) {
            g_wait = 0;
            g_press = TEXT_PRESS_TICKS - 1;
            g_arrow_age = NEVER;          /* the next press needs a fresh arrow */
            g_presses++;
            if (skip) {
                g_seen_presses++;
                printf("[SEEN] confirm on a read page (%u)\n", g_seen_presses);
                fflush(stdout);
            }
            out = 1;
        }
    } else {
        g_wait = 0;
    }
    g_last = out;
    return out;
}

/* After a restore (pc/hal/dbg_state.c dbg_state_after_restore): the wait
 * counted, the press in flight and the arrow seen belong to the moment
 * left behind (these statics are the session's, kept out of a state), so
 * a loaded page waits its own full delay for its own arrow. */
void text_assist_after_load(void)
{
    g_wait = 0;
    g_press = 0;
    g_last = 0;
    g_arrow_age = NEVER;
}

/* ---------------------------------------------------------------- skip intro */
extern s32 frame_counter;
extern const PcSettings *pc_settings_get(void) __attribute__((weak));

#define INTRO_HOLD 2               /* game frames a tap holds START */
#define INTRO_GAP  4               /* game frames between taps the game missed */

static int g_intro = -1;           /* -1 not decided, 1 on, 0 off / done */
static long g_tap_from = -1;       /* frame_counter the current tap started at */
static long g_tap_next;            /* the next tap may start here */
static unsigned g_taps;

unsigned intro_assist_taps(void) { return g_taps; }

/* The environment's TOKI_SKIP_INTRO, when set, decides (tests); otherwise
 * the enhanced profile's switch and [enhanced] skip_intro, never in an
 * automated run. */
static int intro_enabled(void)
{
    const char *e = getenv("TOKI_SKIP_INTRO");
    const PcSettings *s;
    if (e && *e) return *e != '0';
    if (!hal_feature_on("TOKI_SKIP_INTRO") || !pc_settings_get) return 0;
    s = pc_settings_get();
    return !s->harness && s->skip_intro;
}

/* screens whose own code skips on a START trigger */
static int intro_screen(void)
{
    return now_step == 16 || (now_step == 18 && now_sub_step == 1);
}

/* still booting: power-on, the movie, the boot step, the title before its menu */
static int booting(void)
{
    return now_step == 255 || now_step == 16 || now_step == 0 ||
           (now_step == 18 && now_sub_step < 2);
}

/* One pad poll (pc/hal/pad.c).  -> the buttons to add: START during a tap. */
u32 intro_assist_buttons(u32 player)
{
    long f = frame_counter;
    if (g_intro < 0) g_intro = intro_enabled();
    if (!g_intro) return 0;
    if (g_tap_from >= 0 && f >= g_tap_from && f < g_tap_from + INTRO_HOLD)
        return player ? 0 : PC_PAD_START;           /* a tap runs its length */
    if (g_tap_from >= 0) {                          /* a tap just ended */
        g_tap_from = -1;
        g_tap_next = f + INTRO_GAP;
    }
    if (!booting()) {
        g_intro = 0;                                /* the title menu, or a loaded game */
        printf("[INTRO] done at game frame %ld (step %u/%u), %u tap(s)\n", f, now_step,
               now_sub_step, g_taps);
        fflush(stdout);
        return 0;
    }
    if (player || !intro_screen() || f < g_tap_next) return 0;
    g_tap_from = f;
    g_taps++;
    printf("[INTRO] START at game frame %ld (step %u/%u/%u), held %d frames\n", f, now_step,
           now_sub_step, now_sub_sub_step, INTRO_HOLD);
    fflush(stdout);
    return PC_PAD_START;
}
