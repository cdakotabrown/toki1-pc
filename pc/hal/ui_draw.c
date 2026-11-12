/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_draw.c -- the PC layer's UI drawn with SDL (pc/include/pc_ui_draw.h).
 * Text is rasterised by pc/hal/ui_font.c into runs and filled as
 * rectangles, batched per colour; nothing is scaled by the renderer, so
 * every font pixel is exactly u x u output pixels. */
#include "pc_ui_draw.h"

#define BATCH 512

typedef struct {
    SDL_Renderer *ren;
    int x, y, u, n;
    SDL_Rect r[BATCH];
} Spans;

static void set(SDL_Renderer *ren, unsigned rgba) {
    SDL_SetRenderDrawColor(ren, (Uint8)(rgba >> 24), (Uint8)(rgba >> 16), (Uint8)(rgba >> 8),
                           (Uint8)rgba);
}

void ui_fill(SDL_Renderer *ren, int x, int y, int w, int h, unsigned rgba) {
    SDL_Rect r;
    if (w <= 0 || h <= 0) return;
    r.x = x; r.y = y; r.w = w; r.h = h;
    set(ren, rgba);
    SDL_RenderFillRect(ren, &r);
}

static void flush(Spans *sp) {
    if (sp->n) SDL_RenderFillRects(sp->ren, sp->r, sp->n);
    sp->n = 0;
}

static void span(void *ctx, int x, int y, int w) {
    Spans *sp = (Spans *)ctx;
    SDL_Rect *r;
    if (sp->n == BATCH) flush(sp);
    r = &sp->r[sp->n++];
    r->x = sp->x + x * sp->u;
    r->y = sp->y + y * sp->u;
    r->w = w * sp->u;
    r->h = sp->u;
}

/* The batch is a static, not a local: overlays (the rewind's and
 * quicksave's notes) are drawn on the game thread, whose stack is part of
 * a machine state, and 8 KB of rects there would leave host bytes in it.
 * The static is the presenter's scratch (pc/hal/dbg_state.c KEEP_FILES). */
static int pass(SDL_Renderer *ren, int x, int y, int u, unsigned rgba, const char *s) {
    static Spans sp;
    int w;
    sp.ren = ren;
    sp.x = x;
    sp.y = y;
    sp.u = u;
    sp.n = 0;
    set(ren, rgba);
    w = ui_text_spans(s, span, &sp);
    flush(&sp);
    return w;
}

int ui_draw_text(SDL_Renderer *ren, int x, int y, int u, unsigned rgba, int style, const char *s) {
    unsigned dark = (UI_C_SHADOW & 0xFFFFFF00u) | (rgba & 0xFFu);
    if (!s || !*s) return 0;
    if (u < 1) u = 1;
    if (style == UI_SHADOW) {
        pass(ren, x + u, y + u, u, dark, s);
    } else if (style == UI_OUTLINE) {
        int dx, dy;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++)
                if (dx || dy) pass(ren, x + dx * u, y + dy * u, u, dark, s);
    }
    return pass(ren, x, y, u, rgba, s);
}

/* The bevel, outside in, as the game's window draws it (one game pixel
 * each): dark red, red-brown, dark red, cream, copper, dark red. */
static const unsigned k_edge[UI_PANEL_EDGE] = {
    0x300000FFu, 0xA03018FFu, 0x300000FFu, 0xF0D098FFu, 0xC07030FFu, 0x300000FFu,
};
/* the body: the window's green, lighter at the top */
#define BODY_TOP 0x188000u
#define BODY_BOT 0x104400u

void ui_draw_panel(SDL_Renderer *ren, int x, int y, int w, int h, int u) {
    int i, bh = h - 2 * UI_PANEL_EDGE;
    for (i = 0; i < UI_PANEL_EDGE; i++) {
        int k = i * u;
        unsigned c = k_edge[i];
        ui_fill(ren, x + k, y + k, (w - 2 * i) * u, u, c);                  /* top */
        ui_fill(ren, x + k, y + (h - 1 - i) * u, (w - 2 * i) * u, u, c);    /* bottom */
        ui_fill(ren, x + k, y + k, u, (h - 2 * i) * u, c);                  /* left */
        ui_fill(ren, x + (w - 1 - i) * u, y + k, u, (h - 2 * i) * u, c);    /* right */
    }
    /* the gradient in bands of 4 game pixels */
    for (i = 0; i < bh; i += 4) {
        int t = bh > 1 ? i * 256 / (bh - 1) : 0, b = 4;
        unsigned c = 0;
        int sh;
        for (sh = 0; sh < 24; sh += 8) {
            int a = (BODY_TOP >> sh) & 0xFF, z = (BODY_BOT >> sh) & 0xFF;
            /* 5 bits a channel, as the console shades it */
            c |= (unsigned)((a + (z - a) * t / 256) & 0xF8) << sh;
        }
        if (i + b > bh) b = bh - i;
        ui_fill(ren, x + UI_PANEL_EDGE * u, y + (UI_PANEL_EDGE + i) * u,
                (w - 2 * UI_PANEL_EDGE) * u, b * u, c << 8 | 0xFFu);
    }
}

void ui_draw_box(SDL_Renderer *ren, int x, int y, int w, int h, int u) {
    ui_fill(ren, x - u, y - u, (w + 2) * u, (h + 2) * u, 0x300000FFu);
    ui_fill(ren, x, y, w * u, h * u, UI_C_RULE);
    ui_fill(ren, x + u, y + u, (w - 2) * u, (h - 2) * u, UI_C_BOX);
}
