/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backlog_capture.c -- the dialogue backlog's capture (pc/include/pc_backlog.h).
 *
 * READ ONLY.  Nothing here writes game memory, VRAM, the drive or the SPU;
 * it observes the matched text engine once per game frame (hal_frame_end)
 * and the XA sectors the drive model reads, and hands completed pages to
 * pc/hal/backlog.c.  Off (no reads at all) unless TOKI_BACKLOG is on --
 * the enhanced profile's default, never parity's.
 *
 * When is a page captured.  The test of the auto-advance assist
 * (pc/hal/enhance.c): the text engine's page counters agree
 * (check_end_k_tbl0, glyphs loaded == check_end_k_tbl1, glyphs shown) and
 * the "next page" arrow (yazirushi_cw_disp bit 0x80) was up within the last
 * ARROW_GAP frames on this screen.  serifu_main raises the arrow only once
 * the page AND its voice line have ended; the TEL calls raise it through
 * yazirushi_disp_switch; result screens blink it.  A page whose wait never
 * came -- the player confirmed during the voice -- is captured when it is
 * taken down, if it has the speech layout (below).  Once each: not again
 * for a blinking arrow, nor when the same page is shown again on the
 * same screen.  The rules are at backlog_capture_frame.
 *
 * What a page is.  Exactly what disp_string (src/main/matched/disp_string.c)
 * draws: it walks the string table disp_string_tbl0 (52 records: x, y,
 * CLUT, glyph count, flag) and glyph slot i (0 .. check_end_k_tbl0) of the
 * font cache that vload_font fills in VRAM (16x12 4-bit cells at 800/864/
 * 928, 256 + 16 * row).  The capture copies each shown glyph's 12x12 bits
 * out of VRAM -- the game's own font, as the game loaded it -- with its
 * position and its string's CLUT colour.  The Shift-JIS code is recovered
 * by matching the bits against the game's font table in RAM
 * (melt_font_tbl0, 18 bytes a glyph, indexed through load_string_tbl1's
 * code ranges, the way load_string looks a code up), so the backlog holds
 * the text as codes as well as pixels.  Speaker: the name string left of
 * the opening bracket at x = -86 (find_speaker); `girl` is now_girl.
 *
 * The voice.  The drive model calls backlog_xa_sector for every realtime
 * ADPCM sector it reads; the sectors the controller takes into its stream
 * (cd_audio_accepts: filter, current-file lock) from a girl's voice file
 * (CDROM/XA_G/, not the streamed BGM378.XA) make the stream, which ends at
 * a decoder reset (Pause, a new read, a seek) or an EOF sector.  A page
 * takes the newest stream not yet given to a page that started since the
 * page's text did (its reset): serifu_cmd_exec starts the line before it
 * prints the page (and waits for the drive), so the stream starts just
 * before the text.  The stream also records the whole line as the game
 * requested it (request_window), for a replay of a line cut short.
 *
 * Machine state: a quickload restores these statics with the machine
 * (pc/hal/dbg_state.c), so the detector stays in step with the game it
 * watches; the session's list (backlog.c) is kept.
 */
#include "types.h"
#include "profile.h"
#include "pc_backlog.h"
#include "file_id.h"
#include "pc_settings.h"
#include "lang.h"
#include <string.h>

extern s16 check_end_k_tbl0;
extern s16 check_end_k_tbl1;
extern s16 disp_string_state;              /* strings in disp_string_tbl0 */
extern u8  k_cell_w;              /* glyph advance */
extern u8  yazirushi_cw_disp;
extern u8  now_step, now_sub_step, now_sub_sub_step, now_girl;
extern s32 frame_counter;
extern u8  melt_font_tbl0[];
extern u16 load_string_tbl0;
extern u16 load_string_tbl1[];
extern u16 g_vram[512][1024];
extern u32 hal_clock_vblanks(void) __attribute__((weak));
/* pc/hal/cd_audio.c; weak so harnesses can link this file without it */
extern int cd_audio_accepts(const u8 *raw, int filter) __attribute__((weak));

typedef struct {
    s16 x, y;
    u8 tp, count, pal;
    s8 flag;
} TextRec;                          /* disp_string's IconRec */
extern TextRec disp_string_tbl0[];

#define ARROW_GAP   20
#define NEVER       (1 << 30)
#define SLOTS       384             /* the glyph cache: 3 x 128 cells */
#define FONT_RANGES 45              /* load_string's code ranges */

/* The profile switch (TOKI_BACKLOG: enhanced on, parity off), read once,
 * and the player's [enhanced] backlog (the settings menu changes it live). */
extern const PcSettings *pc_settings_get(void) __attribute__((weak));
int backlog_enabled(void)
{
    static int on = -1;
    if (on < 0) on = hal_feature_on("TOKI_BACKLOG");
    return on && (!pc_settings_get || pc_settings_get()->backlog);
}

/* ---------------------------------------------------------------- font */

/* The code ranges, as load_string walks them: the first entry of
 * load_string_tbl1 whose start is above the code ends the range, and the
 * (start, base) pair before it -- for the first range load_string_tbl0 and
 * the halfword after it (0) -- gives the glyph index.  45 ranges; the
 * pair after the last (0x9873) is only its end: nothing at or above it is
 * ever found.
 * Range k: [start, end) -> base + (code - start). */
static int font_range(int k, u32 *start, u32 *end, u32 *base)
{
    if (k < 0 || k >= FONT_RANGES) return 0;
    if (k == 0) {
        *start = load_string_tbl0;
        *base = (&load_string_tbl0)[1];
    } else {
        *start = load_string_tbl1[2 * (k - 1)];
        *base = load_string_tbl1[2 * (k - 1) + 1];
    }
    *end = load_string_tbl1[2 * k];
    return *end > *start && *end - *start <= 4096;
}

static int font_index_of(u16 code)
{
    u32 start, end, base;
    int k;
    for (k = 0; k < FONT_RANGES; k++)
        if (font_range(k, &start, &end, &base) && code >= start && code < end)
            return (int)(base + (code - start));
    return -1;
}

#define FONT_HASH 8192
static u16 g_font_code[FONT_HASH];  /* open addressing: code, 0 = empty */
static int g_font_built;

static u32 bits_hash(const u8 *b)
{
    u32 h = 2166136261u;
    int i;
    for (i = 0; i < BACKLOG_GLYPH_BYTES; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* Shift-JIS trail byte */
static int sjis_trail(u32 code)
{
    u32 t = code & 0xFF;
    return (t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC);
}

/* bitmap -> code for every glyph of the font, built on first use from the
 * font in RAM.  The ranges overlap (load_string's spans cover the unused
 * trail bytes, so e.g. 0x92FD..0x933F reach the 0x9340 row's glyphs): each
 * glyph index gets its canonical code -- from the range whose base it is
 * at or after and before the next one's -- and only a valid Shift-JIS
 * code.  Identical bitmaps (blank cells) keep the lowest index's code. */
static void font_build(void)
{
    int k;
    if (g_font_built) return;
    g_font_built = 1;
    memset(g_font_code, 0, sizeof g_font_code);
    for (k = 0; k < FONT_RANGES; k++) {
        u32 start, end, base, nstart, nend, nbase, idx, last;
        if (!font_range(k, &start, &end, &base)) continue;
        last = base + (end - start);
        if (font_range(k + 1, &nstart, &nend, &nbase) && nbase < last) last = nbase;
        for (idx = base; idx < last; idx++) {
            u32 c = start + (idx - base);
            const u8 *b = &melt_font_tbl0[idx * BACKLOG_GLYPH_BYTES];
            u32 h = bits_hash(b) & (FONT_HASH - 1);
            int probes = 0;
            if (!sjis_trail(c)) continue;
            while (g_font_code[h] && probes < FONT_HASH) {
                int j = font_index_of(g_font_code[h]);
                if (j >= 0 && !memcmp(&melt_font_tbl0[j * BACKLOG_GLYPH_BYTES], b, BACKLOG_GLYPH_BYTES))
                    break;              /* this bitmap is already known */
                h = (h + 1) & (FONT_HASH - 1);
                probes++;
            }
            if (!g_font_code[h]) g_font_code[h] = (u16)c;
        }
    }
}

u16 backlog_font_code(const u8 bits[BACKLOG_GLYPH_BYTES])
{
    u32 h;
    int probes;
    font_build();
    h = bits_hash(bits) & (FONT_HASH - 1);
    for (probes = 0; g_font_code[h] && probes < FONT_HASH; probes++) {
        int k = font_index_of(g_font_code[h]);
        if (k >= 0 && !memcmp(&melt_font_tbl0[k * BACKLOG_GLYPH_BYTES], bits, BACKLOG_GLYPH_BYTES))
            return g_font_code[h];
        h = (h + 1) & (FONT_HASH - 1);
    }
    return 0;
}

int backlog_font_glyph(u16 code, u8 bits[BACKLOG_GLYPH_BYTES])
{
    int k = font_index_of(code);
    if (k < 0) return 0;
    memcpy(bits, &melt_font_tbl0[k * BACKLOG_GLYPH_BYTES], BACKLOG_GLYPH_BYTES);
    return 1;
}

/* ---------------------------------------------------------------- page */

/* Glyph cache cell `i` (vload_font's placement) -> 12x12 bits. */
static void cell_bits(int i, u8 out[BACKLOG_GLYPH_BYTES])
{
    int x0 = (i < 128 ? 800 : i < 256 ? 864 : 928) + (i & 7) * 4;
    int y0 = 256 + ((i & 0x78) << 1);
    int r, c;
    memset(out, 0, BACKLOG_GLYPH_BYTES);
    for (r = 0; r < BACKLOG_GLYPH_PX; r++)
        for (c = 0; c < BACKLOG_GLYPH_PX; c++) {
            u16 hw = g_vram[y0 + r][x0 + c / 4];
            if ((hw >> ((c & 3) * 4)) & 15) {
                int bit = r * BACKLOG_GLYPH_PX + c;
                out[bit >> 3] |= (u8)(0x80 >> (bit & 7));
            }
        }
}

/* Colour 1 of the string's CLUT (disp_string's GetClut(256 + (tp&8)*4,
 * 504 + (tp&7))); its shadow is the same cell drawn black one pixel down
 * and right. */
static u16 clut_fg(u8 tp)
{
    return g_vram[504 + (tp & 7)][256 + (tp & 8) * 4 + 1];
}

static BacklogPage g_page;

/* disp_string's walk, without drawing: glyph i belongs to record r at
 * column col; only records with flag 1 are shown.  The page is glyphs
 * `base` .. the last shown one. */
static int build_page(int base)
{
    int n = check_end_k_tbl0 < check_end_k_tbl1 ? check_end_k_tbl0 : check_end_k_tbl1;
    int i, col = 0, r = 0, cur = -1, nrec = disp_string_state;
    BacklogPage *p = &g_page;
    memset(p, 0, sizeof *p);
    p->speaker = -1;
    if (n > SLOTS) n = SLOTS;
    if (nrec > BACKLOG_MAX_SEGS) nrec = BACKLOG_MAX_SEGS;
    for (i = 0; i < n; i++, col++) {
        const TextRec *t;
        BacklogGlyph *g;
        if (!((u32)col < disp_string_tbl0[r].count)) {
            col = 0;
            r++;
        }
        if (r >= nrec) break;
        t = &disp_string_tbl0[r];
        if (t->flag != 1 || i < base) continue;
        if (p->nglyph >= BACKLOG_MAX_GLYPHS) break;
        if (cur != r) {
            if (p->nseg >= BACKLOG_MAX_SEGS) break;
            cur = r;
            p->seg[p->nseg].x = t->x;
            p->seg[p->nseg].y = t->y;
            p->seg[p->nseg].first = p->nglyph;
            p->nseg++;
        }
        g = &p->g[p->nglyph++];
        /* English (pc/hal/lang.c): a proportional cell keeps its own x */
        g->x = (s16)(t->x + lang_glyph_x(r, i, col, col * k_cell_w));
        g->y = t->y;
        g->fg = clut_fg(t->tp);
        g->shadow = 0;
        cell_bits(i, g->bits);
        /* English (pc/hal/lang.c): a Latin glyph's ASCII code first (its
         * bits can equal a full-width retail glyph's, e.g. '.') */
        g->code = lang_glyph_code(g->bits);
        if (!g->code) g->code = backlog_font_code(g->bits);
        p->seg[p->nseg - 1].count++;
    }
    return p->nglyph;
}

/* The speech layout: a name string, then at x = -86 on the same line the
 * opening bracket (serifu_cmd_exec: the bracket alone, the lines at -72;
 * the TEL calls: the bracket and the first line).  -> the name's segment. */
static int find_speaker(const BacklogPage *p)
{
    int s;
    for (s = 0; s + 1 < p->nseg; s++)
        if (p->seg[s + 1].x == -86 && p->seg[s + 1].y == p->seg[s].y &&
            p->seg[s].x < -86 && p->seg[s].count > 0)
            return s;
    /* English (pc/hal/lang.c): the patch draws no bracket; the body starts
     * on the name's line at the retail -72, an opening quote hung left of
     * it (-77 for '"') */
    if (lang_en())
        for (s = 0; s + 1 < p->nseg; s++)
            if (p->seg[s + 1].x >= -86 && p->seg[s + 1].x <= -72 && p->seg[s + 1].y == p->seg[s].y &&
                p->seg[s].x < -86 && p->seg[s].count > 0)
                return s;
    return -1;
}

/* A page with a speaker is the message window: the name's line and the
 * lines below it.  Strings above it (a menu the conversation was opened
 * from, a screen title) are dropped. */
static void keep_window(BacklogPage *p)
{
    BacklogPage *q = p;
    int s, n = 0, k = 0, top;
    if (p->speaker < 0) return;
    top = p->seg[p->speaker].y;
    for (s = 0; s < p->nseg; s++) {
        BacklogSeg seg = p->seg[s];
        int j;
        if (seg.y < top) continue;
        if (s == p->speaker) q->speaker = (s16)n;
        q->seg[n] = seg;
        q->seg[n].first = (u16)k;
        for (j = 0; j < seg.count; j++) q->g[k++] = p->g[seg.first + j];
        n++;
    }
    q->nseg = (u16)n;
    q->nglyph = (u16)k;
}

static u32 page_sig(const BacklogPage *p)
{
    u32 h = 2166136261u;
    const u8 *b = (const u8 *)p->g;
    size_t k, n = (size_t)p->nglyph * sizeof(BacklogGlyph);
    for (k = 0; k < n; k++) h = (h ^ b[k]) * 16777619u;
    return h ^ p->nglyph;
}

/* ---------------------------------------------------------------- voice */

typedef struct {
    u32 serial;                     /* 0 = none yet */
    int open;                       /* still taking sectors */
    s32 frame;                      /* frame_counter at its first sector */
    BacklogVoice v;
} Stream;
static Stream g_stream;
static u32 g_stream_serial, g_attached_serial;
static int g_entry = -1;            /* g_file_lba_table row of the last sector */

static int voice_file(const char *path)
{
    return !strncmp(path, "CDROM/XA_G/", 11) && strcmp(path, "CDROM/XA_G/BGM378.XA") &&
           strlen(path) < sizeof(g_stream.v.path);
}

static const FileLbaEntry *file_of(u32 lba)
{
    u32 i;
    const FileLbaEntry *e;
    if (g_entry >= 0) {
        e = &g_file_lba_table[g_entry];
        if (lba >= e->lba && lba < e->lba + (e->size + 2047) / 2048) return e;
    }
    for (i = 0; i < g_file_lba_table_count; i++) {
        e = &g_file_lba_table[i];
        if (lba >= e->lba && lba < e->lba + (e->size + 2047) / 2048) {
            g_entry = (int)i;
            return e;
        }
    }
    return NULL;
}

/* The line the game asked for: set_XAsector(e0, e1, girl) records
 * {file index, the file's base, e0, e1} in D_800BA260, and SD_XACmd turns
 * it into the drive's read window: channel = e0 >> 24, interleave 8 for
 * index 15 else 32, first = (e0 & 0xFFFFFF) * interleave + channel + base
 * - 16, last = (e1 & 0xFFFFFF) * interleave + channel + base + 16 (it
 * pauses there).  A player who skips the voice stops the drive early; the
 * window still says where the whole line is.  Kept only when the sector
 * being read is inside it and on its channel. */
extern s32 XAsector[4];
static void request_window(BacklogVoice *v, u32 lba)
{
    u32 e0 = (u32)XAsector[2], e1 = (u32)XAsector[3];
    u32 chan = e0 >> 24, base = (u32)XAsector[1];
    u32 step = (XAsector[0] & 0xF) == 0xF ? 8u : 32u;
    u32 first = (e0 & 0xFFFFFFu) * step + chan + base - 16u;
    u32 last = (e1 & 0xFFFFFFu) * step + chan + base + 16u;
    v->req_first = v->req_last = 0;
    if (chan == v->channel && first <= lba && lba <= last && last - first < 75u * 60u) {
        v->req_first = first;
        v->req_last = last;
    }
}

void backlog_xa_reset(void)
{
    if (!backlog_enabled()) return;
    g_stream.open = 0;
}

void backlog_xa_sector(u32 lba, const u8 *raw, int filter)
{
    const FileLbaEntry *e;
    if (!backlog_enabled()) return;
    if (cd_audio_accepts && !cd_audio_accepts(raw, filter)) return;
    e = file_of(lba);
    if (!e || !voice_file(e->path)) return;
    if (!(g_stream.open && g_stream.v.file_lba == e->lba && g_stream.v.file == raw[0] &&
          g_stream.v.channel == raw[1] && lba > g_stream.v.lba_last)) {
        memset(&g_stream, 0, sizeof g_stream);
        g_stream.serial = ++g_stream_serial;
        g_stream.open = 1;
        g_stream.frame = frame_counter;
        strcpy(g_stream.v.path, e->path);
        g_stream.v.file_lba = e->lba;
        g_stream.v.lba_first = lba;
        g_stream.v.file = raw[0];
        g_stream.v.channel = raw[1];
        request_window(&g_stream.v, lba);
        backlog_game_voice();
    }
    g_stream.v.lba_last = lba;
    g_stream.v.sectors++;
    if (raw[2] & 0x80) g_stream.open = 0;     /* EOF */
}

/* ---------------------------------------------------------------- frame */

static int g_arrow_age = NEVER;
static u32 g_arrow_screen;
static int g_waiting;               /* the previous frame was an arrow wait */
static s16 g_prev_loaded, g_prev_shown, g_prev_count;
static int g_base;                  /* the page's first glyph */
static s32 g_base_frame;            /* ... set on this frame */
/* The page on screen, built when it completed (at most once per state). */
static int g_cand;                  /* 0 none, 1 built, 2 committed */
static u32 g_cand_key;
static u32 g_last_sig, g_last_sig_screen;
static u32 g_serial;
/* "Skip read dialogue" (pc/hal/seen_text.h): the page on screen's text
 * hash, and whether it was read before this showing (looked up when the
 * page is built, before this showing records it). */
static unsigned long long g_cur_hash;
static int g_cur_seen;
/* pc/hal/seen_text.c; weak so the capture harness links without it */
extern int seen_text_recording(void) __attribute__((weak));
extern unsigned long long seen_text_page_hash(const BacklogPage *p) __attribute__((weak));
extern int seen_text_has(unsigned long long h) __attribute__((weak));
extern void seen_text_add(unsigned long long h) __attribute__((weak));
static int seen_on(void) { return seen_text_recording && seen_text_recording(); }

/* The page on screen (built: complete) was read before.  pc/hal/enhance.c */
int backlog_page_seen(void)
{
    return g_cand != 0 && g_cur_seen;
}

static void take(void)
{
    u32 sig = page_sig(&g_page);
    g_cand = 2;
    if (seen_on()) seen_text_add(g_cur_hash);
    if (sig == g_last_sig && g_page.screen == g_last_sig_screen) return;   /* the same page again */
    g_last_sig = sig;
    g_last_sig_screen = g_page.screen;
    g_page.serial = ++g_serial;
    g_page.vblank = hal_clock_vblanks ? hal_clock_vblanks() : 0;
    if (g_stream.serial && g_stream.serial != g_attached_serial &&
        g_stream.frame >= g_base_frame) {
        g_attached_serial = g_stream.serial;
        g_page.has_voice = 1;
        g_page.voice = g_stream.v;
    }
    if (backlog_enabled()) backlog_add(&g_page);
}

/* Once per game frame.  A page is the text shown since the last page:
 * after a reset (k_reset: check_end_k_tbl1 drops to the strings it keeps)
 * the glyphs from there on; after strings were added to a page already
 * captured, the new glyphs.  It is built the first time it is complete
 * (every loaded glyph shown) and captured
 *   - when the arrow wait starts (the page and its voice are done), or
 *   - when it is taken down unseen-by-the-arrow (a reset, a new screen):
 *     a player who confirms during a voice line skips the arrow
 *     (serifu_main: voice_level 2, pad -> state 2 -> next page), and the
 *     page still belongs in the log when it has the speech layout or a
 *     voice line of its own.  Menus and labels (no name, no voice, no
 *     arrow) are not dialogue and are left out. */
void backlog_capture_frame(void)
{
    u32 screen, key;
    s16 loaded, shown, count;
    int waiting, complete, reset;
    /* the backlog's capture, or only the read-page record (seen_text.c) */
    if (!backlog_enabled() && !seen_on()) return;
    screen = (u32)now_step << 16 | (u32)now_sub_step << 8 | now_sub_sub_step;
    loaded = check_end_k_tbl0;
    shown = check_end_k_tbl1;
    count = disp_string_state;
    if (g_arrow_age < NEVER) g_arrow_age++;
    if (yazirushi_cw_disp & 0x80) {
        g_arrow_age = 0;
        g_arrow_screen = screen;
    } else if (screen != g_arrow_screen) {
        g_arrow_age = NEVER;
    }

    /* the page on screen goes away: capture it if it was dialogue */
    reset = shown < g_prev_shown || count < g_prev_count;
    if (g_cand == 1 && reset && g_page.speaker >= 0) take();
    if (reset) {
        g_base = shown < g_prev_shown ? shown : 0;
        g_base_frame = frame_counter;
        g_cand = 0;
    } else if (loaded > g_prev_loaded && g_cand == 2) {
        g_base = g_prev_loaded;             /* strings added after a capture */
        g_base_frame = frame_counter;
        g_cand = 0;
    }

    complete = loaded == shown && shown > g_base;
    key = (u32)(u16)shown << 16 | (u32)(u16)count;
    if (complete && (g_cand == 0 || (g_cand == 1 && key != g_cand_key))) {
        if (build_page(g_base)) {
            g_page.frame = (u32)frame_counter;
            g_page.screen = screen;
            g_page.girl = now_girl;
            g_page.speaker = (s16)find_speaker(&g_page);
            keep_window(&g_page);
            g_cand = 1;
            g_cand_key = key;
            if (seen_on()) {
                g_cur_hash = seen_text_page_hash(&g_page);
                g_cur_seen = seen_text_has(g_cur_hash);
            }
        }
    }
    waiting = complete && g_arrow_age <= ARROW_GAP;
    if (waiting && !g_waiting && g_cand == 1) take();

    g_waiting = waiting;
    g_prev_loaded = loaded;
    g_prev_shown = shown;
    g_prev_count = count;
}
