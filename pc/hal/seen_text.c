/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seen_text.c -- the set of dialogue pages read in any session
 * (pc/hal/seen_text.h).  Host data, never the machine's: pc/hal/dbg_state.c
 * keeps this file out of every restore (KEEP_FILES), so neither a
 * quickload nor a rewind forgets what was read.
 *
 * Recording: whenever the backlog capture runs under the enhanced profile
 * in an interactive session (TOKI_SKIP_SEEN, never in an automated run --
 * the settings' harness rule -- unless the environment sets TOKI_SKIP_SEEN
 * or TOKI_SEEN_FILE), so that turning skip_seen on later already knows the
 * pages read before.  Skipping: [enhanced] skip_seen (or TOKI_SKIP_SEEN=1);
 * the input assist that uses it is pc/hal/enhance.c.
 *
 * Buffers come from mmap, as the debug toolkit's do (pc/hal/dbg_state.c:
 * the brk heap can span the MMIO page); the file is written with
 * open/write.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include "seen_text.h"
#include "pc_settings.h"
#include "profile.h"

static const char k_magic[8] = { 'T', 'K', 'S', 'E', 'E', 'N', '0', '1' };

/* ---------------------------------------------------------------- hash */

unsigned long long seen_text_page_hash(const BacklogPage *p) {
    unsigned long long h = 14695981039346656037ull;
    int s, k;
#define MIX(byte) (h = (h ^ (unsigned char)(byte)) * 1099511628211ull)
    for (s = 0; s < p->nseg; s++) {
        const BacklogSeg *g = &p->seg[s];
        if (s == p->speaker) continue;
        MIX(0xFF); MIX(0xFE);                    /* a string starts */
        for (k = 0; k < g->count && g->first + k < p->nglyph; k++) {
            const BacklogGlyph *c = &p->g[g->first + k];
            if (c->code) {
                MIX(c->code >> 8); MIX(c->code);
            } else {
                int b;
                MIX(0); MIX(0);
                for (b = 0; b < BACKLOG_GLYPH_BYTES; b++) MIX(c->bits[b]);
            }
        }
    }
#undef MIX
    return h ? h : 1;                            /* 0 marks an empty slot */
}

/* ---------------------------------------------------------------- set */

static void *mem_alloc(unsigned long n) {
#ifdef _WIN32
    /* no process-image save states on Windows (WINDOWS-BUILD.md), so the
     * mmap-outside-the-image rule has nothing to protect; zeroed pages */
    return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void mem_free(void *p, unsigned long n) {
#ifdef _WIN32
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

static unsigned long slot_of(const SeenSet *s, unsigned long long h) {
    unsigned long i = (unsigned long)(h ^ (h >> 29)) & (s->cap - 1);
    while (s->slot[i] && s->slot[i] != h) i = (i + 1) & (s->cap - 1);
    return i;
}

int seen_set_has(const SeenSet *s, unsigned long long h) {
    if (!s->cap) return 0;
    if (!h) h = 1;
    return s->slot[slot_of(s, h)] == h;
}

static int grow(SeenSet *s) {
    unsigned long cap = s->cap ? s->cap * 2 : 4096, i;
    SeenSet t;
    t.slot = mem_alloc(cap * sizeof *t.slot);
    if (!t.slot) return -1;
    t.cap = cap;
    t.n = s->n;
    for (i = 0; i < s->cap; i++)
        if (s->slot[i]) t.slot[slot_of(&t, s->slot[i])] = s->slot[i];
    if (s->slot) mem_free(s->slot, s->cap * sizeof *s->slot);
    *s = t;
    return 0;
}

int seen_set_add(SeenSet *s, unsigned long long h) {
    unsigned long i;
    if (!h) h = 1;
    if ((s->n + 1) * 2 > s->cap && grow(s)) return -1;
    i = slot_of(s, h);
    if (s->slot[i] == h) return 0;
    s->slot[i] = h;
    s->n++;
    return 1;
}

void seen_set_free(SeenSet *s) {
    if (s->slot) mem_free(s->slot, s->cap * sizeof *s->slot);
    memset(s, 0, sizeof *s);
}

int seen_set_load(SeenSet *s, const char *path) {
    int fd = open(path, O_RDONLY);
    unsigned char buf[8 * 512];
    char magic[8];
    ssize_t r;
    if (fd < 0) return 0;
    if (read(fd, magic, 8) != 8 || memcmp(magic, k_magic, 8)) {
        close(fd);
        return -1;
    }
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        ssize_t k;
        for (k = 0; k + 8 <= r; k += 8) {
            unsigned long long h = 0;
            int b;
            for (b = 7; b >= 0; b--) h = h << 8 | buf[k + b];
            if (h && seen_set_add(s, h) < 0) { close(fd); return -1; }
        }
    }
    close(fd);
    return 0;
}

int seen_file_append(const char *path, unsigned long long h) {
    struct stat st;
    unsigned char rec[16];
    int fd, n = 0, b, ok;
    int fresh = stat(path, &st) != 0 || st.st_size < 8;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    if (fresh) {
        if (ftruncate(fd, 0)) { close(fd); return -1; }
        memcpy(rec, k_magic, 8);
        n = 8;
    }
    for (b = 0; b < 8; b++) rec[n++] = (unsigned char)(h >> (8 * b));
    ok = write(fd, rec, (size_t)n) == n;
    return close(fd) == 0 && ok ? 0 : -1;
}

/* ---------------------------------------------------------------- session */

extern const char *pc_save_dir(void) __attribute__((weak));

static SeenSet g_set;
static int g_loaded;            /* 0 not yet, 1 read, -1 unusable file */
static char g_path[PC_SETTINGS_PATH_MAX + 32];

static int env_switch(const char *name) {      /* -1 unset, else 0/1 */
    const char *e = getenv(name);
    return e && *e ? *e != '0' : -1;
}

int seen_text_recording(void) {
    static int on = -1;
    if (on < 0) {
        const char *f = getenv("TOKI_SEEN_FILE");
        int e = env_switch("TOKI_SKIP_SEEN");
        if (f && *f) on = 1;
        else if (e >= 0) on = e;
        else on = hal_feature_on("TOKI_SKIP_SEEN") && !pc_settings_get()->harness;
    }
    return on;
}

int seen_text_skip_enabled(void) {
    static int env = -2;
    if (env == -2) env = env_switch("TOKI_SKIP_SEEN");
    if (env >= 0) return env;
    return seen_text_recording() && pc_settings_get()->skip_seen;
}

static void load(void) {
    const char *f;
    if (g_loaded) return;
    f = getenv("TOKI_SEEN_FILE");
    if (f && *f) snprintf(g_path, sizeof g_path, "%s", f);
    else snprintf(g_path, sizeof g_path, "%s/seen_text.dat", pc_save_dir ? pc_save_dir() : "saves");
    g_loaded = seen_set_load(&g_set, g_path) ? -1 : 1;
    if (g_loaded < 0)
        fprintf(stderr, "[SEEN] %s is not a seen-text file: read pages are not remembered\n", g_path);
    else
        printf("[SEEN] %lu read dialogue pages (%s)\n", g_set.n, g_path);
    fflush(stdout);
}

int seen_text_has(unsigned long long h) {
    if (!seen_text_recording()) return 0;
    load();
    return seen_set_has(&g_set, h);
}

void seen_text_add(unsigned long long h) {
    if (!seen_text_recording()) return;
    load();
    if (g_loaded < 0) return;
    if (seen_set_add(&g_set, h) == 1) {
        printf("[SEEN] page %016llx read\n", h);
        fflush(stdout);
        if (pc_save_dir && !getenv("TOKI_SEEN_FILE"))
            pc_settings_mkdirs(g_path, 0);
        if (seen_file_append(g_path, h))
            fprintf(stderr, "[SEEN] cannot write %s\n", g_path);
    }
}

unsigned long seen_text_count(void) {
    load();
    return g_set.n;
}
