/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backlog_harness.c -- the dialogue backlog's capture, list and voice
 * decode against a synthetic text engine, font and XA file (no game data).
 * Linked with pc/hal/backlog_capture.c, backlog.c, voice_replay.c,
 * cd_audio.c, profile.c and menu.c (pc/tools/test_backlog.py builds it).
 *
 *   backlog_harness font       every glyph index -> its canonical code
 *   backlog_harness capture    a scripted conversation: what is captured
 *   backlog_harness list       the 200-page cap and the view's input
 *   backlog_harness decode     replay decode == the controller's PCM
 *   backlog_harness decode-asset PATH FILE_LBA FIRST LAST FILE CHANNEL [OUT]
 *                              the same over a real disc asset (OUT: the
 *                              replay's PCM, s16le stereo)
 */
#include "types.h"
#include "pc_backlog.h"
#include "cd_audio.h"
#include "file_id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the text engine's globals (PS1 RAM in the game) ---- */
s16 check_end_k_tbl0, check_end_k_tbl1, disp_string_state;
u8 k_cell_w = 12;
u8 yazirushi_cw_disp, now_step, now_sub_step, now_sub_sub_step, now_girl;
s32 frame_counter;
s32 XAsector[4];
u16 g_vram[512][1024];
typedef struct { s16 x, y; u8 tp, count, pal; s8 flag; } TextRec;
TextRec disp_string_tbl0[52];
#define NGLYPH 8000
u8 melt_font_tbl0[NGLYPH * 18];
u16 font_pair[2] __asm__("load_string_tbl0");
u16 load_string_tbl1[92];

/* ---- the file table: one voice file at LBA 1000 ---- */
const FileLbaEntry g_file_lba_table[] = {
    { "CDROM/XA_G/TEST.XA", 1000, 2048 * 400 },
    { "CDROM/XA_G/BGM378.XA", 5000, 2048 * 400 },
};
const u32 g_file_lba_table_count = 2;

/* cd_audio.c's output hook */
static int (*g_source)(s16 *, s16 *);
void spu_psx_set_cd_audio_source(int (*source)(s16 *, s16 *)) { g_source = source; }

/* ---------------------------------------------------------------- font */

/* A font laid out like the game's: overlapping index spans, unused trail
 * bytes inside the code spans.  Range k: (start, base). */
static const u16 RANGES[][2] = {
    { 0x8180, 63 }, { 0x824f, 108 }, { 0x8260, 118 }, { 0x8281, 144 }, { 0x829f, 170 },
};
static void font_init(void)
{
    int k, i;
    font_pair[0] = 0x8140;
    font_pair[1] = 0;
    for (k = 0; k < 5; k++) {
        load_string_tbl1[2 * k] = RANGES[k][0];
        load_string_tbl1[2 * k + 1] = RANGES[k][1];
    }
    for (k = 5; k < 44; k++) {          /* 0x8340.. one row of 0x40 each */
        load_string_tbl1[2 * k] = (u16)(0x8340 + 0x100 * (k - 5));
        load_string_tbl1[2 * k + 1] = (u16)(253 + 0xBD * (k - 5));
    }
    load_string_tbl1[88] = (u16)(0x8340 + 0x100 * 39);   /* the end */
    for (i = 0; i < NGLYPH; i++) {      /* a distinct bitmap per index */
        u32 h = 2166136261u ^ (u32)i;
        int b;
        for (b = 0; b < 18; b++) {
            h = h * 16777619u + 0x9E37u;
            melt_font_tbl0[i * 18 + b] = (u8)(h >> 13);
        }
    }
}

/* the canonical code of glyph index idx (the range owning it), 0 none */
static u16 canonical(int idx)
{
    u32 start, base, end, nbase;
    int k;
    for (k = 44; k >= 0; k--) {
        start = k ? load_string_tbl1[2 * (k - 1)] : font_pair[0];
        base = k ? load_string_tbl1[2 * (k - 1) + 1] : font_pair[1];
        end = load_string_tbl1[2 * k];
        nbase = k < 44 ? load_string_tbl1[2 * k + 1] : 99999;
        if ((u32)idx >= base) {
            u32 c = start + ((u32)idx - base);
            u32 t = c & 0xFF;
            if (c >= end || (u32)idx >= nbase) return 0;
            return (t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC) ? (u16)c : 0;
        }
    }
    return 0;
}

static int mode_font(void)
{
    int idx, n = 0;
    u8 bits[18];
    for (idx = 0; idx < 253 + 0xBD * 39; idx++) {
        u16 want = canonical(idx), got = backlog_font_code(&melt_font_tbl0[idx * 18]);
        if (!want) continue;
        if (got != want) {
            printf("FAIL idx %d: code %04x, want %04x\n", idx, got, want);
            return 1;
        }
        if (!backlog_font_glyph(want, bits) || memcmp(bits, &melt_font_tbl0[idx * 18], 18)) {
            printf("FAIL glyph %04x\n", want);
            return 1;
        }
        n++;
    }
    printf("font ok %d\n", n);
    return 0;
}

/* ---------------------------------------------------------------- engine */

static int g_loaded;                    /* glyph cache cells used */

/* vload_font: glyph `code` into cache cell i (16x12 4-bit, value 1 = ink) */
static void load_cell(int i, u16 code)
{
    int x0 = (i < 128 ? 800 : i < 256 ? 864 : 928) + (i & 7) * 4, y0 = 256 + ((i & 0x78) << 1);
    u8 bits[18];
    int r, c;
    if (!backlog_font_glyph(code, bits)) abort();
    for (r = 0; r < 12; r++)
        for (c = 0; c < 16; c++) {
            u16 *hw = &g_vram[y0 + r][x0 + c / 4];
            int bit = r * 12 + c, on = c < 12 && (bits[bit >> 3] & (0x80 >> (bit & 7)));
            *hw = (u16)((*hw & ~(15 << ((c & 3) * 4))) | (on << ((c & 3) * 4)));
        }
}

/* set_kanji_string: a string record, its glyphs loaded */
static void set_string(s16 x, s16 y, u8 tp, const u16 *codes, int n)
{
    TextRec *t = &disp_string_tbl0[disp_string_state++];
    int k;
    t->x = x; t->y = y; t->tp = tp; t->count = (u8)n; t->pal = 0; t->flag = 1;
    for (k = 0; k < n; k++) load_cell(g_loaded++, codes[k]);
    check_end_k_tbl0 = (s16)g_loaded;
}

/* k_reset(n): keep the first n strings */
static void k_reset(int n)
{
    int i, sum = 0;
    for (i = 0; i < n; i++) sum += disp_string_tbl0[i].count;
    disp_string_state = (s16)n;
    check_end_k_tbl0 = check_end_k_tbl1 = (s16)sum;
    g_loaded = sum;
}

static void frame(void)
{
    frame_counter++;
    backlog_capture_frame();
}

/* type the page out, one glyph a frame */
static void type_out(void)
{
    while (check_end_k_tbl1 < check_end_k_tbl0) {
        check_end_k_tbl1++;
        frame();
    }
    frame();
}

static u8 g_sector[CD_AUDIO_RAW_SECTOR];
/* set_XAsector(chan << 24 | k, 1 << 24 | k + n - 1, girl 0) and the drive
 * reading the line: `n` sectors of channel `chan`, 32 apart, from
 * 1000 + chan + 32 k (the file is at LBA 1000) */
static void voice(u32 k, int chan, int n)
{
    u32 lba = 1000u + (u32)chan + 32u * k;
    int i;
    XAsector[0] = 0;
    XAsector[1] = 1000;
    XAsector[2] = (s32)((u32)chan << 24 | k);
    XAsector[3] = (s32)(1u << 24 | (k + (u32)n - 1u));
    backlog_xa_reset();
    for (i = 0; i < n; i++) {
        memset(g_sector, 0, sizeof g_sector);
        g_sector[0] = 1;
        g_sector[1] = (u8)chan;
        g_sector[2] = 0x64;
        g_sector[3] = 0x04;
        backlog_xa_sector(lba + 32u * (u32)i, g_sector, 0);
    }
    backlog_xa_reset();
}

static void show_pages(void)
{
    int i, s, k;
    for (i = 0; i < backlog_count(); i++) {
        const BacklogPage *p = backlog_page(i);
        printf("page %u screen=%06x speaker=%d voice=%d", p->serial, p->screen, p->speaker,
               p->has_voice);
        if (p->has_voice)
            printf(" lba=%u..%u req=%u..%u chan=%u", p->voice.lba_first, p->voice.lba_last,
                   p->voice.req_first, p->voice.req_last, p->voice.channel);
        printf(" segs=");
        for (s = 0; s < p->nseg; s++) {
            printf("%s%d,%d:", s ? "/" : "", p->seg[s].x, p->seg[s].y);
            for (k = 0; k < p->seg[s].count; k++) printf("%04x", p->g[p->seg[s].first + k].code);
        }
        printf("\n");
    }
}

static const u16 MENU1[] = { 0x8341, 0x8342 }, MENU2[] = { 0x8343 }, MENU3[] = { 0x8344, 0x8345 };
static const u16 NAME[] = { 0x82a0, 0x82a2 }, BRACKET[] = { 0x8175, 0x8441 }, LINE[] = { 0x8442, 0x8443, 0x8144 };
static const u16 GIRL[] = { 0x8541, 0x8542 }, NARR[] = { 0x8641, 0x8642, 0x8643 };

static int mode_capture(void)
{
    int f;
    now_step = 0x45;
    now_sub_step = 1;
    now_sub_sub_step = 1;
    /* a menu: three strings, complete, no arrow, no speaker */
    set_string(-128, -84, 2, MENU1, 2);
    set_string(-136, -56, 2, MENU2, 1);
    set_string(40, -24, 2, MENU3, 2);
    type_out();
    for (f = 0; f < 30; f++) frame();
    /* page 1: the girl, voiced; the line starts before the text */
    voice(3, 3, 6);
    frame();
    set_string(-121, 50, 1, GIRL, 2);
    set_string(-86, 50, 1, BRACKET, 2);
    set_string(-72, 66, 1, LINE, 3);
    type_out();
    yazirushi_cw_disp |= 0x80;          /* serifu_main: page and voice done */
    for (f = 0; f < 80; f++) {          /* the arrow blinks */
        if (f % 16 == 0) yazirushi_cw_disp ^= 0x80;
        frame();
    }
    yazirushi_cw_disp &= 0x7F;
    /* some other line plays before the next page's reset (not its voice) */
    voice(9, 5, 2);
    for (f = 0; f < 5; f++) frame();
    /* page 2: the player, confirmed during the page: no arrow, then the reset */
    k_reset(3);
    frame();
    set_string(-121, 50, 1, NAME, 2);
    set_string(-86, 50, 1, BRACKET, 2);
    type_out();
    for (f = 0; f < 3; f++) frame();
    /* page 3: narration taken down without an arrow -- not dialogue */
    k_reset(3);
    frame();
    set_string(-128, 50, 1, NARR, 3);
    type_out();
    k_reset(3);
    frame();
    /* page 4: narration that waits on the arrow */
    set_string(-128, 50, 1, NARR, 2);
    type_out();
    yazirushi_cw_disp |= 0x80;
    for (f = 0; f < 5; f++) frame();
    yazirushi_cw_disp &= 0x7F;
    /* the same page shown again on the same screen: not a second entry */
    k_reset(3);
    frame();
    set_string(-128, 50, 1, NARR, 2);
    type_out();
    yazirushi_cw_disp |= 0x80;
    frame();
    show_pages();
    return 0;
}

/* ---------------------------------------------------------------- list */

static int mode_list(void)
{
    static BacklogPage p;
    int i;
    memset(&p, 0, sizeof p);
    p.nglyph = 1;
    p.nseg = 1;
    p.seg[0].count = 1;
    for (i = 0; i < 205; i++) backlog_add(&p);
    printf("count=%d first=%u last=%u\n", backlog_count(), backlog_page(0)->serial,
           backlog_page(backlog_count() - 1)->serial);
    backlog_open();
    printf("open=%d sel=%d\n", backlog_is_open(), backlog_selected());
    backlog_input(BACKLOG_UP);
    backlog_input(BACKLOG_UP);
    backlog_input(BACKLOG_UP);
    printf("up3=%d\n", backlog_selected());
    backlog_input(BACKLOG_PAGE_DOWN);
    printf("pgdn=%d\n", backlog_selected());
    backlog_input(BACKLOG_HOME);
    printf("home=%d\n", backlog_selected());
    backlog_input(BACKLOG_PAGE_UP);
    printf("pgup=%d\n", backlog_selected());
    backlog_input(BACKLOG_OK);            /* no voice: a note, nothing plays */
    printf("playing=%d\n", voice_replay_active());
    backlog_input(BACKLOG_BACK);
    printf("open=%d\n", backlog_is_open());
    return 0;
}

/* ---------------------------------------------------------------- decode */

static s16 *g_ctrl;
static int g_nctrl, g_capctrl;

static void pop_all(void)
{
    s16 l, r;
    while (g_source(&l, &r)) {
        if (g_nctrl < g_capctrl) {
            g_ctrl[g_nctrl * 2] = l;
            g_ctrl[g_nctrl * 2 + 1] = r;
            g_nctrl++;
        }
    }
}

/* The controller over the same sectors, filter on: the PCM it hands the SPU. */
static int controller_pcm(const BacklogVoice *v, const char *root)
{
    char path[600];
    u8 raw[CD_AUDIO_RAW_SECTOR];
    FILE *f;
    u32 lba;
    snprintf(path, sizeof path, "%s/%s", root, v->path);
    f = fopen(path, "rb");
    if (!f) return -1;
    cd_audio_init();
    cd_audio_reset_decoder(0);
    cd_audio_set_filter(v->file, v->channel);
    for (lba = v->req_first; lba <= v->req_last; lba++) {
        fseek(f, (long)(lba - v->file_lba) * CD_AUDIO_RAW_SECTOR, SEEK_SET);
        if (fread(raw, 1, sizeof raw, f) != sizeof raw) break;
        if ((raw[2] & 0x44) != 0x44) continue;
        cd_audio_xa_sector(raw, 1, 0);
        pop_all();
        if (raw[0] == v->file && raw[1] == v->channel && (raw[2] & 0x80)) break;
    }
    fclose(f);
    return g_nctrl;
}

static const char *g_pcm_out;           /* decode-asset: write the replay PCM here */

static int compare(const BacklogVoice *v, const char *root)
{
    static s16 rep[44100 * 60 * 2];
    int n, m, i;
    g_capctrl = 44100 * 60;
    g_ctrl = (s16 *)malloc((size_t)g_capctrl * 4);
    n = voice_replay_decode(v, root, rep, 44100 * 60);
    m = controller_pcm(v, root);
    printf("replay %d frames, controller %d frames\n", n, m);
    if (n <= 0 || n != m) return 1;
    for (i = 0; i < n * 2; i++)
        if (rep[i] != g_ctrl[i]) {
            printf("differ at sample %d: %d %d\n", i, rep[i], g_ctrl[i]);
            return 1;
        }
    printf("identical\n");
    if (g_pcm_out) {
        FILE *o = fopen(g_pcm_out, "wb");
        if (!o || fwrite(rep, 4, (size_t)n, o) != (size_t)n) return 1;
        fclose(o);
    }
    return 0;
}

/* A file of 64 sectors, channels 0..3 interleaved (lba % 4), 18.9 kHz
 * mono 4-bit ADPCM with random sound groups; channel 2's last sector EOF. */
static int mode_decode(void)
{
    char dir[] = "/tmp/backlogXXXXXX", path[600];
    FILE *f;
    BacklogVoice v;
    u8 raw[CD_AUDIO_RAW_SECTOR];
    u32 seed = 12345;
    int i, b, rc;
    if (!mkdtemp(dir)) return 2;
    snprintf(path, sizeof path, "%s/CDROM", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/CDROM/XA_G", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/CDROM/XA_G/TEST.XA", dir);
    f = fopen(path, "wb");
    for (i = 0; i < 64; i++) {
        memset(raw, 0, sizeof raw);
        raw[0] = 1;
        raw[1] = (u8)(i % 4);
        raw[2] = 0x64 | (i == 62 ? 0x80 : 0);
        raw[3] = 0x04;
        for (b = 8; b < 8 + 18 * 128; b++) {
            seed = seed * 1103515245u + 12345u;
            raw[b] = (u8)(seed >> 16);
        }
        fwrite(raw, 1, sizeof raw, f);
    }
    fclose(f);
    memset(&v, 0, sizeof v);
    strcpy(v.path, "CDROM/XA_G/TEST.XA");
    v.file_lba = 1000;
    v.req_first = 1000;
    v.req_last = 1063;
    v.file = 1;
    v.channel = 2;
    rc = compare(&v, dir);
    unlink(path);
    return rc;
}

int main(int argc, char **argv)
{
    setenv("TOKI_XA_MARGIN_MS", "0", 1);
    font_init();
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "font")) return mode_font();
    if (!strcmp(argv[1], "capture")) return mode_capture();
    if (!strcmp(argv[1], "list")) return mode_list();
    if (!strcmp(argv[1], "decode")) return mode_decode();
    if (!strcmp(argv[1], "decode-asset") && (argc == 8 || argc == 9)) {
        BacklogVoice v;
        memset(&v, 0, sizeof v);
        snprintf(v.path, sizeof v.path, "%s", argv[2]);
        v.file_lba = (u32)atol(argv[3]);
        v.req_first = (u32)atol(argv[4]);
        v.req_last = (u32)atol(argv[5]);
        v.file = (u8)atoi(argv[6]);
        v.channel = (u8)atoi(argv[7]);
        if (argc == 9) g_pcm_out = argv[8];
        return compare(&v, "pc/assets/raw");
    }
    return 2;
}
