/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_present.h -- the PC presenter: window, scaling, overlays, hotkeys.
 *
 * Everything here happens AFTER the PS1 picture exists: the scanout is
 * packed from VRAM exactly as before (pc/hal/gpu_scanout.c), and only the
 * way that 320x240 picture is placed in the window (scale, filter,
 * scanlines, toasts, the settings menu of pc/include/pc_menu.h) is decided
 * here.  The PS1 VRAM and the rasterizer never see any of it.
 *
 * pc/hal/present_layout.c holds the SDL-free parts (layout math, the
 * window-shot names, the PNG writer) so pc/tests/test_settings.c can test
 * them; pc/hal/present.c is the SDL glue.  Every overlay's text is the UI
 * font of pc/include/pc_ui.h (latin12).  system.c / gpu.c / pad.c / spu.c
 * reach present.c through weak references, so the harness binaries that
 * compile those files without it keep their old behaviour.
 */
#ifndef PC_PRESENT_H
#define PC_PRESENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRESENT_SRC_W 320
#define PRESENT_SRC_H 240

typedef struct { int x, y, w, h; } PresentRect;

/* Where the 320x240 picture lands in a win_w x win_h window for a
 * PC_SCALE_* mode.  aspect / integer reproduce SDL2's
 * SDL_RenderSetLogicalSize letterboxing (with / without
 * SDL_RenderSetIntegerScale), which is what the renderer actually uses. */
PresentRect present_layout_rect(int win_w, int win_h, int scale_mode);

/* A window position -> the matching point of the 320x240 picture, for the
 * absolute mouse (pc/hal/pad.c).  (x, y) are in window coordinates of a
 * win_w x win_h window whose renderer draws out_w x out_h pixels (they
 * differ on high-DPI displays); the picture's placement is
 * present_layout_rect(out_w, out_h, scale_mode).  The result is fractional
 * and unclamped: over the letterbox / pillarbox bars it lies outside
 * 0..320 x 0..240.  Returns 1 when the point is on the picture. */
int present_window_to_frame(double x, double y, int win_w, int win_h,
                            int out_w, int out_h, int scale_mode,
                            double *fx, double *fy);

/* The automatic first-launch window: the largest whole multiple of 320x240
 * that fits within 75% of the usable desktop (at least 1x; 2x when the
 * desktop size is unknown, i.e. usable_w/h <= 0). */
void present_auto_window(int usable_w, int usable_h, int *w, int *h);

/* The y extent of the scanline band of source row `row` inside `r`:
 * the lower half of the row's height (at least one pixel). */
void present_scanline_band(PresentRect r, int row, int *y, int *h);

/* A 24-bit RGB PNG (stored deflate blocks).  0 on success. */
int present_png_write(const char *path, const unsigned char *rgb, int w, int h);
/* Read back a PNG of exactly that form (8-bit RGB, no interlace, every
 * row filter 0, stored deflate blocks) and of exactly W x H into RGB
 * (W * H * 3).  0 on success; -1 for anything else (no inflater here). */
int present_png_read(const char *path, unsigned char *rgb, int w, int h);

/* PC_PRESENT_WINDOW_SHOT=PATTERN: the n-th window shot's file (the
 * pattern's first "%d" -> n; without one, the pattern).  0, or -1: too
 * long for cap. */
int present_shot_path(const char *pattern, unsigned n, char *out, size_t cap);

/* Clamp a volume step: v + delta within 0..100. */
int present_volume_step(int v, int delta);

/* A settings-menu row's note (right-aligned, in game pixels): the right
 * edge it may use -- short of the page arrow (PRESENT_MENU_ARROW_W) on a
 * row that opens a page -- and the width left for it after the row's
 * content ends at `end`, keeping a 10-pixel gap.  Returns that width
 * (<= 12: no note drawn) and sets *note_right. */
#define PRESENT_MENU_ARROW_W 10
int present_menu_note_room(int right, int end, int is_page, int *note_right);

/* Notes (pc_present_note): messages the player must get to read -- the
 * launch's "cannot be resumed", "English unavailable", a cancelled save.
 * They queue (PRESENT_NOTES, oldest first) instead of replacing each
 * other, and a note's time runs only while it is on screen: the menu, the
 * backlog, the pause label, a live OSD line (slot list, rewind) or a toast
 * covering the slot holds it until the slot is free again.
 * present_notes_push: 0 queued, 1 already queued (the same text), -1 the
 * queue is full (dropped, counted).  present_notes_show(now, visible): the
 * note to draw at time NOW (ms) when the slot is VISIBLE, else NULL; the
 * shown note's time counts from one visible call to the next. */
#define PRESENT_NOTES 8
#define PRESENT_NOTE_MAX 200
typedef struct { char text[PRESENT_NOTE_MAX]; unsigned left; } PresentNote;
typedef struct {
    PresentNote q[PRESENT_NOTES];
    int head, n, shown;
    unsigned last, dropped;
} PresentNotes;
int present_notes_push(PresentNotes *q, const char *text, unsigned ms);
const char *present_notes_show(PresentNotes *q, unsigned now, int visible);

#ifdef __cplusplus
}
#endif
#endif /* PC_PRESENT_H */
