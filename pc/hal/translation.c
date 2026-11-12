/* SPDX-License-Identifier: GPL-3.0-or-later */
/* translation.c -- apply the embedded English translation pack.
 *
 * See translation.h for the format (no retail byte is stored in the pack; every
 * byte that is not ours is taken from the player's own file).
 *
 * SDL-free and game-free: pc/tests/test_translation.c links it alone, and
 * the first-run setup can call translation_prepare() right after the asset
 * extraction.
 */
#include "translation.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <direct.h>
#define tr_getpid() ((long)_getpid())
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#define tr_getpid() ((long)getpid())
#endif
#include "pc_settings.h"   /* PC_MKDIR_SPLIT */

/* ------------------------------------------------------------------ SHA-1 */
typedef struct { unsigned int h[5]; unsigned long long len; unsigned char buf[64]; size_t n; } Sha1;

static unsigned int rol(unsigned int v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(Sha1 *c, const unsigned char *p) {
    unsigned int w[80], a, b, d, e, f, k, t, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (unsigned int)p[4 * i] << 24 | (unsigned int)p[4 * i + 1] << 16 |
               (unsigned int)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void translation_sha1(const void *data, size_t n, unsigned char out[20]) {
    Sha1 c;
    const unsigned char *p = (const unsigned char *)data;
    unsigned char pad[72];
    size_t padn, i;
    unsigned long long bits = (unsigned long long)n * 8;
    c.h[0] = 0x67452301u; c.h[1] = 0xEFCDAB89u; c.h[2] = 0x98BADCFEu;
    c.h[3] = 0x10325476u; c.h[4] = 0xC3D2E1F0u;
    while (n >= 64) { sha1_block(&c, p); p += 64; n -= 64; }
    memset(pad, 0, sizeof pad);
    memcpy(pad, p, n);
    pad[n] = 0x80;
    padn = (n < 56) ? 64 : 128;
    {
        unsigned char tail[128];
        memset(tail, 0, sizeof tail);
        memcpy(tail, pad, n + 1);
        for (i = 0; i < 8; i++) tail[padn - 1 - i] = (unsigned char)(bits >> (8 * i));
        sha1_block(&c, tail);
        if (padn == 128) sha1_block(&c, tail + 64);
    }
    for (i = 0; i < 5; i++) {
        out[4 * i] = (unsigned char)(c.h[i] >> 24); out[4 * i + 1] = (unsigned char)(c.h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(c.h[i] >> 8); out[4 * i + 3] = (unsigned char)c.h[i];
    }
}

/* ------------------------------------------------------------ the embedded pack */
/* pc/build/translation_pack.S defines these when the build had a pack;
 * otherwise the weak NULLs stand (a build without English). */
extern const unsigned char tktp_embedded_start[] __attribute__((weak));
extern const unsigned char tktp_embedded_end[] __attribute__((weak));

const unsigned char *translation_pack_data(size_t *size) {
    if (!tktp_embedded_start || !tktp_embedded_end
        || (unsigned long)tktp_embedded_end <= (unsigned long)tktp_embedded_start) {
        if (size) *size = 0;
        return NULL;
    }
    if (size) *size = (size_t)(tktp_embedded_end - tktp_embedded_start);
    return tktp_embedded_start;
}

/* ------------------------------------------------------------ pack reader */
typedef struct { const unsigned char *p, *end; int bad; } Rd;

static unsigned int rd_u32(Rd *r) {
    unsigned int v;
    if (r->end - r->p < 4) { r->bad = 1; return 0; }
    v = (unsigned int)r->p[0] | (unsigned int)r->p[1] << 8 | (unsigned int)r->p[2] << 16 | (unsigned int)r->p[3] << 24;
    r->p += 4;
    return v;
}
static unsigned rd_u16(Rd *r) {
    unsigned v;
    if (r->end - r->p < 2) { r->bad = 1; return 0; }
    v = r->p[0] | r->p[1] << 8;
    r->p += 2;
    return v;
}
static const unsigned char *rd_bytes(Rd *r, size_t n) {
    const unsigned char *q = r->p;
    if ((size_t)(r->end - r->p) < n) { r->bad = 1; return NULL; }
    r->p += n;
    return q;
}

typedef struct {
    char path[256];
    unsigned int base_size, out_size, nops;
    const unsigned char *base_sha1, *out_sha1, *ops;
} Entry;

/* Read the next entry header; leaves r just past its ops. */
static int next_entry(Rd *r, Entry *e) {
    unsigned plen = rd_u16(r);
    const unsigned char *pp = rd_bytes(r, plen);
    unsigned int i;
    if (r->bad || plen >= sizeof e->path) return 0;
    memcpy(e->path, pp, plen);
    e->path[plen] = 0;
    e->base_size = rd_u32(r);
    e->base_sha1 = rd_bytes(r, 20);
    e->out_size = rd_u32(r);
    e->out_sha1 = rd_bytes(r, 20);
    e->nops = rd_u32(r);
    e->ops = r->p;
    for (i = 0; i < e->nops && !r->bad; i++) {
        unsigned char t;
        unsigned int len;
        if (!rd_bytes(r, 1)) break;
        t = r->p[-1];
        switch (t) {
        case TKTP_SAME: rd_u32(r); break;
        case TKTP_COPY: rd_u32(r); rd_u32(r); break;
        case TKTP_MASK: len = rd_u32(r); rd_bytes(r, len); rd_bytes(r, len); break;
        case TKTP_FILL: rd_u32(r); rd_bytes(r, 1); break;
        default: r->bad = 1;
        }
    }
    return !r->bad;
}

static int open_pack(const unsigned char *pack, size_t size, Rd *r, unsigned int *nfiles) {
    r->p = pack; r->end = pack + size; r->bad = 0;
    if (!pack || size < 12 || memcmp(pack, "TKTP", 4) != 0) return 0;
    r->p += 4;
    if (rd_u32(r) != 1) return 0;
    *nfiles = rd_u32(r);
    return !r->bad;
}

/* Run one entry's ops over `base`; NULL on a malformed recipe or a result
 * whose hash is not the one the pack promises. */
static unsigned char *run_ops(const Entry *e, const unsigned char *base, size_t base_size) {
    Rd r;
    unsigned char *out, h[20];
    unsigned int pos = 0, i;
    if (e->base_size && (base_size != e->base_size)) return NULL;
    out = (unsigned char *)malloc(e->out_size ? e->out_size : 1);
    if (!out) return NULL;
    r.p = e->ops; r.end = e->ops + 0x7fffffff; r.bad = 0;
    for (i = 0; i < e->nops; i++) {
        unsigned char t = *r.p++;
        unsigned int len, src, k;
        const unsigned char *m, *v;
        switch (t) {
        case TKTP_SAME:
            len = rd_u32(&r);
            if (pos + len > e->out_size || pos + len > base_size) goto bad;
            memcpy(out + pos, base + pos, len);
            break;
        case TKTP_COPY:
            src = rd_u32(&r); len = rd_u32(&r);
            if (pos + len > e->out_size || src + len > base_size || src + len < src) goto bad;
            memcpy(out + pos, base + src, len);
            break;
        case TKTP_MASK:
            len = rd_u32(&r);
            m = rd_bytes(&r, len); v = rd_bytes(&r, len);
            if (pos + len > e->out_size) goto bad;
            for (k = 0; k < len; k++) {
                unsigned char b = (pos + k < base_size) ? base[pos + k] : 0;
                out[pos + k] = (unsigned char)((b & ~m[k]) | (v[k] & m[k]));
            }
            break;
        case TKTP_FILL:
            len = rd_u32(&r);
            if (pos + len > e->out_size) goto bad;
            memset(out + pos, *r.p++, len);
            break;
        default:
            goto bad;
        }
        pos += len;
    }
    if (pos != e->out_size) goto bad;
    translation_sha1(out, e->out_size, h);
    if (memcmp(h, e->out_sha1, 20) != 0) goto bad;
    return out;
bad:
    free(out);
    return NULL;
}

unsigned char *translation_apply_file(const unsigned char *pack, size_t pack_size,
                                      const char *path, const unsigned char *base,
                                      size_t base_size, size_t *out_size) {
    Rd r;
    unsigned int n, i;
    Entry e;
    if (!open_pack(pack, pack_size, &r, &n)) return NULL;
    for (i = 0; i < n; i++) {
        if (!next_entry(&r, &e)) return NULL;
        if (strcmp(e.path, path) != 0) continue;
        if (e.base_size) {
            unsigned char h[20];
            if (!base || base_size != e.base_size) return NULL;
            translation_sha1(base, base_size, h);
            if (memcmp(h, e.base_sha1, 20) != 0) return NULL;
        }
        if (out_size) *out_size = e.out_size;
        return run_ops(&e, base, base_size);
    }
    return NULL;
}

/* ------------------------------------------------------------ files */
static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long sz;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char *)malloc(sz ? (size_t)sz : 1);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *n = (size_t)sz;
    return buf;
}

#ifdef _WIN32
#define tr_mkdir(p) _mkdir(p)
#else
#define tr_mkdir(p) mkdir(p, 0755)
#endif

static int mkdirs_for(const char *file) {
    char tmp[1024];
    size_t i;
    snprintf(tmp, sizeof tmp, "%s", file);
    for (i = 1; tmp[i]; i++) {
        if (PC_MKDIR_SPLIT(tmp + i)) {
            char c = tmp[i];
            tmp[i] = 0;
            if (tr_mkdir(tmp) != 0 && errno != EEXIST) return -1;
            tmp[i] = c;
        }
    }
    return 0;
}

/* Test hook (pc/tools/test_translation_race.py's negative control):
 * TOKI_TRANSLATION_NO_LOCK=1 restores the pre-2026-11-02 behaviour -- no
 * lock, one shared "<file>.part" name for every process. */
static int no_lock(void) {
    const char *v = getenv("TOKI_TRANSLATION_NO_LOCK");
    return v && *v && *v != '0';
}

/* Replace `path` with `tmp`.  POSIX rename replaces atomically; Windows
 * rename() refuses an existing target, and a file another process has open
 * cannot be replaced for a moment, so MoveFileEx with a few retries. */
static int replace_file(const char *tmp, const char *path) {
#ifdef _WIN32
    int i;
    for (i = 0; i < 50; i++) {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
        Sleep(20);
    }
    return -1;
#else
    return rename(tmp, path);
#endif
}

static int write_atomic(const char *path, const unsigned char *data, size_t n) {
    char tmp[1100];
    FILE *f;
    if (mkdirs_for(path) != 0) return -1;
    /* a name of this process's own: two processes preparing at once never
     * write, rename or delete each other's temporary file */
    if (no_lock())
        snprintf(tmp, sizeof tmp, "%s.part", path);
    else
        snprintf(tmp, sizeof tmp, "%s.%ld.part", path, tr_getpid());
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, n, f) != n) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (replace_file(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* The preparation lock: <out_root>/.prepare.lock, held (blocking) for the
 * whole preparation, so a second game started at the same time waits for
 * the first and then finds every file up to date.  flock / LockFileEx:
 * released by the system if the holder dies.  The file is removed when the
 * preparation ends (2026-11-07): POSIX unlinks it while still holding it
 * (a waiter that then gets the lock of the removed file opens the new one
 * instead); Windows deletes it when its last handle closes. */
typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
    char path[1100];
#endif
    int held;
} PrepLock;

static void prep_lock(PrepLock *l, const char *out_root) {
    char path[1100];
    l->held = 0;
    if (no_lock()) return;
    snprintf(path, sizeof path, "%s/.prepare.lock", out_root);
    if (mkdirs_for(path) != 0) return;
#ifdef _WIN32
    {
        OVERLAPPED ov;
        l->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE | DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_HIDDEN |
                           FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (l->h == INVALID_HANDLE_VALUE) return;
        memset(&ov, 0, sizeof ov);
        if (LockFileEx(l->h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) l->held = 1;
        else CloseHandle(l->h);
    }
#else
    {
        int tries;
        snprintf(l->path, sizeof l->path, "%s", path);
        for (tries = 0; tries < 50; tries++) {
            struct stat a, b;
            l->fd = open(path, O_RDWR | O_CREAT, 0644);
            if (l->fd < 0) return;
            while (flock(l->fd, LOCK_EX) != 0) {
                if (errno != EINTR) { close(l->fd); return; }
            }
            if (fstat(l->fd, &a) == 0 && stat(path, &b) == 0 && a.st_dev == b.st_dev &&
                a.st_ino == b.st_ino) {
                l->held = 1;
                return;
            }
            close(l->fd);           /* the previous holder removed this one */
        }
    }
#endif
}

static void prep_unlock(PrepLock *l) {
    if (!l->held) return;
#ifdef _WIN32
    {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        UnlockFileEx(l->h, 0, 1, 0, &ov);
        CloseHandle(l->h);
    }
#else
    unlink(l->path);
    flock(l->fd, LOCK_UN);
    close(l->fd);
#endif
    l->held = 0;
}

/* 1 when `path` already holds the entry's result */
static int is_up_to_date(const char *path, const unsigned char *sha1, size_t size) {
    size_t have_n = 0;
    unsigned char h[20], *out = slurp(path, &have_n);
    int ok = 0;
    if (!out) return 0;
    if (have_n == size) {
        translation_sha1(out, have_n, h);
        ok = memcmp(h, sha1, 20) == 0;
    }
    free(out);
    return ok;
}

static void note(TranslationResult *res, const char *fmt, const char *a) {
    if (!res->first_error[0]) snprintf(res->first_error, sizeof res->first_error, fmt, a);
}

static int prepare_locked(const unsigned char *pack, size_t pack_size,
                          const char *assets_root, const char *out_root,
                          TranslationResult *res);

int translation_prepare_pack(const unsigned char *pack, size_t pack_size,
                             const char *assets_root, const char *out_root,
                             TranslationResult *res) {
    TranslationResult local;
    PrepLock lock;
    int rc;
    if (!res) res = &local;
    memset(res, 0, sizeof *res);
    if (!pack) pack = translation_pack_data(&pack_size);
    if (!pack) {
        note(res, "%s", "no translation pack in this build");
        res->failed = 1;
        return -1;
    }
    /* two games started at once (or the setup and the game) must not
     * prepare the same tree at the same time: the loser used to fail on
     * the shared temporary file and play in Japanese (2026-11-02) */
    prep_lock(&lock, out_root);
    rc = prepare_locked(pack, pack_size, assets_root, out_root, res);
    prep_unlock(&lock);
    return rc;
}

static int prepare_locked(const unsigned char *pack, size_t pack_size,
                          const char *assets_root, const char *out_root,
                          TranslationResult *res) {
    Rd r;
    unsigned int n, i;
    if (!open_pack(pack, pack_size, &r, &n)) {
        note(res, "%s", "no translation pack in this build");
        res->failed = 1;
        return -1;
    }
    res->files = (int)n;
    for (i = 0; i < n; i++) {
        Entry e;
        char src[1024], dst[1024];
        unsigned char *base = NULL, *out, h[20];
        size_t base_n = 0;
        if (!next_entry(&r, &e)) { res->failed++; note(res, "%s", "malformed pack"); return -1; }
        if (e.path[0] == '@') { res->files--; continue; }   /* metadata, not a disc file */
        snprintf(src, sizeof src, "%s/raw/%s", assets_root, e.path);
        snprintf(dst, sizeof dst, "%s/raw/%s", out_root, e.path);
        /* already there and right? */
        if (is_up_to_date(dst, e.out_sha1, e.out_size)) { res->up_to_date++; continue; }
        if (e.base_size) {
            base = slurp(src, &base_n);
            if (!base) { res->missing_base++; note(res, "missing base file %s", src); continue; }
            translation_sha1(base, base_n, h);
            if (base_n != e.base_size || memcmp(h, e.base_sha1, 20) != 0) {
                res->bad_base++;
                note(res, "%s is not the supported retail file", src);
                free(base);
                continue;
            }
        }
        out = run_ops(&e, base, base_n);
        free(base);
        if (!out) { res->failed++; note(res, "recipe failed for %s", e.path); continue; }
        if (write_atomic(dst, out, e.out_size) != 0) {
            /* somebody else's process (no lock: an older build, a file
             * system without locks) may have written the same file */
            if (!no_lock() && is_up_to_date(dst, e.out_sha1, e.out_size)) {
                res->up_to_date++;
            } else {
                res->failed++;
                note(res, "cannot write %s", dst);
            }
        } else {
            res->written++;
        }
        free(out);
    }
    return (res->missing_base || res->bad_base || res->failed) ? -1 : 0;
}

int translation_prepare(const char *assets_root, const char *out_root) {
    TranslationResult res;
    int rc = translation_prepare_pack(NULL, 0, assets_root, out_root, &res);
    printf("[TRANSLATION] %s -> %s: %d files, %d written, %d up to date",
           assets_root, out_root, res.files, res.written, res.up_to_date);
    if (rc) printf(", %d missing, %d wrong base, %d failed (%s)",
                   res.missing_base, res.bad_base, res.failed, res.first_error);
    printf("\n");
    return rc;
}
