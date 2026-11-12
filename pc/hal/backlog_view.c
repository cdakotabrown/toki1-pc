/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backlog_view.c -- draws the dialogue backlog over the paused game and
 * turns SDL events into its input (model: pc/hal/backlog.c).
 *
 * Drawn by the presenter (pc/hal/present.c pc_present_overlay) in output
 * pixels over the finished, scaled frame -- never into PS1 VRAM -- like the
 * settings menu, at the menu's size and unit (u output pixels per game
 * pixel).  Each page is drawn from its capture: every glyph's 12x12 bits
 * as the game's font cache held them, at the position the text engine
 * gave it, in its string's CLUT colour with the game's black shadow one
 * pixel down and right (disp_string draws the same cell twice).  The
 * frame -- the game's message window, title, counter, key help -- is the
 * settings menu's: latin12 text (pc/include/pc_ui.h), shadowed the same
 * way.
 *
 * Newest at the bottom.  Up / Down (and the wheel) move the selection one
 * page, Page Up / Page Down five, Home / End the ends; Enter replays the
 * selected page's voice; Escape, Backspace, the backlog key, a right click,
 * or the wheel down past the newest page close it.
 */
#include "types.h"
#include "pc_backlog.h"
#include "pc_present.h"
#include "pc_settings.h"
#include "pc_ui_draw.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

extern unsigned pad_menu_key_mask(int scancode) __attribute__((weak));
extern unsigned pad_menu_button_mask(int button) __attribute__((weak));

/* the settings menu's panel (pc/hal/present.c): the game's message window */
#define VIEW_W 304
#define VIEW_H 228
#define EDGE 10                      /* text inset from the panel's edge */
#define TITLE_Y 9
#define TOP 27                       /* the list below the title */
#define FOOT 33
#define GUTTER 16                    /* the voice mark left of a page */
#define PAD 3                        /* above and below a page */

static SDL_Renderer *g_ren;
static int g_top;                    /* the first page shown */

static void fill(int x, int y, int w, int h)
{
    SDL_Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    SDL_RenderFillRect(g_ren, &r);
}

static void color(Uint8 r, Uint8 g, Uint8 b, Uint8 a) { SDL_SetRenderDrawColor(g_ren, r, g, b, a); }

/* the frame's Latin text: latin12 (pc/include/pc_ui.h) */
static int text(int x, int y, int u, unsigned rgba, const char *t)
{
    return ui_draw_text(g_ren, x, y, u, rgba, UI_SHADOW, t);
}

/* The page's extent in the text engine's units. */
static void extent(const BacklogPage *p, int *x0, int *y0, int *x1, int *y1)
{
    int i;
    *x0 = *y0 = 32767;
    *x1 = *y1 = -32768;
    for (i = 0; i < p->nglyph; i++) {
        const BacklogGlyph *g = &p->g[i];
        if (g->x < *x0) *x0 = g->x;
        if (g->y < *y0) *y0 = g->y;
        if (g->x + BACKLOG_GLYPH_PX > *x1) *x1 = g->x + BACKLOG_GLYPH_PX;
        if (g->y + BACKLOG_GLYPH_PX > *y1) *y1 = g->y + BACKLOG_GLYPH_PX;
    }
    if (!p->nglyph) *x0 = *y0 = *x1 = *y1 = 0;
}

static int page_height(const BacklogPage *p)
{
    int x0, y0, x1, y1;
    extent(p, &x0, &y0, &x1, &y1);
    return (y1 - y0) + 1 + 2 * PAD;      /* +1: the shadow */
}

/* One glyph: its set pixels as horizontal runs, in the current colour. */
static void glyph_runs(const BacklogGlyph *g, int x, int y, int u)
{
    int r, c;
    for (r = 0; r < BACKLOG_GLYPH_PX; r++) {
        int start = -1;
        for (c = 0; c <= BACKLOG_GLYPH_PX; c++) {
            int bit = r * BACKLOG_GLYPH_PX + c;
            int on = c < BACKLOG_GLYPH_PX && (g->bits[bit >> 3] & (0x80 >> (bit & 7)));
            if (on && start < 0) start = c;
            if (!on && start >= 0) {
                fill(x + start * u, y + r * u, (c - start) * u, u);
                start = -1;
            }
        }
    }
}

static void draw_page(const BacklogPage *p, int x, int y, int u)
{
    int px0, py0, px1, py1, i;
    extent(p, &px0, &py0, &px1, &py1);
    color(0, 0, 0, 255);                 /* the shadow, under everything */
    for (i = 0; i < p->nglyph; i++)
        glyph_runs(&p->g[i], x + (p->g[i].x - px0 + 1) * u, y + (p->g[i].y - py0 + 1) * u, u);
    for (i = 0; i < p->nglyph; i++) {
        u16 c = p->g[i].fg;
        color((Uint8)((c & 31) << 3 | (c & 31) >> 2), (Uint8)((c >> 5 & 31) << 3 | (c >> 5 & 31) >> 2),
              (Uint8)((c >> 10 & 31) << 3 | (c >> 10 & 31) >> 2), 255);
        glyph_runs(&p->g[i], x + (p->g[i].x - px0) * u, y + (p->g[i].y - py0) * u, u);
    }
}

/* A small speaker: the page has a voice line. */
static void voice_mark(int x, int y, int u, int playing)
{
    if (playing) color(0xF8, 0xD8, 0x60, 255);     /* UI_C_HEAD */
    else color(0xC8, 0xE8, 0xF8, 255);     /* UI_C_KEY */
    fill(x, y + 3 * u, 3 * u, 4 * u);
    fill(x + 3 * u, y + 2 * u, u, 6 * u);
    fill(x + 4 * u, y + u, u, 8 * u);
    fill(x + 5 * u, y, u, 10 * u);
    if (playing) {
        fill(x + 7 * u, y + 3 * u, u, 4 * u);
        fill(x + 9 * u, y + u, u, 8 * u);
    }
}

void backlog_view_draw(SDL_Renderer *ren, int rx, int ry, int rw, int rh)
{
    int u = rh / 240, x0, y0, i, y, area, n = backlog_count(), sel = backlog_selected();
    char line[64];
    SDL_Rect clip;
    g_ren = ren;
    if (u < 1) u = 1;
    if (u > 4) u = 4;
    while (u > 1 && (VIEW_W * u > rw || VIEW_H * u > rh)) u--;
    x0 = rx + (rw - VIEW_W * u) / 2;
    y0 = ry + (rh - VIEW_H * u) / 2;
    area = VIEW_H - TOP - FOOT;

    ui_fill(ren, rx, ry, rw, rh, 0x00000096u);             /* dim the game */
    ui_draw_panel(ren, x0, y0, VIEW_W, VIEW_H, u);
    text(x0 + EDGE * u, y0 + TITLE_Y * u, u, UI_C_HEAD, "Backlog");
    snprintf(line, sizeof line, "%d/%d", sel + 1, n);
    if (n) text(x0 + (VIEW_W - EDGE - ui_text_width(line)) * u, y0 + TITLE_Y * u, u, UI_C_KEY, line);
    ui_fill(ren, x0 + EDGE * u, y0 + (TOP - 4) * u, (VIEW_W - 2 * EDGE) * u, u, UI_C_RULE);

    if (!n) {
        text(x0 + EDGE * u, y0 + (TOP + 8) * u, u, UI_C_DIM, "No dialogue yet this session");
    } else {
        int h;
        /* keep the selection in view; newest at the bottom */
        if (g_top > sel) g_top = sel;
        if (g_top < 0) g_top = 0;
        for (;;) {
            for (h = 0, i = g_top; i <= sel; i++) h += page_height(backlog_page(i));
            if (h <= area || g_top >= sel) break;
            g_top++;
        }
        /* fill the space below the newest page from above */
        for (;;) {
            for (h = 0, i = g_top; i < n; i++) h += page_height(backlog_page(i));
            if (g_top == 0 || h + page_height(backlog_page(g_top - 1)) > area) break;
            g_top--;
        }
        clip.x = x0 + UI_PANEL_EDGE * u;
        clip.y = y0 + TOP * u;
        clip.w = (VIEW_W - 2 * UI_PANEL_EDGE) * u;
        clip.h = area * u;
        SDL_RenderSetClipRect(ren, &clip);
        /* a short list sits at the bottom, against the game's window */
        for (h = 0, i = g_top; i < n; i++) h += page_height(backlog_page(i));
        y = h < area ? TOP + area - h : TOP;
        for (i = g_top; i < n && y < TOP + area; i++) {
            const BacklogPage *p = backlog_page(i);
            int ph = page_height(p);
            if (i == sel) ui_fill(ren, x0 + (EDGE - 2) * u, y0 + y * u, (VIEW_W - 2 * EDGE + 4) * u,
                                  ph * u, UI_C_SEL);
            if (p->has_voice) voice_mark(x0 + EDGE * u, y0 + (y + PAD + 1) * u, u,
                                         i == sel && voice_replay_active());
            draw_page(p, x0 + (EDGE + GUTTER) * u, y0 + (y + PAD) * u, u);
            y += ph;
        }
        SDL_RenderSetClipRect(ren, NULL);
    }

    ui_fill(ren, x0 + EDGE * u, y0 + (VIEW_H - FOOT + 1) * u, (VIEW_W - 2 * EDGE) * u, u, UI_C_RULE);
    text(x0 + EDGE * u, y0 + (VIEW_H - FOOT + 4) * u, u, UI_C_KEY,
         "\xE2\x86\x91\xE2\x86\x93/Wheel Select   Esc Close");
    if (sel >= 0 && backlog_page(sel)->has_voice)
        text(x0 + EDGE * u, y0 + (VIEW_H - FOOT + 15) * u, u, UI_C_HEAD,
             voice_replay_active() ? "Playing\xE2\x80\xA6   Enter: Again" : "Enter: Replay voice");
    else if (sel >= 0)
        text(x0 + EDGE * u, y0 + (VIEW_H - FOOT + 15) * u, u, UI_C_DIM, "No voice on this page");
}

/* The pad-mapped keys / buttons (circle = OK, cross = back, START closes). */
static void from_mask(unsigned m)
{
    if (m & PC_PAD_UP) backlog_input(BACKLOG_UP);
    else if (m & PC_PAD_DOWN) backlog_input(BACKLOG_DOWN);
    else if (m & PC_PAD_LEFT) backlog_input(BACKLOG_PAGE_UP);
    else if (m & PC_PAD_RIGHT) backlog_input(BACKLOG_PAGE_DOWN);
    else if (m & PC_PAD_CIRCLE) backlog_input(BACKLOG_OK);
    else if (m & (PC_PAD_CROSS | PC_PAD_START)) backlog_input(BACKLOG_BACK);
}

/* An event while the backlog is open; -> 1 consumed (the game never sees
 * it).  The presenter handles the hotkeys before this. */
int backlog_view_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: backlog_input(BACKLOG_UP); return 1;
        case SDL_SCANCODE_DOWN: backlog_input(BACKLOG_DOWN); return 1;
        case SDL_SCANCODE_PAGEUP: backlog_input(BACKLOG_PAGE_UP); return 1;
        case SDL_SCANCODE_PAGEDOWN: backlog_input(BACKLOG_PAGE_DOWN); return 1;
        case SDL_SCANCODE_HOME: backlog_input(BACKLOG_HOME); return 1;
        case SDL_SCANCODE_END: backlog_input(BACKLOG_END); return 1;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            if (!e->key.repeat) backlog_input(BACKLOG_OK);
            return 1;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_BACKSPACE:
            if (!e->key.repeat) backlog_input(BACKLOG_BACK);
            return 1;
        default:
            break;
        }
        if (e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) return 1;
        if (pad_menu_key_mask) {
            unsigned m = pad_menu_key_mask((int)e->key.keysym.scancode);
            if (e->key.repeat) m &= PC_PAD_UP | PC_PAD_DOWN | PC_PAD_LEFT | PC_PAD_RIGHT;
            from_mask(m);
        }
        return 1;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: backlog_input(BACKLOG_UP); return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: backlog_input(BACKLOG_DOWN); return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: backlog_input(BACKLOG_PAGE_UP); return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: backlog_input(BACKLOG_PAGE_DOWN); return 1;
        default: break;
        }
        if (pad_menu_button_mask) from_mask(pad_menu_button_mask(e->cbutton.button));
        return 1;
    case SDL_MOUSEWHEEL: {
        int dy = e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
        if (dy > 0) backlog_input(BACKLOG_UP);
        else if (dy < 0) {
            if (backlog_selected() >= backlog_count() - 1) backlog_input(BACKLOG_BACK);
            else backlog_input(BACKLOG_DOWN);
        }
        return 1;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_RIGHT) backlog_input(BACKLOG_BACK);
        else if (e->button.button == SDL_BUTTON_LEFT) backlog_input(BACKLOG_OK);
        return 1;
    case SDL_KEYUP:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
        return 1;
    default:
        return 0;
    }
}
