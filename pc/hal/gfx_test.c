/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gfx_test.c — Graphics test screen for the PC port.
 *
 * Activated via F9 or GFXTEST=1 env var. Tests the entire graphics pipeline:
 *   1. TIM file loading from assets
 *   2. VRAM upload via LoadImage
 *   3. GPU primitive rendering (SPRT, POLY_FT4, TILE, etc.)
 *
 * Navigation:
 *   Up/Down  — move cursor
 *   Tab      — switch test category
 *   Enter    — execute current test
 *   F9/Esc   — close graphics test
 *
 * The goal is to verify sprites and textures render correctly before
 * debugging the full game overlay flow.
 */
#include "hal.h"
#include "pc_data_root.h"
#include "gpu_vram.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

extern void debug_log(const char *fmt, ...);
extern void draw_sprt(s32 x, s32 y, s32 w, s32 h, u8 u0, u8 v0,
                      u16 clut, u16 tpage, u8 r, u8 g, u8 b);
extern void LoadImage(RECT *rect, u32 *src);
extern void debug_screenshot(const char *path);

/* TIM file parsing structures */
typedef struct {
    u32 magic;      /* 0x10 = TIM */
    u32 flags;      /* bits 0-2: bpp (0=4bit, 1=8bit, 2=16bit, 3=24bit) */
} TIMHeader;

typedef struct {
    u32 length;     /* block length including this field */
    u16 x, y;       /* VRAM position */
    u16 w, h;       /* dimensions in 16-bit units */
    /* pixel/clut data follows */
} TIMBlock;

static int g_active = 0;
static int g_cursor = 0;
static char g_status[128] = "Graphics test ready";

#define MAX_TIMS 64
static char g_tim_paths[MAX_TIMS][256];
static char g_tim_names[MAX_TIMS][32];
static int g_tim_count = 0;
static int g_scanned = 0;

/* Track which TIMs have been loaded to VRAM */
static int g_tim_loaded[MAX_TIMS] = {0};
static u16 g_tim_vram_x[MAX_TIMS];
static u16 g_tim_vram_y[MAX_TIMS];
static u16 g_tim_width[MAX_TIMS];
static u16 g_tim_height[MAX_TIMS];
static u16 g_tim_tpage[MAX_TIMS];
static u16 g_tim_clut[MAX_TIMS];
static u8  g_tim_bpp[MAX_TIMS];

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

static int ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), tl = strlen(suffix);
    if (tl > sl) return 0;
    return strcasecmp(s + sl - tl, suffix) == 0;
}

static void scan_tim_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!ends_with(ent->d_name, ".TIM") && !ends_with(ent->d_name, ".tim")) continue;
        if (g_tim_count >= MAX_TIMS) break;
        snprintf(g_tim_paths[g_tim_count], sizeof(g_tim_paths[0]), "%s/%s", dir, ent->d_name);
        const char *dot = strrchr(ent->d_name, '.');
        size_t stem_len = dot ? (size_t)(dot - ent->d_name) : strlen(ent->d_name);
        if (stem_len >= sizeof(g_tim_names[0])) stem_len = sizeof(g_tim_names[0]) - 1;
        memcpy(g_tim_names[g_tim_count], ent->d_name, stem_len);
        g_tim_names[g_tim_count][stem_len] = '\0';
        g_tim_count++;
    }
    closedir(d);
}

static void scan_assets(void) {
    if (g_scanned) return;
    g_scanned = 1;
    /* Scan various locations where TIM files might be */
    {
        char d[1100];
        scan_tim_dir(pc_asset_path(d, sizeof d, "textures"));
        scan_tim_dir(pc_asset_path(d, sizeof d, "extracted/tim"));
        scan_tim_dir(pc_asset_path(d, sizeof d, "raw/CDROM/FUKUTEST"));
    }
    scan_tim_dir("assets/extracted/CDROM/FUKUTEST");
    scan_tim_dir("build/disc_extract/CDROM/FUKUTEST");
    debug_log("Graphics test: scanned %d TIM files", g_tim_count);
}

/* Load a TIM file and upload to VRAM */
static int load_tim(int idx) {
    if (idx < 0 || idx >= g_tim_count) return 0;
    if (g_tim_loaded[idx]) {
        set_status("TIM %s already loaded at (%d,%d)", g_tim_names[idx],
                   g_tim_vram_x[idx], g_tim_vram_y[idx]);
        return 1;
    }

    FILE *f = fopen(g_tim_paths[idx], "rb");
    if (!f) {
        set_status("Failed to open %s", g_tim_paths[idx]);
        return 0;
    }

    TIMHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        set_status("Failed to read TIM header");
        return 0;
    }

    if (hdr.magic != 0x10) {
        fclose(f);
        set_status("Invalid TIM magic 0x%08x", hdr.magic);
        return 0;
    }

    u8 bpp = hdr.flags & 0x7;
    int has_clut = (hdr.flags >> 3) & 1;
    u16 clut_vram_y = 0;
    u16 clut_vram_x = 0;

    debug_log("TIM %s: bpp=%d has_clut=%d flags=0x%08x",
              g_tim_names[idx], bpp, has_clut, hdr.flags);

    /* Read CLUT block if present */
    if (has_clut) {
        TIMBlock clut_blk;
        if (fread(&clut_blk, sizeof(clut_blk), 1, f) != 1) {
            fclose(f);
            set_status("Failed to read CLUT block");
            return 0;
        }
        u32 clut_data_size = clut_blk.length - 12;
        u16 *clut_data = (u16 *)malloc(clut_data_size);
        if (!clut_data || fread(clut_data, 1, clut_data_size, f) != clut_data_size) {
            free(clut_data);
            fclose(f);
            set_status("Failed to read CLUT data");
            return 0;
        }
        /* Upload CLUT to VRAM */
        RECT clut_rect;
        clut_rect.x = clut_blk.x;
        clut_rect.y = clut_blk.y;
        clut_rect.w = clut_blk.w;
        clut_rect.h = clut_blk.h;
        LoadImage(&clut_rect, (u32 *)clut_data);
        clut_vram_x = clut_blk.x;
        clut_vram_y = clut_blk.y;
        debug_log("  CLUT: vram(%d,%d) %dx%d", clut_blk.x, clut_blk.y, clut_blk.w, clut_blk.h);
        free(clut_data);
    }

    /* Read pixel block */
    TIMBlock pix_blk;
    if (fread(&pix_blk, sizeof(pix_blk), 1, f) != 1) {
        fclose(f);
        set_status("Failed to read pixel block header");
        return 0;
    }
    u32 pix_data_size = pix_blk.length - 12;
    u16 *pix_data = (u16 *)malloc(pix_data_size);
    if (!pix_data || fread(pix_data, 1, pix_data_size, f) != pix_data_size) {
        free(pix_data);
        fclose(f);
        set_status("Failed to read pixel data");
        return 0;
    }

    /* Upload pixels to VRAM */
    RECT pix_rect;
    pix_rect.x = pix_blk.x;
    pix_rect.y = pix_blk.y;
    pix_rect.w = pix_blk.w;
    pix_rect.h = pix_blk.h;
    LoadImage(&pix_rect, (u32 *)pix_data);
    debug_log("  Pixels: vram(%d,%d) %dx%d", pix_blk.x, pix_blk.y, pix_blk.w, pix_blk.h);
    free(pix_data);
    fclose(f);

    /* Calculate actual texture dimensions based on bpp */
    u16 tex_w = pix_blk.w;
    if (bpp == 0) tex_w *= 4;       /* 4bpp: 4 pixels per 16-bit word */
    else if (bpp == 1) tex_w *= 2;  /* 8bpp: 2 pixels per 16-bit word */
    /* 16bpp: 1 pixel per word (no change) */

    /* Calculate tpage: (Y/256)*16 + (X/64) for 4-bit mode */
    u16 tpage_x = pix_blk.x / 64;
    u16 tpage_y = pix_blk.y / 256;
    u16 tpage = tpage_y * 16 + tpage_x;
    /* Add bpp to tpage bits 7-8 */
    tpage |= (bpp << 7);

    /* CLUT address for 4/8 bit modes */
    u16 clut = 0;
    if (has_clut) {
        clut = ((clut_vram_y & 0x1FF) << 6) | ((clut_vram_x / 16) & 0x3F);
    }

    g_tim_loaded[idx] = 1;
    g_tim_vram_x[idx] = pix_blk.x;
    g_tim_vram_y[idx] = pix_blk.y;
    g_tim_width[idx] = tex_w;
    g_tim_height[idx] = pix_blk.h;
    g_tim_tpage[idx] = tpage;
    g_tim_clut[idx] = clut;
    g_tim_bpp[idx] = bpp;

    set_status("Loaded %s to VRAM(%d,%d) %dx%d tpage=%d clut=0x%04x",
               g_tim_names[idx], pix_blk.x, pix_blk.y, tex_w, pix_blk.h, tpage, clut);
    return 1;
}

int gfxtest_is_active(void) {
    static int env_checked = 0;
    if (!env_checked) {
        const char *env = getenv("GFXTEST");
        const char *env3d = getenv("GFXTEST3D");
        env_checked = 1;
        if ((env && env[0] && env[0] != '0') || (env3d && env3d[0] && env3d[0] != '0')) {
            scan_assets();
            g_active = 1;
            set_status("Auto-opened via GFXTEST env var, %d TIMs found", g_tim_count);
            debug_log("Graphics test: auto-opened");
            if (g_tim_count > 0) {
                load_tim(0);
            }
            /* Auto-enable 3D mode if GFXTEST3D is set */
            if (env3d && env3d[0] && env3d[0] != '0') {
                gfxtest_toggle_3d();
                debug_log("Graphics test: 3D mode auto-enabled via GFXTEST3D");
            }
        }
    }
    return g_active;
}

void gfxtest_toggle(void) {
    scan_assets();
    g_active = !g_active;
    if (g_active) {
        set_status("Graphics test: %d TIM files, ENTER to load, TAB to switch", g_tim_count);
    }
    debug_log("Graphics test: %s", g_active ? "OPEN" : "CLOSED");
}

int gfxtest_handle_key(SDL_Event *e) {
    if (!g_active) return 0;
    if (e->type != SDL_KEYDOWN) return 0;
    int n = g_tim_count;
    switch (e->key.keysym.sym) {
    case SDLK_F7:
        gfxtest_toggle_3d();
        return 1;
    case SDLK_UP:
        if (n > 0) g_cursor = (g_cursor + n - 1) % n;
        return 1;
    case SDLK_DOWN:
        if (n > 0) g_cursor = (g_cursor + 1) % n;
        return 1;
    case SDLK_PAGEUP:
        if (n > 0) {
            g_cursor -= 10;
            if (g_cursor < 0) g_cursor = 0;
        }
        return 1;
    case SDLK_PAGEDOWN:
        if (n > 0) {
            g_cursor += 10;
            if (g_cursor >= n) g_cursor = n - 1;
        }
        return 1;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        load_tim(g_cursor);
        return 1;
    case SDLK_l:
        /* Load all TIMs at once */
        for (int i = 0; i < g_tim_count; i++) {
            load_tim(i);
        }
        set_status("Loaded all %d TIMs", g_tim_count);
        return 1;
    default:
        return 0;
    }
}

/* Minimal text renderer using the system font tpage */
static void gt_text(s32 x, s32 y, const char *text, u8 r, u8 g, u8 b) {
    u16 tpage = 0x000F;  /* tpage 15 = VRAM X=960 where font is loaded */
    u16 clut = (481 << 6) | 0;
    static int text_trace = 0;
    for (int i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            s32 u0, v0;
            if (ch >= '0' && ch <= '9') { u0 = (ch - '0') * 8; v0 = 16; }
            else { u0 = (ch - 'A') * 8; v0 = 0; }
            draw_sprt(x + i * 8, y, 8, 14, u0, v0, clut, tpage, r, g, b);
            if (text_trace < 5) {
                text_trace++;
                /* Check what texel_fetch returns for first pixel of this char */
                extern u16 texel_fetch_debug(u8 u, u8 v, u16 clut, u16 tpage);
                u16 t = texel_fetch_debug(u0, v0, clut, tpage);
                debug_log("[GFXTEST] ch='%c' uv=(%d,%d) tpage=0x%04X clut=0x%04X texel[0,0]=0x%04X",
                          ch, u0, v0, tpage, clut, t);
            }
        }
    }
}

#define VIEW_ROWS 12

/* Forward declarations for primitive drawing from gpu.c */
extern void draw_poly_f3(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2, u8 r, u8 g, u8 b);
extern void draw_poly_g3(s32 x0, s32 y0, u8 r0, u8 g0, u8 b0,
                         s32 x1, s32 y1, u8 r1, u8 g1, u8 b1,
                         s32 x2, s32 y2, u8 r2, u8 g2, u8 b2);
extern void draw_poly_ft3(s32 x0, s32 y0, u8 u0, u8 v0,
                          s32 x1, s32 y1, u8 u1, u8 v1,
                          s32 x2, s32 y2, u8 u2, u8 v2,
                          u16 clut, u16 tpage, u8 r, u8 g, u8 b);
extern void draw_poly_gt3(s32 x0, s32 y0, u8 u0, u8 v0, u8 r0, u8 g0, u8 b0,
                          s32 x1, s32 y1, u8 u1, u8 v1, u8 r1, u8 g1, u8 b1,
                          s32 x2, s32 y2, u8 u2, u8 v2, u8 r2, u8 g2, u8 b2,
                          u16 clut, u16 tpage);
extern void draw_line_g2(s32 x0, s32 y0, u8 r0, u8 g0, u8 b0,
                         s32 x1, s32 y1, u8 r1, u8 g1, u8 b1);
extern void draw_tile(s32 x, s32 y, s32 w, s32 h, u8 r, u8 g, u8 b);
extern u16 g_vram[512][1024];

/* Comprehensive graphics test - tests all primitive types */
static void draw_all_primitives(void) {
    int i;

    /* === ROW 1: Sprites with different color modes === */

    /* 8bpp Portrait with grayscale CLUT */
    u16 portrait_tpage = 0x0085;  /* tpage 5, 8bpp */
    u16 portrait_clut = (480 << 6) | 0;  /* grayscale at Y=480 */
    draw_sprt(5, 22, 40, 40, 0, 0, portrait_clut, portrait_tpage, 128, 128, 128);
    gt_text(5, 64, "8BPP", 180, 180, 255);

    /* 4bpp Font */
    u16 font_tpage = 0x000F;  /* tpage 15, 4bpp */
    u16 font_clut = (481 << 6) | 0;
    for (i = 0; i < 5; i++) {
        draw_sprt(50 + i * 8, 22, 8, 14, i * 8, 0, font_clut, font_tpage, 128, 128, 128);
    }
    gt_text(50, 40, "4BPP", 180, 180, 255);
    gt_text(50, 52, "FONT", 150, 150, 255);

    /* Direct 16bpp copy from VRAM */
    {
        s32 sx, sy;
        for (sy = 0; sy < 30; sy++) {
            for (sx = 0; sx < 30; sx++) {
                g_vram[22 + sy][100 + sx] = g_vram[sy][320 + sx];  /* Portrait area */
            }
        }
    }
    gt_text(100, 54, "16B", 180, 180, 255);

    /* === ROW 2: Flat and Gouraud polygons (3D primitives) === */

    /* Flat-colored triangle (POLY_F3) */
    draw_poly_f3(10, 75, 40, 75, 25, 105, 255, 0, 0);
    gt_text(5, 108, "F3", 255, 100, 100);

    /* Gouraud-shaded triangle (POLY_G3) */
    draw_poly_g3(50, 75, 255, 0, 0,
                 80, 75, 0, 255, 0,
                 65, 105, 0, 0, 255);
    gt_text(50, 108, "G3", 100, 255, 100);

    /* Gouraud quad (POLY_G4) - two triangles */
    draw_poly_g3(95, 75, 255, 0, 0,      /* TL - red */
                 135, 75, 0, 255, 0,      /* TR - green */
                 95, 105, 0, 0, 255);     /* BL - blue */
    draw_poly_g3(135, 75, 0, 255, 0,     /* TR - green */
                 95, 105, 0, 0, 255,      /* BL - blue */
                 135, 105, 255, 255, 0);  /* BR - yellow */
    gt_text(95, 108, "G4", 255, 255, 100);

    /* Textured triangle (POLY_FT3) - uses font texture at tpage 15 */
    {
        u16 tex_tpage = 0x008F;  /* tpage 15, 8bpp (font texture) */
        u16 tex_clut = (481 << 6) | 0;
        /* Flat-textured triangle */
        draw_poly_ft3(145, 75, 0, 0,
                      175, 75, 30, 0,
                      160, 105, 15, 30,
                      tex_clut, tex_tpage, 128, 128, 128);
    }
    gt_text(145, 108, "FT3", 180, 180, 255);

    /* Gouraud-textured triangle (POLY_GT3) */
    {
        u16 tex_tpage = 0x008F;  /* tpage 15, 8bpp */
        u16 tex_clut = (481 << 6) | 0;
        /* Per-vertex RGB modulation */
        draw_poly_gt3(185, 75, 0, 0, 255, 128, 128,
                      215, 75, 30, 0, 128, 255, 128,
                      200, 105, 15, 30, 128, 128, 255,
                      tex_clut, tex_tpage);
    }
    gt_text(185, 108, "GT3", 255, 200, 150);

    /* === ROW 3: Tiles and Lines === */

    /* Flat tiles */
    draw_tile(10, 120, 20, 20, 0, 128, 255);
    draw_tile(35, 120, 20, 20, 255, 128, 0);
    gt_text(10, 142, "TILE", 100, 200, 255);

    /* Gouraud lines */
    draw_line_g2(70, 120, 255, 0, 0, 120, 120, 0, 255, 0);
    draw_line_g2(70, 125, 0, 0, 255, 120, 135, 255, 255, 0);
    draw_line_g2(70, 130, 255, 0, 255, 120, 120, 0, 255, 255);
    gt_text(70, 142, "LINE", 255, 100, 255);

    /* === ROW 4: Blending/transparency test === */

    /* Base colors */
    draw_tile(10, 155, 30, 25, 255, 0, 0);
    draw_tile(25, 160, 30, 25, 0, 0, 255);
    gt_text(10, 185, "BLEND", 200, 200, 200);

    /* More gradient lines */
    for (i = 0; i < 5; i++) {
        draw_line_g2(70, 155 + i * 5, 255, (u8)(i * 50), 0,
                     120, 155 + i * 5, 0, (u8)(255 - i * 50), 255);
    }
    gt_text(70, 185, "MULTI", 200, 200, 200);

    /* === ROW 5: Raw VRAM samples === */

    /* Show raw VRAM from different areas */
    {
        s32 sx, sy;
        /* VRAM(120,64) - NBN sprite area */
        for (sy = 0; sy < 20; sy++) {
            for (sx = 0; sx < 25; sx++) {
                g_vram[195 + sy][10 + sx] = g_vram[64 + sy][120 + sx];
            }
        }
        /* VRAM(192,80) - another sprite area */
        for (sy = 0; sy < 20; sy++) {
            for (sx = 0; sx < 25; sx++) {
                g_vram[195 + sy][40 + sx] = g_vram[80 + sy][192 + sx];
            }
        }
        /* VRAM(960,128) - MAINROM area */
        for (sy = 0; sy < 20; sy++) {
            for (sx = 0; sx < 25; sx++) {
                g_vram[195 + sy][70 + sx] = g_vram[128 + sy][960 + sx];
            }
        }
    }
    gt_text(10, 217, "VRAM", 150, 150, 150);

    /* === Right side: Color test bars === */

    /* Color gradient bars */
    for (i = 0; i < 32; i++) {
        draw_tile(280, 22 + i * 3, 35, 3, i * 8, 0, 0);  /* Red ramp */
    }
    for (i = 0; i < 32; i++) {
        draw_tile(245, 22 + i * 3, 35, 3, 0, i * 8, 0);  /* Green ramp */
    }
    for (i = 0; i < 32; i++) {
        draw_tile(210, 22 + i * 3, 35, 3, 0, 0, i * 8);  /* Blue ramp */
    }

    /* Rainbow at bottom */
    {
        s32 colors[7][3] = {
            {255,0,0}, {255,128,0}, {255,255,0}, {0,255,0},
            {0,255,255}, {0,0,255}, {128,0,255}
        };
        for (i = 0; i < 7; i++) {
            draw_tile(130 + i * 12, 195, 12, 20, colors[i][0], colors[i][1], colors[i][2]);
        }
    }
}

/* ============================================================
 * TMD MODEL LOADER - Loads actual PS1 3D models from game assets
 * ============================================================
 * TMD (Tokimeki Model Data / PlayStation Model Data) format:
 *   Header: version, flags, object count
 *   Per-object: vertex list, normal list, primitive list
 */

#define MAX_TMD_VERTS 256
#define MAX_TMD_PRIMS 256

static SVECTOR g_tmd_verts[MAX_TMD_VERTS];
static SVECTOR g_tmd_normals[MAX_TMD_VERTS];
static int g_tmd_vert_count = 0;
static int g_tmd_prim_count = 0;
static int g_tmd_loaded = 0;
static char g_tmd_name[64] = "";

/* TMD primitive storage - simplified: store as quads with vertex indices */
typedef struct {
    u16 v0, v1, v2, v3;   /* vertex indices */
    u8 r0, g0, b0;        /* vertex 0 color */
    u8 r1, g1, b1;        /* vertex 1 color */
    u8 r2, g2, b2;        /* vertex 2 color */
    u8 r3, g3, b3;        /* vertex 3 color */
    u8 is_quad;           /* 1 = quad, 0 = triangle */
} TMDPrim;

static TMDPrim g_tmd_prims[MAX_TMD_PRIMS];

/* Load a TMD file from the game assets - handles multiple objects */
static int load_tmd_at(const char *path);
/* rel: under the asset root */
static int load_tmd(const char *rel) {
    char p[1100];
    return load_tmd_at(pc_asset_path(p, sizeof p, rel));
}

static int load_tmd_at(const char *path) {
    FILE *f = fopen(path, "rb");
    u8 header[12];
    u32 version, flags, num_objects;
    u32 obj, total_verts = 0, total_prims = 0;
    u8 *file_data;
    long file_size;
    int i;

    if (!f) {
        debug_log("TMD: Cannot open %s", path);
        return 0;
    }

    /* Read main header (12 bytes) */
    if (fread(header, 1, 12, f) != 12) {
        fclose(f);
        return 0;
    }

    version = *(u32*)&header[0];
    flags = *(u32*)&header[4];
    num_objects = *(u32*)&header[8];

    if (version != 0x41) {
        debug_log("TMD: Unknown version 0x%X", version);
        fclose(f);
        return 0;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file_data = (u8*)malloc(file_size);
    if (!file_data) { fclose(f); return 0; }
    fread(file_data, 1, file_size, f);
    fclose(f);

    g_tmd_vert_count = 0;
    g_tmd_prim_count = 0;

    /* Object table starts at offset 12, each entry is 28 bytes */
    for (obj = 0; obj < num_objects && obj < 64; obj++) {
        u32 obj_ofs = 12 + obj * 28;
        if (obj_ofs + 28 > (u32)file_size) break;

        u32 vert_ofs = *(u32*)&file_data[obj_ofs + 0];
        u32 vert_count = *(u32*)&file_data[obj_ofs + 4];
        u32 norm_ofs = *(u32*)&file_data[obj_ofs + 8];
        u32 norm_count = *(u32*)&file_data[obj_ofs + 12];
        u32 prim_ofs = *(u32*)&file_data[obj_ofs + 16];
        u32 prim_count = *(u32*)&file_data[obj_ofs + 20];

        /* Vertices are relative to object table start (offset 12) */
        u32 vert_base = 12 + vert_ofs;
        for (i = 0; i < (int)vert_count && g_tmd_vert_count < MAX_TMD_VERTS; i++) {
            u32 v_ofs = vert_base + i * 8;
            if (v_ofs + 8 > (u32)file_size) break;
            g_tmd_verts[g_tmd_vert_count].vx = *(s16*)&file_data[v_ofs];
            g_tmd_verts[g_tmd_vert_count].vy = *(s16*)&file_data[v_ofs + 2];
            g_tmd_verts[g_tmd_vert_count].vz = *(s16*)&file_data[v_ofs + 4];
            g_tmd_verts[g_tmd_vert_count].pad = 0;
            g_tmd_vert_count++;
        }

        /* Primitives are relative to object table start (offset 12) */
        u32 prim_base = 12 + prim_ofs;
        u32 prim_end = (obj + 1 < num_objects) ?
            12 + *(u32*)&file_data[12 + (obj+1)*28 + 16] : (u32)file_size;

        /* Parse this object's primitives */
        u32 ofs = prim_base;
        u32 prims_parsed = 0;
        while (ofs + 4 < prim_end && prims_parsed < prim_count && g_tmd_prim_count < MAX_TMD_PRIMS) {
            u8 olen = file_data[ofs];
            u8 ilen = file_data[ofs+1];
            u8 flag = file_data[ofs+2];
            u8 mode = file_data[ofs+3];

            if (olen == 0 || ilen == 0) break;

            u8 *p = &file_data[ofs + 4];
            TMDPrim *prim = &g_tmd_prims[g_tmd_prim_count];
            int data_len = ilen * 4;
            u32 vert_idx_base = total_verts;  /* offset for this object's verts */

            /* Mode 0x30 = POLY_G3 (gouraud untextured triangle) - most common */
            if (mode == 0x30 && ilen >= 4) {
                prim->r0 = p[0]; prim->g0 = p[1]; prim->b0 = p[2];
                prim->r1 = p[4]; prim->g1 = p[5]; prim->b1 = p[6];
                prim->r2 = p[8]; prim->g2 = p[9]; prim->b2 = p[10];
                prim->v0 = (*(u16*)&p[12]) + vert_idx_base;
                prim->v1 = (*(u16*)&p[14]) + vert_idx_base;
                prim->v2 = (*(u16*)&p[16]) + vert_idx_base;
                prim->v3 = prim->v2;
                prim->r3 = prim->r2; prim->g3 = prim->g2; prim->b3 = prim->b2;
                prim->is_quad = 0;
                g_tmd_prim_count++;
            }
            /* Mode 0x38 = POLY_G4 (gouraud untextured quad) */
            else if (mode == 0x38 && ilen >= 5) {
                prim->r0 = p[0]; prim->g0 = p[1]; prim->b0 = p[2];
                prim->r1 = p[4]; prim->g1 = p[5]; prim->b1 = p[6];
                prim->r2 = p[8]; prim->g2 = p[9]; prim->b2 = p[10];
                prim->r3 = p[12]; prim->g3 = p[13]; prim->b3 = p[14];
                prim->v0 = (*(u16*)&p[16]) + vert_idx_base;
                prim->v1 = (*(u16*)&p[18]) + vert_idx_base;
                prim->v2 = (*(u16*)&p[20]) + vert_idx_base;
                prim->v3 = (*(u16*)&p[22]) + vert_idx_base;
                prim->is_quad = 1;
                g_tmd_prim_count++;
            }
            /* Mode 0x20 = POLY_F3 (flat untextured triangle) */
            else if (mode == 0x20 && ilen >= 2) {
                prim->r0 = p[0]; prim->g0 = p[1]; prim->b0 = p[2];
                prim->v0 = (*(u16*)&p[4]) + vert_idx_base;
                prim->v1 = (*(u16*)&p[6]) + vert_idx_base;
                prim->v2 = (*(u16*)&p[8]) + vert_idx_base;
                prim->v3 = prim->v2;
                prim->r1 = prim->r2 = prim->r3 = prim->r0;
                prim->g1 = prim->g2 = prim->g3 = prim->g0;
                prim->b1 = prim->b2 = prim->b3 = prim->b0;
                prim->is_quad = 0;
                g_tmd_prim_count++;
            }
            /* Mode 0x21 = POLY_F3 with normals (flag=1) - ilen=3 */
            else if (mode == 0x21 && ilen >= 3) {
                prim->r0 = p[0]; prim->g0 = p[1]; prim->b0 = p[2];
                /* With normals: v0+n0(4), v1+n1(4), v2+n2(4) */
                prim->v0 = (*(u16*)&p[4]) + vert_idx_base;
                prim->v1 = (*(u16*)&p[8]) + vert_idx_base;
                prim->v2 = (*(u16*)&p[10]) + vert_idx_base;
                prim->v3 = prim->v2;
                prim->r1 = prim->r2 = prim->r3 = prim->r0;
                prim->g1 = prim->g2 = prim->g3 = prim->g0;
                prim->b1 = prim->b2 = prim->b3 = prim->b0;
                prim->is_quad = 0;
                g_tmd_prim_count++;
            }
            /* Mode 0x24 = POLY_FT3 (flat textured triangle) - ilen=5 */
            else if (mode == 0x24 && ilen >= 5) {
                /* u0v0+clut(4), r0g0b0+code(4), u1v1+tpage(4), v0+n0(4), v1+v2(4) */
                prim->r0 = p[4]; prim->g0 = p[5]; prim->b0 = p[6];
                prim->v0 = (*(u16*)&p[12]) + vert_idx_base;
                prim->v1 = (*(u16*)&p[16]) + vert_idx_base;
                prim->v2 = (*(u16*)&p[18]) + vert_idx_base;
                prim->v3 = prim->v2;
                prim->r1 = prim->r2 = prim->r3 = prim->r0;
                prim->g1 = prim->g2 = prim->g3 = prim->g0;
                prim->b1 = prim->b2 = prim->b3 = prim->b0;
                prim->is_quad = 0;
                g_tmd_prim_count++;
            }
            /* Mode 0x25 = POLY_FT3 with normals - ilen=6 */
            else if (mode == 0x25 && ilen >= 6) {
                prim->r0 = p[4]; prim->g0 = p[5]; prim->b0 = p[6];
                prim->v0 = (*(u16*)&p[12]) + vert_idx_base;
                prim->v1 = (*(u16*)&p[16]) + vert_idx_base;
                prim->v2 = (*(u16*)&p[20]) + vert_idx_base;
                prim->v3 = prim->v2;
                prim->r1 = prim->r2 = prim->r3 = prim->r0;
                prim->g1 = prim->g2 = prim->g3 = prim->g0;
                prim->b1 = prim->b2 = prim->b3 = prim->b0;
                prim->is_quad = 0;
                g_tmd_prim_count++;
            }

            ofs += 4 + data_len;
            prims_parsed++;
        }

        total_verts += vert_count;
        total_prims += prim_count;
    }

    debug_log("TMD: %s - %d objs, %d verts, %d prims loaded",
              path, num_objects, g_tmd_vert_count, g_tmd_prim_count);

    /* Extract filename */
    const char *slash = strrchr(path, '/');
    snprintf(g_tmd_name, sizeof(g_tmd_name), "%s", slash ? slash+1 : path);

    g_tmd_loaded = 1;
    debug_log("TMD: Loaded %d vertices, %d primitives from %s",
              g_tmd_vert_count, g_tmd_prim_count, g_tmd_name);

    return 1;
}

/* Draw the loaded TMD model with GTE transforms */
static void draw_tmd_model(s16 rot_x, s16 rot_y, s16 rot_z, s32 trans_z) {
    MATRIX rot_matrix;
    VECTOR trans_vec;
    SVECTOR rot_angles;
    s32 screen_xy[MAX_TMD_VERTS];
    s32 screen_z[MAX_TMD_VERTS];
    s32 flag;
    int i;

    if (!g_tmd_loaded || g_tmd_vert_count == 0) return;

    /* Set up rotation */
    rot_angles.vx = rot_x;
    rot_angles.vy = rot_y;
    rot_angles.vz = rot_z;
    RotMatrix(&rot_angles, &rot_matrix);

    trans_vec.vx = 0;
    trans_vec.vy = 0;
    trans_vec.vz = trans_z;
    TransMatrix(&rot_matrix, &trans_vec);

    SetRotMatrix(&rot_matrix);
    SetTransMatrix(&rot_matrix);

    /* Transform all vertices */
    for (i = 0; i < g_tmd_vert_count; i++) {
        s32 p;
        RotTransPers(&g_tmd_verts[i], &screen_xy[i], &p, &flag);
        screen_z[i] = p;
    }

    /* Draw primitives (simple - no depth sorting for now) */
    for (i = 0; i < g_tmd_prim_count; i++) {
        TMDPrim *p = &g_tmd_prims[i];

        if (p->v0 >= g_tmd_vert_count || p->v1 >= g_tmd_vert_count ||
            p->v2 >= g_tmd_vert_count || p->v3 >= g_tmd_vert_count) continue;

        s16 x0 = (s16)(screen_xy[p->v0] & 0xFFFF);
        s16 y0 = (s16)(screen_xy[p->v0] >> 16);
        s16 x1 = (s16)(screen_xy[p->v1] & 0xFFFF);
        s16 y1 = (s16)(screen_xy[p->v1] >> 16);
        s16 x2 = (s16)(screen_xy[p->v2] & 0xFFFF);
        s16 y2 = (s16)(screen_xy[p->v2] >> 16);

        /* Backface cull */
        s32 nc = NormalClip(screen_xy[p->v0], screen_xy[p->v1], screen_xy[p->v2]);
        if (nc <= 0) continue;

        if (p->is_quad) {
            s16 x3 = (s16)(screen_xy[p->v3] & 0xFFFF);
            s16 y3 = (s16)(screen_xy[p->v3] >> 16);
            /* Draw as two triangles */
            draw_poly_g3(x0, y0, p->r0, p->g0, p->b0,
                         x1, y1, p->r1, p->g1, p->b1,
                         x2, y2, p->r2, p->g2, p->b2);
            draw_poly_g3(x0, y0, p->r0, p->g0, p->b0,
                         x2, y2, p->r2, p->g2, p->b2,
                         x3, y3, p->r3, p->g3, p->b3);
        } else {
            draw_poly_g3(x0, y0, p->r0, p->g0, p->b0,
                         x1, y1, p->r1, p->g1, p->b1,
                         x2, y2, p->r2, p->g2, p->b2);
        }
    }
}

/* ============================================================
 * 3D SPINNING CUBE DEMO - Uses GTE functions for 3D transforms
 * ============================================================
 * Demonstrates the full PS1 3D pipeline:
 *   1. Define cube vertices in model space
 *   2. Set up rotation/translation matrices via GTE
 *   3. Transform vertices with RotTransPers (perspective projection)
 *   4. Draw faces as gouraud-shaded quads with depth sorting
 */

/* Cube vertices - 8 corners of a unit cube centered at origin */
static SVECTOR g_cube_verts[8] = {
    { -64, -64, -64, 0 },  /* 0: front-bottom-left */
    {  64, -64, -64, 0 },  /* 1: front-bottom-right */
    {  64,  64, -64, 0 },  /* 2: front-top-right */
    { -64,  64, -64, 0 },  /* 3: front-top-left */
    { -64, -64,  64, 0 },  /* 4: back-bottom-left */
    {  64, -64,  64, 0 },  /* 5: back-bottom-right */
    {  64,  64,  64, 0 },  /* 6: back-top-right */
    { -64,  64,  64, 0 },  /* 7: back-top-left */
};

/* Cube faces - 6 faces, each with 4 vertex indices (CCW winding) */
static int g_cube_faces[6][4] = {
    { 0, 1, 2, 3 },  /* front  (Z-) */
    { 5, 4, 7, 6 },  /* back   (Z+) */
    { 4, 0, 3, 7 },  /* left   (X-) */
    { 1, 5, 6, 2 },  /* right  (X+) */
    { 3, 2, 6, 7 },  /* top    (Y+) */
    { 4, 5, 1, 0 },  /* bottom (Y-) */
};

/* Face colors (gouraud corners will blend these) */
static u8 g_face_colors[6][3] = {
    { 255,   0,   0 },  /* front: red */
    {   0, 255,   0 },  /* back: green */
    {   0,   0, 255 },  /* left: blue */
    { 255, 255,   0 },  /* right: yellow */
    { 255,   0, 255 },  /* top: magenta */
    {   0, 255, 255 },  /* bottom: cyan */
};

static s16 g_cube_rot_x = 0;
static s16 g_cube_rot_y = 0;
static s16 g_cube_rot_z = 0;
static int g_3d_mode = 0;  /* 0 = off, 1 = spinning cube */

void gfxtest_toggle_3d(void) {
    g_3d_mode = !g_3d_mode;
    if (g_3d_mode) {
        /* Initialize GTE for 3D rendering */
        InitGeom();
        SetGeomOffset(160, 120);  /* Screen center */
        SetGeomScreen(256);       /* Projection distance (FOV) */

        /* Try to load a TMD model from game assets */
        if (!g_tmd_loaded) {
            /* TACO folder = shooting mini-game assets */
            if (!load_tmd("raw/CDROM/TACO/JIKI.TMD"))      /* player ship */
                if (!load_tmd("raw/CDROM/TACO/DRA.TMD"))   /* dragon */
                    if (!load_tmd("raw/CDROM/TACO/TAKO.TMD"))  /* octopus */
                        if (!load_tmd("raw/CDROM/TACO/SPI.TMD"))   /* spider */
                            load_tmd("raw/CDROM/TMD/COREOB00.TMD");
        }

        if (g_tmd_loaded) {
            set_status("3D MODE: %s (%d verts)", g_tmd_name, g_tmd_vert_count);
        } else {
            set_status("3D MODE: Spinning Cube (GTE)");
        }
    } else {
        set_status("3D MODE: Off");
    }
    debug_log("Graphics test: 3D mode %s, TMD=%s",
              g_3d_mode ? "ON" : "OFF", g_tmd_loaded ? g_tmd_name : "none");
}

/* Draw a spinning 3D cube using GTE transforms */
static void draw_3d_cube(void) {
    MATRIX rot_matrix;
    VECTOR trans_vec;
    SVECTOR rot_angles;
    s32 screen_xy[8];  /* Packed screen coords for 8 vertices */
    s32 screen_z[8];   /* Z depth for each vertex */
    s32 flag;
    int i, f;

    /* Face depth sorting */
    struct { int idx; s32 z; } face_order[6];

    /* Set up rotation angles (PS1 angle format: 4096 = 360 degrees) */
    rot_angles.vx = g_cube_rot_x;
    rot_angles.vy = g_cube_rot_y;
    rot_angles.vz = g_cube_rot_z;

    /* Build rotation matrix */
    RotMatrix(&rot_angles, &rot_matrix);

    /* Set translation (push cube away from camera) */
    trans_vec.vx = 0;
    trans_vec.vy = 0;
    trans_vec.vz = 400;  /* Distance from camera */
    TransMatrix(&rot_matrix, &trans_vec);

    /* Set GTE matrices */
    SetRotMatrix(&rot_matrix);
    SetTransMatrix(&rot_matrix);

    /* Transform all 8 vertices */
    for (i = 0; i < 8; i++) {
        s32 p;
        RotTransPers(&g_cube_verts[i], &screen_xy[i], &p, &flag);
        screen_z[i] = p;  /* Depth value */
    }

    /* Calculate average Z for each face for depth sorting */
    for (f = 0; f < 6; f++) {
        s32 avg_z = 0;
        for (i = 0; i < 4; i++) {
            avg_z += screen_z[g_cube_faces[f][i]];
        }
        face_order[f].idx = f;
        face_order[f].z = avg_z / 4;
    }

    /* Simple bubble sort by Z (furthest first = painter's algorithm) */
    for (i = 0; i < 5; i++) {
        for (f = 0; f < 5 - i; f++) {
            if (face_order[f].z < face_order[f+1].z) {
                int tmp_idx = face_order[f].idx;
                s32 tmp_z = face_order[f].z;
                face_order[f].idx = face_order[f+1].idx;
                face_order[f].z = face_order[f+1].z;
                face_order[f+1].idx = tmp_idx;
                face_order[f+1].z = tmp_z;
            }
        }
    }

    /* Draw faces back-to-front */
    for (i = 0; i < 6; i++) {
        int fi = face_order[i].idx;
        int *face = g_cube_faces[fi];
        u8 r = g_face_colors[fi][0];
        u8 g = g_face_colors[fi][1];
        u8 b = g_face_colors[fi][2];

        /* Extract screen coordinates */
        s16 x0 = (s16)(screen_xy[face[0]] & 0xFFFF);
        s16 y0 = (s16)(screen_xy[face[0]] >> 16);
        s16 x1 = (s16)(screen_xy[face[1]] & 0xFFFF);
        s16 y1 = (s16)(screen_xy[face[1]] >> 16);
        s16 x2 = (s16)(screen_xy[face[2]] & 0xFFFF);
        s16 y2 = (s16)(screen_xy[face[2]] >> 16);
        s16 x3 = (s16)(screen_xy[face[3]] & 0xFFFF);
        s16 y3 = (s16)(screen_xy[face[3]] >> 16);

        /* Backface culling via cross product (normal clip) */
        s32 nc = NormalClip(screen_xy[face[0]], screen_xy[face[1]], screen_xy[face[2]]);
        if (nc <= 0) continue;  /* Face pointing away */

        /* Draw as two gouraud triangles (quad split) with slight color variation */
        u8 r0 = r, g0 = g, b0 = b;
        u8 r1 = (u8)(r * 3 / 4), g1 = (u8)(g * 3 / 4), b1 = (u8)(b * 3 / 4);
        u8 r2 = (u8)(r / 2), g2 = (u8)(g / 2), b2 = (u8)(b / 2);
        u8 r3 = (u8)(r * 3 / 4 + 32), g3 = (u8)(g * 3 / 4 + 32), b3 = (u8)(b * 3 / 4 + 32);
        if (r3 < r) r3 = 255;
        if (g3 < g) g3 = 255;
        if (b3 < b) b3 = 255;

        /* Triangle 1: v0, v1, v2 */
        draw_poly_g3(x0, y0, r0, g0, b0,
                     x1, y1, r1, g1, b1,
                     x2, y2, r2, g2, b2);
        /* Triangle 2: v0, v2, v3 */
        draw_poly_g3(x0, y0, r0, g0, b0,
                     x2, y2, r2, g2, b2,
                     x3, y3, r3, g3, b3);
    }

    /* Animate rotation */
    g_cube_rot_x += 12;  /* ~1 degree per frame */
    g_cube_rot_y += 18;
    g_cube_rot_z += 6;
    if (g_cube_rot_x >= 4096) g_cube_rot_x -= 4096;
    if (g_cube_rot_y >= 4096) g_cube_rot_y -= 4096;
    if (g_cube_rot_z >= 4096) g_cube_rot_z -= 4096;
}

/* Temporarily override draw offset for gfxtest rendering */
extern s32 g_draw_x, g_draw_y;

void gfxtest_draw(void) {
    extern s32 g_disp_rgb24;
    s32 saved_disp_rgb24;
    if (!g_active) return;

    int n = g_tim_count;
    int cur = g_cursor;
    int top = cur - VIEW_ROWS / 2;
    if (top < 0) top = 0;
    if (top > n - VIEW_ROWS) top = n - VIEW_ROWS;
    if (top < 0) top = 0;

    /* Save and override draw offset to draw to visible buffer */
    s32 saved_draw_x = g_draw_x;
    s32 saved_draw_y = g_draw_y;
    g_draw_x = 0;  /* Draw to primary display buffer */
    g_draw_y = 0;

    /* Also ensure display reads from (0,0) */
    extern s32 g_disp_x, g_disp_y;
    s32 saved_disp_x = g_disp_x;
    s32 saved_disp_y = g_disp_y;
    saved_disp_rgb24 = g_disp_rgb24;
    g_disp_x = 0;
    g_disp_y = 0;
    g_disp_rgb24 = 0; /* Diagnostics draw BGR555, even over an RGB24 movie. */

    /* Host diagnostics draw eagerly without submitting SDK work or
     * spending game time. These aligned rectangles use the raw data plane. */
    gpu_vram_fill(&g_vram[0][0], 0x1e0a0au, 0, (240u << 16) | 320u);

    /* Header */
    gpu_vram_fill(&g_vram[0][0], 0x501e32u, 0, (18u << 16) | 320u);

    if (g_3d_mode) {
        if (g_tmd_loaded) {
            gt_text(6, 3, "TMD MODEL", 255, 200, 100);
            gt_text(160, 3, g_tmd_name, 180, 255, 180);

            /* Draw the spinning TMD model - push camera back to see full model */
            draw_tmd_model(g_cube_rot_x, g_cube_rot_y, g_cube_rot_z, 1500);

            /* Animate rotation */
            g_cube_rot_x += 8;
            g_cube_rot_y += 12;
            g_cube_rot_z += 4;
            if (g_cube_rot_x >= 4096) g_cube_rot_x -= 4096;
            if (g_cube_rot_y >= 4096) g_cube_rot_y -= 4096;
            if (g_cube_rot_z >= 4096) g_cube_rot_z -= 4096;

            /* Info text */
            gt_text(6, 200, "F7 EXIT", 150, 150, 200);
            {
                char info[64];
                snprintf(info, sizeof(info), "%dV %dP", g_tmd_vert_count, g_tmd_prim_count);
                gt_text(6, 212, info, 100, 200, 255);
            }
        } else {
            gt_text(6, 3, "3D CUBE DEMO", 255, 200, 100);
            gt_text(200, 3, "GTE", 180, 255, 180);

            /* Draw the spinning 3D cube */
            draw_3d_cube();

            /* Info text */
            gt_text(6, 200, "F7 EXIT 3D", 150, 150, 200);
            gt_text(6, 212, "GTE TRANSFORMS", 100, 200, 255);
        }
    } else {
        gt_text(6, 3, "GRAPHICS TEST", 255, 200, 100);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "F7 3D");
            gt_text(200, 3, buf, 180, 220, 255);
        }

        /* Draw all primitive types for comprehensive testing */
        draw_all_primitives();
    }

    /* Status bar */
    gpu_vram_fill(&g_vram[0][0], 0x281414u, 218u << 16,
                   (22u << 16) | 320u);
    gt_text(6, 223, g_status, 200, 255, 200);

    /* Auto-screenshot on first few frames for debugging */
    {
        static int shot_frame = 0;
        static int shot_3d_frame = 0;
        if (!g_3d_mode && shot_frame < 3) {
            shot_frame++;
            if (shot_frame == 2) {
                debug_screenshot("gfxtest_screenshot.ppm");
                debug_log("Graphics test: auto-screenshot saved");
            }
        }
        /* Save 3D screenshots at several rotation angles */
        if (g_3d_mode && shot_3d_frame < 60) {
            shot_3d_frame++;
            if (shot_3d_frame == 5) {
                debug_screenshot("gfxtest_3d_cube_1.ppm");
                debug_log("Graphics test: 3D cube screenshot 1 saved");
            }
            if (shot_3d_frame == 30) {
                debug_screenshot("gfxtest_3d_cube_2.ppm");
                debug_log("Graphics test: 3D cube screenshot 2 saved (rotated)");
            }
        }
    }

    /* Scanout now happens at presentation, not DrawSync. Present while
     * the debug origin is selected; this host-only loop owns its pacing. */
    {
        extern void hal_debug_frame_end(void);
        hal_debug_frame_end();
    }

    /* Restore offsets for game rendering */
    g_draw_x = saved_draw_x;
    g_draw_y = saved_draw_y;
    g_disp_x = saved_disp_x;
    g_disp_y = saved_disp_y;
    g_disp_rgb24 = saved_disp_rgb24;
}
