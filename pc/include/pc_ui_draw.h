/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_ui_draw.h -- drawing the PC layer's UI with SDL (pc/hal/ui_draw.c):
 * latin12 text (pc/include/pc_ui.h) and the game-style window.
 *
 * Positions are output pixels; sizes in units are multiplied by u, the
 * whole number of output pixels per game pixel the caller draws at.
 * Everything is axis-aligned rectangles: crisp at any u. */
#ifndef PC_UI_DRAW_H
#define PC_UI_DRAW_H

#include <SDL2/SDL.h>
#include "pc_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PANEL_EDGE 6         /* the window's bevelled frame, units */

void ui_fill(SDL_Renderer *ren, int x, int y, int w, int h, unsigned rgba);

/* UTF-8 text with its box's top-left at (x, y), each font pixel u x u, in
 * rgba; UI_SHADOW adds the black copy one unit down and right (the game's
 * own text), UI_OUTLINE a black unit all round (over pictures).  Returns
 * the width in units. */
int ui_draw_text(SDL_Renderer *ren, int x, int y, int u, unsigned rgba, int style, const char *s);

/* The game's message window: the dark-red / copper / cream bevel and the
 * green body shading darker downwards; w x h units at (x, y). */
void ui_draw_panel(SDL_Renderer *ren, int x, int y, int w, int h, int u);

/* A popup over a panel: one copper unit round a dark green body. */
void ui_draw_box(SDL_Renderer *ren, int x, int y, int w, int h, int u);

#ifdef __cplusplus
}
#endif
#endif /* PC_UI_DRAW_H */
