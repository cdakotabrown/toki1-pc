/* SPDX-License-Identifier: GPL-3.0-or-later */
/* present_layout.c -- SDL-free presenter pieces (see pc/include/pc_present.h):
 * layout math, window-shot names, a small PNG writer and the notes queue.
 * Unit-tested by pc/tests/test_settings.c (the notes: pc/tests/
 * present_notes_harness.c, pc/tools/test_state_machines.py). */
#include "pc_present.h"
#include "pc_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PresentRect present_layout_rect(int w, int h, int scale_mode) {
    PresentRect r;
    double want = (double)PRESENT_SRC_W / PRESENT_SRC_H;
    double real, scale;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    real = (double)w / h;
    if (scale_mode == PC_SCALE_STRETCH) {
        r.x = r.y = 0;
        r.w = w;
        r.h = h;
        return r;
    }
    if (scale_mode == PC_SCALE_INTEGER) {
        /* SDL2 UpdateLogicalSize, integer_scale branch. */
        int k = want > real ? w / PRESENT_SRC_W : h / PRESENT_SRC_H;
        if (k < 1) k = 1;
        r.w = PRESENT_SRC_W * k;
        r.h = PRESENT_SRC_H * k;
        r.x = (w - r.w) / 2;
        r.y = (h - r.h) / 2;
        return r;
    }
    if (fabs(want - real) < 0.0001) {
        r.x = r.y = 0;
        r.w = w;
        r.h = h;
    } else if (want > real) {           /* window taller: letterbox */
        scale = (double)w / PRESENT_SRC_W;
        r.x = 0;
        r.w = w;
        r.h = (int)floor(PRESENT_SRC_H * scale);
        r.y = (h - r.h) / 2;
    } else {                            /* window wider: pillarbox */
        scale = (double)h / PRESENT_SRC_H;
        r.y = 0;
        r.h = h;
        r.w = (int)floor(PRESENT_SRC_W * scale);
        r.x = (w - r.w) / 2;
    }
    return r;
}

int present_window_to_frame(double x, double y, int win_w, int win_h,
                            int out_w, int out_h, int scale_mode,
                            double *fx, double *fy) {
    PresentRect r;
    double px, py;
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;
    if (out_w < 1) out_w = win_w;
    if (out_h < 1) out_h = win_h;
    px = x * out_w / win_w;                 /* window points -> output pixels */
    py = y * out_h / win_h;
    r = present_layout_rect(out_w, out_h, scale_mode);
    *fx = (px - r.x) * PRESENT_SRC_W / r.w;
    *fy = (py - r.y) * PRESENT_SRC_H / r.h;
    return *fx >= 0.0 && *fx < PRESENT_SRC_W && *fy >= 0.0 && *fy < PRESENT_SRC_H;
}

void present_auto_window(int uw, int uh, int *w, int *h) {
    int k = 2;
    if (uw > 0 && uh > 0) {
        int kx = (int)(uw * 0.75) / PRESENT_SRC_W;
        int ky = (int)(uh * 0.75) / PRESENT_SRC_H;
        k = kx < ky ? kx : ky;
        if (k < 1) k = 1;
    }
    *w = PRESENT_SRC_W * k;
    *h = PRESENT_SRC_H * k;
}

void present_scanline_band(PresentRect r, int row, int *y, int *h) {
    int top = r.y + (int)((long)row * r.h / PRESENT_SRC_H);
    int bot = r.y + (int)((long)(row + 1) * r.h / PRESENT_SRC_H);
    int half = (bot - top) / 2;
    if (half < 1) half = 1;
    *y = bot - half;
    *h = half;
}

int present_volume_step(int v, int delta) {
    v += delta;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

int present_menu_note_room(int right, int end, int is_page, int *note_right) {
    int r = is_page ? right - PRESENT_MENU_ARROW_W : right;
    if (note_right) *note_right = r;
    return r - end - 10;
}

/* PC_PRESENT_WINDOW_SHOT's file for the n-th shot: the pattern with its
 * first "%d" replaced by n (several shots in a run, one file each), or the
 * pattern itself (every shot overwrites the one file, as before).  Any
 * other '%' is copied as it is.  0 on success, -1 if it does not fit. */
int present_shot_path(const char *pattern, unsigned n, char *out, size_t cap) {
    const char *d = strstr(pattern, "%d");
    int k;
    if (!d) k = snprintf(out, cap, "%s", pattern);
    else k = snprintf(out, cap, "%.*s%u%s", (int)(d - pattern), pattern, n, d + 2);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

/* ---- PNG ---- */
static unsigned long g_crc_table[256];

static unsigned long crc_update(unsigned long crc, const unsigned char *p, size_t n) {
    size_t i;
    if (!g_crc_table[1]) {
        unsigned long c;
        int k, j;
        /* entry 1 last: it is the "built" flag, and the quicksave writer
         * thread (pc/hal/quicksave.c) may get here at the same time */
        for (k = 256; k-- > 0;) {
            c = (unsigned long)k;
            for (j = 0; j < 8; j++) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            g_crc_table[k] = c;
        }
    }
    for (i = 0; i < n; i++) crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put32(unsigned char *b, unsigned long v) {
    b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
}

static int chunk(FILE *f, const char *type, const unsigned char *data, size_t n,
                 unsigned long *crc_out) {
    unsigned char hdr[8];
    unsigned long crc;
    put32(hdr, (unsigned long)n);
    memcpy(hdr + 4, type, 4);
    crc = crc_update(0xFFFFFFFFUL, hdr + 4, 4);
    if (data && n) crc = crc_update(crc, data, n);
    if (crc_out) { *crc_out = crc; return 0; }
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    put32(hdr, crc ^ 0xFFFFFFFFUL);
    return fwrite(hdr, 1, 4, f) == 4 ? 0 : -1;
}

int present_png_write(const char *path, const unsigned char *rgb, int w, int h) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    unsigned char ihdr[13], b4[4];
    size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h;
    size_t blocks = (raw + 65534) / 65535, idat_len = 2 + raw + blocks * 5 + 4, done = 0;
    unsigned long a = 1, b = 0, crc;
    size_t y, i;
    FILE *f;
    if (w <= 0 || h <= 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    put32(ihdr, (unsigned long)w);
    put32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (fwrite(sig, 1, 8, f) != 8 || chunk(f, "IHDR", ihdr, 13, NULL)) goto fail;

    /* IDAT streamed: zlib header, stored blocks over the filtered rows. */
    put32(b4, (unsigned long)idat_len);
    if (fwrite(b4, 1, 4, f) != 4 || fwrite("IDAT", 1, 4, f) != 4) goto fail;
    crc = crc_update(0xFFFFFFFFUL, (const unsigned char *)"IDAT", 4);
    {
        unsigned char zh[2] = { 0x78, 0x01 };
        fwrite(zh, 1, 2, f);
        crc = crc_update(crc, zh, 2);
    }
    y = 0; i = 0;   /* position: row y, byte i within the filtered row */
    while (done < raw) {
        size_t n = raw - done > 65535 ? 65535 : raw - done, k;
        unsigned char bh[5];
        bh[0] = done + n == raw ? 1 : 0;
        bh[1] = (unsigned char)n; bh[2] = (unsigned char)(n >> 8);
        bh[3] = (unsigned char)~n; bh[4] = (unsigned char)(~n >> 8);
        fwrite(bh, 1, 5, f);
        crc = crc_update(crc, bh, 5);
        for (k = 0; k < n; k++) {
            unsigned char v = i == 0 ? 0 : rgb[y * (size_t)w * 3 + i - 1];
            fputc(v, f);
            crc = crc_update(crc, &v, 1);
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            if (++i == row) { i = 0; y++; }
        }
        done += n;
    }
    put32(b4, (b << 16) | a);
    fwrite(b4, 1, 4, f);
    crc = crc_update(crc, b4, 4);
    put32(b4, crc ^ 0xFFFFFFFFUL);
    if (fwrite(b4, 1, 4, f) != 4) goto fail;
    if (chunk(f, "IEND", NULL, 0, NULL)) goto fail;
    return fclose(f) ? -1 : 0;
fail:
    fclose(f);
    return -1;
}

/* The reader for the thumbnails the writer above makes (the save browser,
 * pc/hal/save_browser.c): PNG signature, IHDR W x H 8-bit RGB, the IDAT
 * chunks concatenated, a zlib header, stored blocks only, every row's
 * filter byte 0.  CRCs and the Adler sum are not checked; a file of any
 * other shape is refused rather than decoded wrongly. */
static unsigned long get32(const unsigned char *b) {
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
           ((unsigned long)b[2] << 8) | b[3];
}

int present_png_read(const char *path, unsigned char *rgb, int w, int h) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    unsigned char hdr[8], *z = NULL;
    size_t zn = 0, zcap = 0, row = (size_t)w * 3 + 1, raw = row * (size_t)h, got = 0, p;
    int ok = 0, have_ihdr = 0;
    FILE *f;
    if (w <= 0 || h <= 0) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, sig, 8)) goto out;
    for (;;) {
        unsigned long n;
        if (fread(hdr, 1, 8, f) != 8) goto out;
        n = get32(hdr);
        if (n > (1ul << 24)) goto out;
        if (!memcmp(hdr + 4, "IHDR", 4)) {
            unsigned char ih[13];
            if (n != 13 || fread(ih, 1, 13, f) != 13) goto out;
            if (get32(ih) != (unsigned long)w || get32(ih + 4) != (unsigned long)h ||
                ih[8] != 8 || ih[9] != 2 || ih[10] || ih[11] || ih[12])
                goto out;
            have_ihdr = 1;
            if (fseek(f, 4, SEEK_CUR)) goto out;
        } else if (!memcmp(hdr + 4, "IDAT", 4)) {
            if (zn + n > zcap) {
                unsigned char *q;
                zcap = (zn + n) * 2;
                q = (unsigned char *)realloc(z, zcap);
                if (!q) goto out;
                z = q;
            }
            if (fread(z + zn, 1, n, f) != n) goto out;
            zn += n;
            if (fseek(f, 4, SEEK_CUR)) goto out;
        } else if (!memcmp(hdr + 4, "IEND", 4)) {
            break;
        } else {
            if (fseek(f, (long)n + 4, SEEK_CUR)) goto out;
        }
    }
    if (!have_ihdr || zn < 2 || (z[0] & 0x0F) != 8 || (z[1] & 0x20)) goto out;
    p = 2;
    for (;;) {
        size_t len, k;
        int final;
        if (p + 5 > zn || (z[p] & 0x06)) goto out;          /* BTYPE 00 only */
        final = z[p] & 1;
        len = (size_t)z[p + 1] | ((size_t)z[p + 2] << 8);
        if ((len ^ 0xFFFF) != ((size_t)z[p + 3] | ((size_t)z[p + 4] << 8))) goto out;
        p += 5;
        if (p + len > zn || got + len > raw) goto out;
        for (k = 0; k < len; k++, got++) {
            size_t y = got / row, i = got % row;
            if (i == 0) { if (z[p + k]) goto out; continue; }
            rgb[y * (size_t)w * 3 + i - 1] = z[p + k];
        }
        p += len;
        if (final) break;
    }
    ok = got == raw;
out:
    free(z);
    fclose(f);
    return ok ? 0 : -1;
}

/* ---- notes (pc/include/pc_present.h) ---- */

int present_notes_push(PresentNotes *q, const char *text, unsigned ms) {
    int i;
    PresentNote *e;
    for (i = 0; i < q->n; i++)
        if (!strcmp(q->q[(q->head + i) % PRESENT_NOTES].text, text)) return 1;
    if (q->n == PRESENT_NOTES) {
        q->dropped++;
        return -1;
    }
    e = &q->q[(q->head + q->n) % PRESENT_NOTES];
    snprintf(e->text, sizeof e->text, "%s", text);
    e->left = ms ? ms : 1;
    q->n++;
    return 0;
}

const char *present_notes_show(PresentNotes *q, unsigned now, int visible) {
    if (!visible || !q->n) {
        q->shown = 0;               /* covered: its time stands still */
        return NULL;
    }
    if (q->shown) {
        unsigned dt = now - q->last;
        PresentNote *e = &q->q[q->head];
        if (dt >= e->left) {
            q->head = (q->head + 1) % PRESENT_NOTES;
            q->n--;
            q->shown = 0;
            if (!q->n) return NULL;
        } else {
            e->left -= dt;
        }
    }
    q->shown = 1;
    q->last = now;
    return q->q[q->head].text;
}
