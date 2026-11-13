/* SPDX-License-Identifier: GPL-3.0-or-later */
/* asset_setup.c -- first-run asset preparation (asset_setup.h).
 *
 * A line-by-line port of pc/tools/disc_image.py (DiscImage, resolve_sources,
 * safe_relative, build_lba_map.walk) and pc/tools/bootstrap_assets.py
 * (prepare_assets, extract_to_stage, verify_cache).  Function names below
 * follow the Python ones; where the C differs it says so.  Output equality
 * with the Python tool is tested by pc/tools/test_asset_setup_c.py on
 * synthetic discs and, with the real disc, by the same test's --real mode.
 */
#include "asset_setup.h"
#include "pc_fs.h"
#include "pc_hash.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define as_getpid() ((long)GetCurrentProcessId())
#else
#include <unistd.h>
#define as_getpid() ((long)getpid())
#endif

#define MAX_OUTPUT (2LL * 1024 * 1024 * 1024)
#define PATH_N 4096

static const char ANOMALY_REASON[] = "source XA subheader copies differ; bytes preserved unchanged";
static const unsigned char SYNC[12] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};

const AsProfile as_supported_profile = {
    702104928, "7b2f72d1319db27d0700070aa0bffe886b5ef7a4",
    24, 690176, "2d8d474816e4bf937b09dc3b017f550f7ce4e14b", 1372,
    45151344, "6cdd8500168ab4ecb18b78ef7589586d26bc4501",
    150, 298664, 39008256, 121080,
};

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

static int fail(AsError *e, int code, const char *fmt, ...) {
    va_list ap;
    if (e) {
        e->code = code;
        va_start(ap, fmt);
        vsnprintf(e->message, sizeof e->message, fmt, ap);
        va_end(ap);
    }
    return code;
}

static void set_path(AsError *e, const char *p) {
    if (e) snprintf(e->path, sizeof e->path, "%s", p);
}

static int cancelled(AsCtl *c) { return c && c->cancel; }

static void progress(AsCtl *c, int64_t n) {
    if (c) c->done += n;
}

static void join(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s/%s", a, b);
}

static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/');
#ifdef _WIN32
    const char *b = strrchr(p, '\\');
    if (b && (!s || b > s)) s = b;
#endif
    return s ? s + 1 : p;
}

/* PurePosixPath(name).suffix: the final component's last ".x", "" for a
 * leading-dot-only name. */
static const char *suffix_of(const char *name) {
    const char *base = basename_of(name);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return "";
    return dot;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == *b;
}

static int icmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ------------------------------------------------------------------ */
/* safe_relative                                                       */
/* ------------------------------------------------------------------ */

int as_safe_relative(const char *name) {
    static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", NULL};
    size_t len = name ? strlen(name) : 0;
    const char *part, *p;
    if (!len || len > 240) return -1;
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c >= 127 || strchr("\\:<>\"|?*", c)) return -1;
    }
    part = name;
    for (;;) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        char stem[256];
        size_t s;
        int i;
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
            return -1;
        if (part[n - 1] == ' ' || part[n - 1] == '.') return -1;
        for (s = 0; s < n && part[s] != '.' && s < sizeof stem - 1; s++)
            stem[s] = (char)toupper((unsigned char)part[s]);
        stem[s] = 0;
        for (i = 0; reserved[i]; i++)
            if (!strcmp(stem, reserved[i])) return -1;
        if (s == 4 && (!strncmp(stem, "COM", 3) || !strncmp(stem, "LPT", 3)) &&
            stem[3] >= '1' && stem[3] <= '9')
            return -1;
        if (!end) break;
        part = end + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* resolve_sources (CUE parsing)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char track1[PATH_N];
    int64_t t1_off, t1_len;         /* t1_len -1: the whole file */
    char audio[PATH_N];
    int has_audio;
    int64_t a_off, a_len;           /* a_len -1: the whole file from a_off */
    int64_t audio_index;            /* Track 2 INDEX 01 (sectors), -1 unknown */
} Sources;

typedef struct {
    int used;
    char file[PATH_N];
    char format[32], mode[32];
    int64_t index1, index0;         /* -1 unset */
} CueTrack;

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f') p++;
    return p;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

/* One \S+ token; returns the position after it, or NULL when none. */
static const char *token(const char *p, char *out, size_t n) {
    size_t k = 0;
    if (!*p || is_ws(*p)) return NULL;
    while (*p && !is_ws(*p)) {
        if (k + 1 < n) out[k++] = *p;
        p++;
    }
    out[k] = 0;
    return p;
}

static int is_absolute(const char *p) {
#ifdef _WIN32
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return 1;
    if (p[0] == '\\') return 1;
#endif
    return p[0] == '/';
}

/* A CUE FILE name must stay beside the .cue: no absolute path, drive,
 * network share (\\host\share: Windows would send the user's NTLM
 * credentials to it just to stat the file, and the setup scans the
 * Downloads folder for .cue files on its own), stream name or "..".
 * Disc dumps name their tracks by file name alone. */
static int cue_name_ok(const char *name) {
    const char *c = name;
    if (!name[0] || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) return 0;
    while (*c) {
        size_t len = strcspn(c, "/\\");
        if (len == 2 && c[0] == '.' && c[1] == '.') return 0;
        c += len;
        if (*c) c++;
    }
    return 1;
}

static void cue_join(char *out, size_t n, const char *cue, const char *name) {
    char dir[PATH_N];
    const char *base;
    if (is_absolute(name)) { snprintf(out, n, "%s", name); return; }
    snprintf(dir, sizeof dir, "%s", cue);
    base = basename_of(dir);
    if (base == dir) snprintf(out, n, "%s", name);
    else {
        dir[base - dir - 1] = 0;
        snprintf(out, n, "%s/%s", dir[0] ? dir : "/", name);
    }
}

static int parse_msf(const char *s, int64_t *out) {
    /* exactly NN:NN:NN */
    int m, sec, f;
    if (strlen(s) != 8 || s[2] != ':' || s[5] != ':') return -1;
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) ||
        !isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4]) ||
        !isdigit((unsigned char)s[6]) || !isdigit((unsigned char)s[7]))
        return -1;
    m = (s[0] - '0') * 10 + s[1] - '0';
    sec = (s[3] - '0') * 10 + s[4] - '0';
    f = (s[6] - '0') * 10 + s[7] - '0';
    if (sec >= 60 || f >= 75) return -2;
    *out = ((int64_t)m * 60 + sec) * 75 + f;
    return 0;
}

/* Python: re.fullmatch(r'FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)', line, re.I),
 * the quoted alternative first, as the regex backtracks. */
static int match_file(const char *line, char *name, size_t nn, char *fmt, size_t fn) {
    const char *q = line + 4, *r;
    if (!is_ws(*q)) return 0;
    q = skip_ws(q);
    if (*q == '"') {
        const char *close = strchr(q + 1, '"');
        if (close && close > q + 1 && is_ws(close[1]) && (size_t)(close - q - 1) < nn) {
            r = token(skip_ws(close + 1), fmt, fn);
            if (r && !*r) {
                memcpy(name, q + 1, (size_t)(close - q - 1));
                name[close - q - 1] = 0;
                return 1;
            }
        }
    }
    r = token(q, name, nn);
    if (!r || !is_ws(*r)) return 0;
    r = token(skip_ws(r), fmt, fn);
    return r && !*r;
}

static int parse_cue(const char *cue, CueTrack tr[100], AsError *err) {
    FILE *f;
    char *text, *line, *next;
    long n;
    char cur_file[PATH_N] = "", cur_format[32] = "";
    int have_file = 0, cur_track = -1;
    PcStat st;
    if (pc_fs_stat(cue, &st)) return fail(err, AS_E_NOT_FOUND, "disc image not found: %s", cue);
    if (st.size > 65536) return fail(err, AS_E_CUE, "CUE exceeds 64 KiB");
    f = pc_fs_fopen(cue, "rb");
    if (!f) return fail(err, AS_E_UNREADABLE, "cannot read %s: %s", cue, strerror(errno));
    text = (char *)calloc(1, (size_t)st.size + 2);
    n = (long)fread(text, 1, (size_t)st.size, f);
    fclose(f);
    if (n < 0) n = 0;
    text[n] = 0;
    if ((long)strlen(text) != n) { free(text); return fail(err, AS_E_CUE, "CUE contains NUL bytes"); }
    line = text;
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line += 3;                                      /* utf-8-sig */
    memset(tr, 0, sizeof(CueTrack) * 100);
    for (; line; line = next) {
        char kw[16], *end;
        const char *p;
        size_t len;
        next = line + strcspn(line, "\r\n");
        if (*next) {
            if (next[0] == '\r' && next[1] == '\n') { *next = 0; next += 2; }
            else { *next = 0; next++; }
        } else {
            next = NULL;
        }
        if (strlen(line) > 4096) { free(text); return fail(err, AS_E_CUE, "CUE line exceeds 4 KiB"); }
        /* str.strip() */
        p = line;
        while (*p && (is_ws(*p))) p++;
        len = strlen(p);
        end = (char *)p + len;
        while (end > p && is_ws(end[-1])) *--end = 0;
        if (!*p) continue;
        if (!token(p, kw, sizeof kw)) continue;
        if (ieq(kw, "FILE")) {
            char name[PATH_N], fmt[32];
            if (!match_file(p, name, sizeof name, fmt, sizeof fmt)) {
                free(text);
                return fail(err, AS_E_CUE, "invalid CUE FILE record");
            }
            if (!cue_name_ok(name)) {
                free(text);
                return fail(err, AS_E_CUE, "the CUE names a file outside its own folder");
            }
            snprintf(cur_file, sizeof cur_file, "%s", name);
            for (len = 0; fmt[len]; len++) fmt[len] = (char)toupper((unsigned char)fmt[len]);
            snprintf(cur_format, sizeof cur_format, "%s", fmt);
            have_file = 1;
            continue;
        } else if (ieq(kw, "TRACK")) {
            const char *q = skip_ws(p + 5);
            char num[16], mode[32];
            long t;
            size_t k;
            if (q == p + 5 || !(q = token(q, num, sizeof num))) goto bad_track;
            for (k = 0; num[k]; k++) if (!isdigit((unsigned char)num[k])) goto bad_track;
            if (!is_ws(*q)) goto bad_track;
            q = skip_ws(q);
            if (!(q = token(q, mode, sizeof mode)) || *q) goto bad_track;
            t = strtol(num, NULL, 10);
            if (t < 0 || t > 99 || tr[t].used || !have_file) {
                free(text);
                return fail(err, AS_E_CUE, "duplicate CUE track or missing FILE");
            }
            cur_track = (int)t;
            tr[t].used = 1;
            cue_join(tr[t].file, sizeof tr[t].file, cue, cur_file);
            snprintf(tr[t].format, sizeof tr[t].format, "%s", cur_format);
            for (k = 0; mode[k]; k++) mode[k] = (char)toupper((unsigned char)mode[k]);
            snprintf(tr[t].mode, sizeof tr[t].mode, "%s", mode);
            tr[t].index0 = tr[t].index1 = -1;
            continue;
        bad_track:
            free(text);
            return fail(err, AS_E_CUE, "invalid CUE TRACK record");
        } else if (ieq(kw, "INDEX")) {
            const char *q = skip_ws(p + 5);
            char num[16], msf[16];
            int64_t v;
            int rc;
            if (q == p + 5 || !(q = token(q, num, sizeof num))) continue;
            if (strcmp(num, "01") && strcmp(num, "00")) continue;
            /* Python reads INDEX 01 only; INDEX 00 is read here for the
             * single-file BIN (both tracks in one file), which the Python
             * tool rejects anyway. */
            if (!is_ws(*q)) goto bad_index;
            q = skip_ws(q);
            if (!(q = token(q, msf, sizeof msf)) || *q) goto bad_index;
            rc = parse_msf(msf, &v);
            if (rc == -1) goto bad_index;
            if (cur_track < 0 || !tr[cur_track].used) goto bad_index;
            if (!strcmp(num, "01")) {
                if (tr[cur_track].index1 >= 0) goto bad_index;
                if (rc == -2) { free(text); return fail(err, AS_E_CUE, "invalid CUE index time"); }
                tr[cur_track].index1 = v;
            } else if (rc == 0) {
                tr[cur_track].index0 = v;
            }
            continue;
        bad_index:
            if (!strcmp(num, "00")) continue;
            free(text);
            return fail(err, AS_E_CUE, "invalid or duplicate CUE INDEX 01");
        }
    }
    free(text);
    return AS_OK;
}

int as_cue_files(const char *cue, char names[][256], int max) {
    FILE *f = pc_fs_fopen(cue, "rb");
    char line[4200];
    int n = 0;
    if (!f) return -1;
    while (n < max && fgets(line, sizeof line, f)) {
        const char *p = line;
        char kw[16], name[PATH_N], fmt[32];
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || is_ws(e[-1]))) *--e = 0;
        while (*p && is_ws(*p)) p++;
        if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        if (!token(p, kw, sizeof kw) || !ieq(kw, "FILE")) continue;
        if (match_file(p, name, sizeof name, fmt, sizeof fmt)) snprintf(names[n++], 256, "%s", name);
    }
    fclose(f);
    return n;
}

/* "(Track 1)" -> "(Track 2)" anywhere in the file name, case-insensitive
 * (Python: re.sub(r'(?i)(\(track\s+)1(\))', r'\g<1>2\2', name)). */
static int partner_name(const char *name, char *out, size_t n) {
    size_t i = 0, k = 0, len = strlen(name);
    int changed = 0;
    while (i < len && k + 1 < n) {
        if (name[i] == '(' && i + 6 <= len && tolower((unsigned char)name[i + 1]) == 't' && tolower((unsigned char)name[i + 2]) == 'r' &&
            tolower((unsigned char)name[i + 3]) == 'a' && tolower((unsigned char)name[i + 4]) == 'c' &&
            tolower((unsigned char)name[i + 5]) == 'k') {
            size_t j = i + 6;
            if (j < len && isspace((unsigned char)name[j])) {
                while (j < len && isspace((unsigned char)name[j])) j++;
                if (j + 1 < len && name[j] == '1' && name[j + 1] == ')') {
                    size_t m;
                    for (m = i; m < j && k + 1 < n; m++) out[k++] = name[m];
                    if (k + 2 < n) { out[k++] = '2'; out[k++] = ')'; }
                    i = j + 2;
                    changed = 1;
                    continue;
                }
            }
        }
        out[k++] = name[i++];
    }
    out[k] = 0;
    return changed;
}

/* Formats we recognise only to explain them (first bytes of the file). */
static int sniff_format(const char *path, char *what, size_t n) {
    unsigned char b[16];
    FILE *f = pc_fs_fopen(path, "rb");
    size_t got;
    const char *ext = suffix_of(path);
    if (!f) return 0;
    got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got >= 8 && !memcmp(b, "MComprHD", 8)) snprintf(what, n, "CHD");
    else if (got >= 4 && !memcmp(b, "ECM\0", 4)) snprintf(what, n, "ECM");
    else if (got >= 4 && !memcmp(b, "\0PBP", 4)) snprintf(what, n, "PBP");
    else if (got >= 4 && !memcmp(b, "PK\3\4", 4)) snprintf(what, n, "ZIP archive");
    else if (got >= 6 && !memcmp(b, "7z\xBC\xAF\x27\x1C", 6)) snprintf(what, n, "7-Zip archive");
    else if (got >= 4 && !memcmp(b, "Rar!", 4)) snprintf(what, n, "RAR archive");
    else if (ieq(ext, ".chd") || ieq(ext, ".ecm") || ieq(ext, ".pbp") || ieq(ext, ".zip") ||
             ieq(ext, ".7z") || ieq(ext, ".rar") || ieq(ext, ".mds") || ieq(ext, ".mdf") ||
             ieq(ext, ".ccd") || ieq(ext, ".cdi") || ieq(ext, ".nrg") || ieq(ext, ".toc"))
        snprintf(what, n, "%s", ext + 1);
    else
        return 0;
    return 1;
}

static int resolve_sources(const char *image, const char *audio_track, Sources *s, AsError *err) {
    char abs[PATH_N], what[32];
    PcStat st;
    memset(s, 0, sizeof *s);
    s->t1_len = s->a_len = -1;
    s->audio_index = -1;
    if (pc_fs_resolve(image, abs, sizeof abs)) snprintf(abs, sizeof abs, "%s", image);
    set_path(err, abs);
    if (pc_fs_stat(abs, &st) || !st.exists)
        return fail(err, AS_E_NOT_FOUND, "disc image not found: %s", abs);   /* resolve(strict=True) */
    if (st.is_link) {
        FILE *f = pc_fs_fopen(abs, "rb");                /* resolve() follows it */
        if (!f) return fail(err, AS_E_NOT_FOUND, "disc image not found: %s", abs);
        pc_fs_fstat(f, &st);
        fclose(f);
    }
    if (!st.is_reg) return fail(err, AS_E_NOT_FOUND, "disc image must be a regular file: %s", abs);
    if (sniff_format(abs, what, sizeof what)) {
        if (err) snprintf(err->found, sizeof err->found, "%s", what);
        return fail(err, AS_E_FORMAT, "%s images are not supported: supply the raw BIN/CUE", what);
    }
    if (!ieq(suffix_of(abs), ".cue")) {
        char partner[PATH_N];
        const char *base = basename_of(abs);
        snprintf(s->track1, sizeof s->track1, "%s", abs);
        if (audio_track) {
            if (pc_fs_resolve(audio_track, s->audio, sizeof s->audio))
                snprintf(s->audio, sizeof s->audio, "%s", audio_track);
            s->has_audio = 1;
        } else if (partner_name(base, partner, sizeof partner)) {
            snprintf(s->audio, sizeof s->audio, "%.*s%s", (int)(base - abs), abs, partner);
            s->has_audio = strcmp(s->audio, abs) != 0;
        }
        return AS_OK;
    }
    if (audio_track) return fail(err, AS_E_CUE, "use CUE track references OR --audio-track, not both");
    {
        CueTrack *tr = (CueTrack *)calloc(100, sizeof(CueTrack));
        int rc;
        if (!tr) return fail(err, AS_E_WRITE, "out of memory");
        rc = parse_cue(abs, tr, err);
        if (rc) { free(tr); return rc; }
        if (!tr[1].used || strcmp(tr[1].format, "BINARY") || strcmp(tr[1].mode, "MODE2/2352") ||
            tr[1].index1 != 0) {
            free(tr);
            return fail(err, AS_E_CUE, "need BINARY Track 1 MODE2/2352 with INDEX 01 00:00:00");
        }
        if (tr[2].used && (strcmp(tr[2].format, "BINARY") || strcmp(tr[2].mode, "AUDIO") ||
                           tr[2].index1 < 0)) {
            free(tr);
            return fail(err, AS_E_CUE, "need BINARY Track 2 AUDIO with INDEX 01");
        }
        snprintf(s->track1, sizeof s->track1, "%s", tr[1].file);
        set_path(err, s->track1);
        if (pc_fs_stat(s->track1, &st)) {
            free(tr);
            return fail(err, AS_E_NOT_FOUND, "the CUE's Track 1 file is missing: %s", s->track1);
        }
        if (tr[2].used) {
            s->has_audio = 1;
            snprintf(s->audio, sizeof s->audio, "%s", tr[2].file);
            s->audio_index = tr[2].index1;
            if (!strcmp(tr[2].file, tr[1].file)) {
                /* Single-file BIN: Track 2 (with its pregap) starts at its
                 * INDEX 00, else at its INDEX 01. */
                int64_t start = tr[2].index0 >= 0 ? tr[2].index0 : tr[2].index1;
                s->t1_len = start * AS_SECTOR;
                s->a_off = start * AS_SECTOR;
                s->audio_index = tr[2].index1 - start;
            }
        }
        free(tr);
    }
    return AS_OK;
}

/* ------------------------------------------------------------------ */
/* DiscImage                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[256];
    int64_t lba, size;
    int media, cdda, omit;
} Entry;

typedef struct {
    char file[256];
    int64_t lba;
} Anomaly;

typedef struct {
    Sources src;
    const AsProfile *prof;
    FILE *f, *af;
    PcStat st0, ast0;
    Entry *e;
    int n;
    Anomaly anom[8];
    int nanom;
    int audio_omitted;
} Disc;

static int64_t sectors_of(const Entry *e) { return (e->size + 2047) / 2048; }

static int64_t output_size(const Entry *e) {
    if (e->cdda) return sectors_of(e) * AS_SECTOR;
    return e->media ? sectors_of(e) * 2336 : e->size;
}

static int seek_to(FILE *f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int validate_sector(const unsigned char *raw, size_t n, int require_dup) {
    if (n != AS_SECTOR || memcmp(raw, SYNC, 12) || raw[15] != 2 ||
        (require_dup && memcmp(raw + 16, raw + 20, 4)))
        return -1;
    return 0;
}

static int read_sector_user(Disc *d, int64_t lba, unsigned char *out, AsError *err) {
    if (lba < 0) return fail(err, AS_E_LAYOUT, "negative sector address");
    if ((d->src.t1_len >= 0 && (lba + 1) * AS_SECTOR > d->src.t1_len) ||
        seek_to(d->f, d->src.t1_off + lba * AS_SECTOR + 24) || fread(out, 1, 2048, d->f) != 2048)
        return fail(err, AS_E_LAYOUT, "truncated disc sector %lld", (long long)lba);
    return AS_OK;
}

typedef struct { Entry *e; int n, cap; } FileList;

static int list_add(FileList *l, const char *name, int64_t lba, int64_t size) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        Entry *e = (Entry *)realloc(l->e, (size_t)cap * sizeof(Entry));
        if (!e) return -1;
        l->e = e;
        l->cap = cap;
    }
    memset(&l->e[l->n], 0, sizeof(Entry));
    snprintf(l->e[l->n].name, sizeof l->e[l->n].name, "%s", name);
    l->e[l->n].lba = lba;
    l->e[l->n].size = size;
    l->n++;
    return 0;
}

typedef struct { int64_t lba[4096]; int n; } Seen;

/* build_lba_map.walk / read_dir */
static int walk(Disc *d, int64_t lba, int64_t size, const char *prefix, Seen *seen, int depth,
                FileList *out, AsError *err) {
    int64_t sectors, off = 0, i;
    unsigned char *data;
    int k, rc = AS_OK;
    for (k = 0; k < seen->n; k++)
        if (seen->lba[k] == lba) return fail(err, AS_E_LAYOUT, "cyclic or excessive ISO directory tree");
    if (depth > 32 || seen->n >= 4096) return fail(err, AS_E_LAYOUT, "cyclic or excessive ISO directory tree");
    seen->lba[seen->n++] = lba;
    if (!(size > 0 && size <= 2 * 1024 * 1024))
        return fail(err, AS_E_LAYOUT, "ISO directory exceeds 2 MiB or is empty");
    sectors = (size + 2047) / 2048;
    data = (unsigned char *)malloc((size_t)sectors * 2048);
    if (!data) return fail(err, AS_E_WRITE, "out of memory");
    for (i = 0; i < sectors; i++)
        if ((rc = read_sector_user(d, lba + i, data + i * 2048, err))) { free(data); return rc; }
    while (off < size) {
        int rec_len = data[off], name_len, is_dir;
        int64_t ext_lba, ext_size;
        char name[256], full[512];
        if (rec_len == 0) {
            off = (off / 2048 + 1) * 2048;
            continue;
        }
        if (rec_len < 34 || off + rec_len > size || off % 2048 + rec_len > 2048) {
            free(data);
            return fail(err, AS_E_LAYOUT, "invalid ISO directory record length");
        }
        ext_lba = (int64_t)data[off + 2] | ((int64_t)data[off + 3] << 8) |
                  ((int64_t)data[off + 4] << 16) | ((int64_t)data[off + 5] << 24);
        ext_size = (int64_t)data[off + 10] | ((int64_t)data[off + 11] << 8) |
                   ((int64_t)data[off + 12] << 16) | ((int64_t)data[off + 13] << 24);
        is_dir = (data[off + 25] & 2) != 0;
        name_len = data[off + 32];
        if (name_len == 0 || 33 + name_len > rec_len) {
            free(data);
            return fail(err, AS_E_LAYOUT, "invalid ISO directory name length");
        }
        {
            /* name.split(';')[0]; a byte >= 0x80 decodes to U+FFFD in
             * Python and is kept as 0x80 here, so safe_relative rejects the
             * name the same way.  A NUL byte stays a byte ("\x00" = .). */
            int j, len = 0;
            for (j = 0; j < name_len; j++) {
                unsigned char c = data[off + 33 + j];
                if (c == ';') break;
                name[len++] = (char)(c >= 0x80 ? 0x80 : c);
            }
            name[len] = 0;
            off += rec_len;
            if ((len == 1 && (name[0] == 0 || name[0] == 1))) continue;   /* . and .. */
            if ((int)strlen(name) != len) {
                free(data);
                return fail(err, AS_E_LAYOUT, "unsafe asset path: NUL in ISO name");
            }
        }
        if (prefix[0]) snprintf(full, sizeof full, "%s/%s", prefix, name);
        else snprintf(full, sizeof full, "%s", name);
        if (is_dir) {
            if ((rc = walk(d, ext_lba, ext_size, full, seen, depth + 1, out, err))) { free(data); return rc; }
        } else if (list_add(out, full, ext_lba, ext_size)) {
            free(data);
            return fail(err, AS_E_WRITE, "out of memory");
        }
        if (out->n > 100000) { free(data); return fail(err, AS_E_LAYOUT, "too many ISO files"); }
    }
    free(data);
    return AS_OK;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    int c = strcmp(x->name, y->name);
    if (c) return c;
    if (x->lba != y->lba) return x->lba < y->lba ? -1 : 1;
    return x->size < y->size ? -1 : x->size > y->size;
}

typedef int (*BlockFn)(void *ud, const unsigned char *p, size_t n);

/* DiscImage.blocks */
static int read_entry(Disc *d, Entry *e, BlockFn fn, void *ud, AsCtl *ctl, AsError *err) {
    unsigned char raw[AS_SECTOR];
    int64_t i, remaining = e->size, sectors = sectors_of(e);
    if (e->cdda) {
        if (seek_to(d->af, d->src.a_off + d->prof->audio_skip * AS_SECTOR))
            return fail(err, AS_E_LAYOUT, "truncated CD-DA audio sector");
        for (i = 0; i < sectors; i++) {
            if (cancelled(ctl)) return fail(err, AS_E_CANCELLED, "cancelled");
            if (fread(raw, 1, AS_SECTOR, d->af) != AS_SECTOR)
                return fail(err, AS_E_LAYOUT, "truncated CD-DA audio sector");
            if (fn(ud, raw, AS_SECTOR)) return fail(err, AS_E_WRITE, "write failed: %s", strerror(errno));
        }
        return AS_OK;
    }
    if (seek_to(d->f, d->src.t1_off + e->lba * AS_SECTOR))
        return fail(err, AS_E_LAYOUT, "invalid/truncated Mode2 sector or duplicated XA subheader");
    for (i = 0; i < sectors; i++) {
        int64_t lba = e->lba + i;
        int preserve = e->media && lba == d->prof->preserved_subheader_lba;
        size_t got;
        if (cancelled(ctl)) return fail(err, AS_E_CANCELLED, "cancelled");
        got = fread(raw, 1, AS_SECTOR, d->f);
        if (validate_sector(raw, got, !preserve))
            return fail(err, AS_E_LAYOUT, "invalid/truncated Mode2 sector or duplicated XA subheader");
        if (preserve && memcmp(raw + 16, raw + 20, 4) && d->nanom < 8) {
            snprintf(d->anom[d->nanom].file, sizeof d->anom[d->nanom].file, "%s", e->name);
            d->anom[d->nanom].lba = lba;
            d->nanom++;
        }
        if (e->media) {
            if (fn(ud, raw + 16, AS_SECTOR - 16)) return fail(err, AS_E_WRITE, "write failed: %s", strerror(errno));
        } else {
            int64_t take = remaining < 2048 ? remaining : 2048;
            if (raw[18] & 0x20) return fail(err, AS_E_LAYOUT, "unexpected Form2 data in %s", e->name);
            if (fn(ud, raw + 24, (size_t)take)) return fail(err, AS_E_WRITE, "write failed: %s", strerror(errno));
            remaining -= take;
        }
    }
    return AS_OK;
}

static int hash_stream(FILE *f, int64_t off, int64_t len, char hex[41], AsCtl *ctl, AsError *err) {
    static unsigned char buf[1 << 20];
    PcSha1 c;
    uint8_t dg[20];
    pc_sha1_init(&c);
    if (seek_to(f, off)) return fail(err, AS_E_UNREADABLE, "cannot read the image");
    while (len > 0) {
        size_t want = (int64_t)sizeof buf > len ? (size_t)len : sizeof buf, got;
        if (cancelled(ctl)) return fail(err, AS_E_CANCELLED, "cancelled");
        got = fread(buf, 1, want, f);
        if (!got) {
            if (ferror(f)) return fail(err, AS_E_UNREADABLE, "read error: %s", strerror(errno));
            break;
        }
        pc_sha1_update(&c, buf, got);
        progress(ctl, (int64_t)got);
        len -= (int64_t)got;
    }
    pc_sha1_final(&c, dg);
    pc_hash_hex(dg, 20, hex);
    return AS_OK;
}

typedef struct { uint8_t *buf; size_t n, cap; } MemSink;

static int mem_sink(void *ud, const unsigned char *p, size_t n) {
    MemSink *m = (MemSink *)ud;
    if (m->n + n > m->cap) return -1;
    memcpy(m->buf + m->n, p, n);
    m->n += n;
    return 0;
}

static void disc_close(Disc *d) {
    if (d->f) fclose(d->f);
    if (d->af) fclose(d->af);
    free(d->e);
    d->f = d->af = NULL;
    d->e = NULL;
}

static int64_t track1_size(Disc *d) {
    return d->src.t1_len >= 0 ? d->src.t1_len : d->st0.size - d->src.t1_off;
}

static int disc_fail(Disc *d, FileList *files, int rc) {
    if (files) { free(files->e); files->e = NULL; }
    disc_close(d);
    return rc;
}

/* DiscImage.__enter__ */
static int disc_open(Disc *d, const char *image, const char *audio_track, const AsProfile *prof,
                     AsCtl *ctl, AsError *err) {
    int rc, i;
    unsigned char sector[AS_SECTOR], pvd[2048];
    char hex[41];
    int64_t root_lba, root_size, total = 0;
    FileList files = {0};
    Seen *seen;
    memset(d, 0, sizeof *d);
    d->prof = prof;
    if ((rc = resolve_sources(image, audio_track, &d->src, err))) return rc;
    set_path(err, d->src.track1);
    d->f = pc_fs_fopen(d->src.track1, "rb");
    if (!d->f) {
        rc = errno == ENOENT ? AS_E_NOT_FOUND : AS_E_UNREADABLE;
        return fail(err, rc, "cannot open %s: %s", d->src.track1, strerror(errno));
    }
    pc_fs_fstat(d->f, &d->st0);
    if (track1_size(d) != prof->track_size) {
        int64_t size = track1_size(d);
        const char *ext = suffix_of(d->src.track1);
        int iso = 0;
        if (ieq(ext, ".iso") || size == (prof->track_size / AS_SECTOR) * 2048) {
            iso = 1;
        } else if (size % AS_SECTOR && !(size % 2048) && size >= 17 * 2048) {
            unsigned char b[8];
            if (!seek_to(d->f, 16 * 2048) && fread(b, 1, 8, d->f) == 8 && !memcmp(b, "\x01" "CD001", 6))
                iso = 1;
        }
        if (err) {
            snprintf(err->found, sizeof err->found, "%lld", (long long)size);
            snprintf(err->expected, sizeof err->expected, "%lld", (long long)prof->track_size);
        }
        disc_close(d);
        if (iso)
            return fail(err, AS_E_COOKED_ISO, "unsupported image size: supply the supported raw MODE2/2352 "
                        "Track 1 BIN (or its CUE), not a cooked 2048-byte ISO/CHD");
        if (prof->audio_size && size == prof->audio_size)
            return fail(err, AS_E_AUDIO_TRACK_GIVEN, "this is the CD audio track (Track 2); "
                        "supply the CUE or the Track 1 BIN");
        return fail(err, AS_E_WRONG_SIZE, "unsupported image size: supply the supported raw MODE2/2352 "
                    "Track 1 BIN (or its CUE), not a cooked 2048-byte ISO/CHD");
    }
    if (ctl) {
        ctl->phase = AS_PH_HASH;
        if (!ctl->total) ctl->total = prof->track_size + prof->audio_size;
    }
    if (ctl) {
        snprintf(ctl->track1_path, sizeof ctl->track1_path, "%s", d->src.track1);
        snprintf(ctl->audio_path, sizeof ctl->audio_path, "%s", d->src.has_audio ? d->src.audio : "");
    }
    if ((rc = hash_stream(d->f, d->src.t1_off, prof->track_size, hex, ctl, err))) return disc_fail(d, NULL, rc);
    if (ctl) snprintf(ctl->track_sha1, sizeof ctl->track_sha1, "%s", hex);
    if (strcmp(hex, prof->track_sha1)) {
        if (err) {
            snprintf(err->found, sizeof err->found, "%s", hex);
            snprintf(err->expected, sizeof err->expected, "%s", prof->track_sha1);
        }
        disc_close(d);
        return fail(err, AS_E_WRONG_SHA1, "unsupported Track 1 SHA1: got %s, expected %s; patched or "
                    "different releases are not supported", hex, prof->track_sha1);
    }
    if (prof->audio_size) {
        PcStat ast;
        int present = 0;
        /* allow_no_audio 2: the player chose to go on without a Track 2
         * that is present but differs */
        if (d->src.has_audio && !(ctl && ctl->allow_no_audio == 2)) {
            FILE *t = pc_fs_fopen(d->src.audio, "rb");
            if (t) { present = !pc_fs_fstat(t, &ast) && ast.is_reg; fclose(t); }
        }
        if (!present) {
            if (!(ctl && ctl->allow_no_audio)) {
                set_path(err, d->src.has_audio ? d->src.audio : d->src.track1);
                disc_close(d);
                return fail(err, AS_E_NO_TRACK2, "complete extraction needs CD-DA Track 2; supply the "
                            "complete CUE or --audio-track /path/to/Track2.bin");
            }
            d->audio_omitted = 1;
            progress(ctl, prof->audio_size);
        } else {
            int64_t asize;
            set_path(err, d->src.audio);
            if (d->src.audio_index >= 0 && d->src.audio_index != prof->audio_skip) {
                disc_close(d);
                return fail(err, AS_E_TRACK2, "unsupported Track 2 INDEX 01/pregap");
            }
            d->af = pc_fs_fopen(d->src.audio, "rb");
            if (!d->af) {
                disc_close(d);
                return fail(err, AS_E_UNREADABLE, "cannot open %s: %s", d->src.audio, strerror(errno));
            }
            pc_fs_fstat(d->af, &d->ast0);
            asize = d->ast0.size - d->src.a_off;
            if (asize != prof->audio_size) {
                if (err) {
                    snprintf(err->found, sizeof err->found, "%lld bytes", (long long)asize);
                    snprintf(err->expected, sizeof err->expected, "%lld bytes", (long long)prof->audio_size);
                }
                disc_close(d);
                return fail(err, AS_E_TRACK2, "unsupported CD-DA Track 2 size/SHA1: got %lld bytes, expected %lld",
                            (long long)asize, (long long)prof->audio_size);
            }
            if (ctl) ctl->phase = AS_PH_HASH_AUDIO;
            if ((rc = hash_stream(d->af, d->src.a_off, prof->audio_size, hex, ctl, err))) return disc_fail(d, NULL, rc);
            if (ctl) snprintf(ctl->audio_sha1, sizeof ctl->audio_sha1, "%s", hex);
            if (strcmp(hex, prof->audio_sha1)) {
                if (err) {
                    snprintf(err->found, sizeof err->found, "%s", hex);
                    snprintf(err->expected, sizeof err->expected, "%s", prof->audio_sha1);
                }
                disc_close(d);
                return fail(err, AS_E_TRACK2, "unsupported CD-DA Track 2 size/SHA1: got %s, expected %s",
                            hex, prof->audio_sha1);
            }
        }
    }
    if (ctl) ctl->phase = AS_PH_CHECK;
    set_path(err, d->src.track1);
    if (seek_to(d->f, d->src.t1_off) || validate_sector(sector, fread(sector, 1, AS_SECTOR, d->f), 1)) {
        disc_close(d);
        return fail(err, AS_E_LAYOUT, "invalid/truncated Mode2 sector or duplicated XA subheader");
    }
    if ((rc = read_sector_user(d, 16, pvd, err))) return disc_fail(d, NULL, rc);
    if (memcmp(pvd, "\x01" "CD001\x01", 7) || (pvd[128] | (pvd[129] << 8)) != 2048) {
        disc_close(d);
        return fail(err, AS_E_LAYOUT, "invalid ISO9660 primary volume descriptor");
    }
    root_lba = (int64_t)pvd[158] | ((int64_t)pvd[159] << 8) | ((int64_t)pvd[160] << 16) | ((int64_t)pvd[161] << 24);
    root_size = (int64_t)pvd[166] | ((int64_t)pvd[167] << 8) | ((int64_t)pvd[168] << 16) | ((int64_t)pvd[169] << 24);
    seen = (Seen *)calloc(1, sizeof(Seen));
    rc = seen ? walk(d, root_lba, root_size, "", seen, 0, &files, err) : fail(err, AS_E_WRITE, "out of memory");
    free(seen);
    if (rc) return disc_fail(d, &files, rc);
    if (files.n != prof->file_count)
        return disc_fail(d, &files, fail(err, AS_E_LAYOUT, "filesystem inventory differs from supported disc"));
    qsort(files.e, (size_t)files.n, sizeof(Entry), entry_cmp);
    for (i = 0; i < files.n; i++) {
        Entry *e = &files.e[i];
        const char *sfx;
        int j;
        if (as_safe_relative(e->name))
            return disc_fail(d, &files, fail(err, AS_E_LAYOUT, "unsafe asset path: '%s'", e->name));
        for (j = 0; j < i; j++)
            if (!icmp(files.e[j].name, e->name))
                return disc_fail(d, &files, fail(err, AS_E_LAYOUT, "duplicate/case-colliding ISO path: %s", e->name));
        sfx = suffix_of(e->name);
        e->media = !strcmp(sfx, ".XA") || !strcmp(sfx, ".STR");
        e->cdda = !strcmp(sfx, ".DA");
        if (e->cdda) {
            if (d->audio_omitted && e->lba == prof->cdda_lba && e->size == prof->cdda_size &&
                !strcmp(e->name, AS_CDDA_NAME)) {
                e->omit = 1;
            } else if (!d->af || e->lba != prof->cdda_lba || e->size != prof->cdda_size ||
                       (prof->audio_skip + sectors_of(e)) * AS_SECTOR != prof->audio_size) {
                return disc_fail(d, &files, fail(err, AS_E_LAYOUT, "unsupported CD-DA filesystem reference"));
            }
        } else if (e->size < 0 || e->lba < 0 || (e->lba + sectors_of(e)) * AS_SECTOR > prof->track_size) {
            return disc_fail(d, &files, fail(err, AS_E_LAYOUT, "out-of-range file extent: %s", e->name));
        }
        if ((e->media || e->cdda) && e->size % 2048)
            return disc_fail(d, &files, fail(err, AS_E_LAYOUT, "media extent is not sector-aligned: %s", e->name));
        if (!e->omit) total += output_size(e);
    }
    d->e = files.e;
    d->n = files.n;
    if (total > MAX_OUTPUT) { disc_close(d); return fail(err, AS_E_LAYOUT, "extracted data exceeds 2 GiB"); }
    {
        Entry *exe = NULL;
        MemSink m;
        uint8_t dg[20];
        PcSha1 c;
        int64_t load, len;
        for (i = 0; i < d->n; i++)
            if (!strcmp(d->e[i].name, "PSX.EXE")) { exe = &d->e[i]; break; }
        if (!exe || exe->lba != prof->exe_lba || exe->size != prof->exe_size) {
            disc_close(d);
            return fail(err, AS_E_LAYOUT, "executable location/size differs from supported layout");
        }
        m.cap = (size_t)output_size(exe);
        m.n = 0;
        m.buf = (uint8_t *)malloc(m.cap ? m.cap : 1);
        if (!m.buf) { disc_close(d); return fail(err, AS_E_WRITE, "out of memory"); }
        if ((rc = read_entry(d, exe, mem_sink, &m, ctl, err))) { free(m.buf); return disc_fail(d, NULL, rc); }
        pc_sha1_init(&c);
        pc_sha1_update(&c, m.buf, m.n);
        pc_sha1_final(&c, dg);
        pc_hash_hex(dg, 20, hex);
        if (ctl) snprintf(ctl->exe_sha1, sizeof ctl->exe_sha1, "%s", hex);
        rc = AS_OK;
        if (strcmp(hex, prof->exe_sha1)) rc = fail(err, AS_E_LAYOUT, "executable SHA1 mismatch");
        else if (m.n < 0x800 || memcmp(m.buf, "PS-X EXE", 8)) rc = fail(err, AS_E_LAYOUT, "invalid PS-X EXE");
        else {
            load = (int64_t)m.buf[0x18] | ((int64_t)m.buf[0x19] << 8) | ((int64_t)m.buf[0x1a] << 16) | ((int64_t)m.buf[0x1b] << 24);
            len = (int64_t)m.buf[0x1c] | ((int64_t)m.buf[0x1d] << 8) | ((int64_t)m.buf[0x1e] << 16) | ((int64_t)m.buf[0x1f] << 24);
            if (!(0x80000000LL <= load && load < load + len && load + len <= 0x80200000LL &&
                  len <= (int64_t)m.n - 0x800))
                rc = fail(err, AS_E_LAYOUT, "invalid executable load range");
        }
        free(m.buf);
        if (rc) return disc_fail(d, NULL, rc);
    }
    return AS_OK;
}

static int same_identity(const PcStat *a, const PcStat *b, int full) {
    if (a->size != b->size || a->mtime_ns != b->mtime_ns) return 0;
    if (!full) return 1;
    return a->dev == b->dev && a->ino == b->ino && a->ctime_ns == b->ctime_ns;
}

/* DiscImage.unchanged: the open handle and the path both still name the
 * same, unmodified file. */
static int disc_unchanged(Disc *d, AsError *err) {
    FILE *fs[2];
    const char *paths[2];
    PcStat *init[2];
    int k, n = 1;
    fs[0] = d->f; paths[0] = d->src.track1; init[0] = &d->st0;
    if (d->af) { fs[1] = d->af; paths[1] = d->src.audio; init[1] = &d->ast0; n = 2; }
    for (k = 0; k < n; k++) {
        PcStat now, by_path;
        FILE *t;
        if (pc_fs_fstat(fs[k], &now) || !same_identity(&now, init[k], 1))
            return fail(err, AS_E_CHANGED, "source image changed during extraction");
        t = pc_fs_fopen(paths[k], "rb");
        if (!t) return fail(err, AS_E_CHANGED, "source image changed during extraction");
        pc_fs_fstat(t, &by_path);
        fclose(t);
        if (!same_identity(&by_path, init[k], 1))
            return fail(err, AS_E_CHANGED, "source image changed during extraction");
    }
    return AS_OK;
}

int as_check_image(const char *image, const char *audio_track, const AsProfile *profile,
                   AsCtl *ctl, AsError *err) {
    Disc d;
    int rc = disc_open(&d, image, audio_track, profile ? profile : &as_supported_profile, ctl, err);
    if (!rc) {
        if (ctl) ctl->audio_omitted = d.audio_omitted;
        disc_close(&d);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* manifest rows, aliases                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[300];
    int64_t size;
    char sha256[65];
} Row;

typedef struct { Row *r; int n, cap; } Rows;

static int rows_add(Rows *rs, const char *path, int64_t size, const char *sha) {
    if (rs->n == rs->cap) {
        int cap = rs->cap ? rs->cap * 2 : 1024;
        Row *r = (Row *)realloc(rs->r, (size_t)cap * sizeof(Row));
        if (!r) return -1;
        rs->r = r;
        rs->cap = cap;
    }
    snprintf(rs->r[rs->n].path, sizeof rs->r[rs->n].path, "%s", path);
    rs->r[rs->n].size = size;
    snprintf(rs->r[rs->n].sha256, sizeof rs->r[rs->n].sha256, "%s", sha);
    rs->n++;
    return 0;
}

static int row_cmp(const void *a, const void *b) {
    return strcmp(((const Row *)a)->path, ((const Row *)b)->path);
}

/* bootstrap_assets.alias_for */
static int alias_for(const char *name, char *out, size_t n) {
    static const struct { const char *sfx, *cat; } cats[] = {
        {".EXN", "overlays"}, {".NBN", "nbn"}, {".VH", "music"}, {".VB", "music"},
        {".SEQ", "music"}, {".XA", "audio"}, {".DA", "audio"}, {".STR", "video"},
        {".TIM", "textures"},
    };
    const char *sfx;
    size_t i;
    if (!strcmp(name, "PSX.EXE")) { snprintf(out, n, "data/PSX.EXE"); return 1; }
    sfx = suffix_of(name);
    for (i = 0; i < sizeof cats / sizeof cats[0]; i++)
        if (!strcmp(sfx, cats[i].sfx)) {
            snprintf(out, n, "%s/%s", cats[i].cat, basename_of(name));
            return 1;
        }
    return 0;
}

/* checked_path: no component of root/name may be a symlink. */
static int checked_path(const char *root, const char *name, char *out, size_t n, AsError *err) {
    char tmp[PATH_N];
    const char *p = name;
    PcStat st;
    if (as_safe_relative(name)) return fail(err, AS_E_MANIFEST, "unsafe asset path: '%s'", name);
    snprintf(out, n, "%s", root);
    for (;;) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        snprintf(tmp, sizeof tmp, "%s/%.*s", out, (int)len, p);
        snprintf(out, n, "%s", tmp);
        if (!pc_fs_stat(out, &st) && st.is_link)
            return fail(err, AS_E_MANIFEST, "asset path must not contain symlinks: %s", name);
        if (!end) break;
        p = end + 1;
    }
    return AS_OK;
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '\b') fputs("\\b", f);
        else if (c == '\f') fputs("\\f", f);
        else if (c < 0x20 || c >= 0x7f) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* json.dumps(result, indent=2, sort_keys=True) + '\n' */
static int write_manifest(const char *path, const AsProfile *p, Rows *rs, Disc *d, int omitted) {
    FILE *f = pc_fs_create_excl(path);
    int i;
    if (!f) return -1;
    fputs("{\n  \"complete\": true,\n  \"files\": [\n", f);
    for (i = 0; i < rs->n; i++) {
        fputs("    {\n      \"path\": ", f);
        json_str(f, rs->r[i].path);
        fprintf(f, ",\n      \"sha256\": \"%s\",\n      \"size\": %lld\n    }%s\n",
                rs->r[i].sha256, (long long)rs->r[i].size, i + 1 < rs->n ? "," : "");
    }
    fputs("  ],\n", f);
    if (omitted) {
        fputs("  \"omitted_files\": [\n    ", f);
        json_str(f, AS_CDDA_NAME);
        fputs("\n  ],\n", f);
    }
    if (d->nanom == 0) {
        fputs("  \"preserved_source_anomalies\": [],\n", f);
    } else {
        fputs("  \"preserved_source_anomalies\": [\n", f);
        for (i = 0; i < d->nanom; i++) {
            fputs("    {\n      \"file\": ", f);
            json_str(f, d->anom[i].file);
            fprintf(f, ",\n      \"lba\": %lld,\n      \"reason\": ", (long long)d->anom[i].lba);
            json_str(f, ANOMALY_REASON);
            fprintf(f, "\n    }%s\n", i + 1 < d->nanom ? "," : "");
        }
        fputs("  ],\n", f);
    }
    fprintf(f, "  \"schema\": 1,\n  \"source\": {\n"
               "    \"audio_sha1\": \"%s\",\n    \"audio_size\": %lld,\n    \"audio_skip\": %lld,\n"
               "    \"cdda_lba\": %lld,\n    \"cdda_size\": %lld,\n    \"exe_lba\": %lld,\n"
               "    \"exe_sha1\": \"%s\",\n    \"exe_size\": %lld,\n    \"file_count\": %lld,\n"
               "    \"preserved_subheader_lba\": %lld,\n    \"track_sha1\": \"%s\",\n"
               "    \"track_size\": %lld\n  }\n}\n",
            p->audio_sha1, (long long)p->audio_size, (long long)p->audio_skip,
            (long long)p->cdda_lba, (long long)p->cdda_size, (long long)p->exe_lba,
            p->exe_sha1, (long long)p->exe_size, (long long)p->file_count,
            (long long)p->preserved_subheader_lba, p->track_sha1, (long long)p->track_size);
    if (ferror(f)) { fclose(f); return -1; }
    return fclose(f) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* a small JSON reader (the manifest only)                             */
/* ------------------------------------------------------------------ */

enum { J_NULL, J_TRUE, J_FALSE, J_INT, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct JNode {
    int type;
    int64_t i;
    char *s;
    int n;
    char **keys;
    struct JNode **kids;
} JNode;

typedef struct { const char *p, *end; int depth; } JParser;

static void jfree(JNode *n) {
    int k;
    if (!n) return;
    for (k = 0; k < n->n; k++) {
        jfree(n->kids[k]);
        if (n->keys) free(n->keys[k]);
    }
    free(n->kids);
    free(n->keys);
    free(n->s);
    free(n);
}

static void jws(JParser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}

static char *jstring(JParser *j) {
    size_t cap = 64, n = 0;
    char *out;
    if (j->p >= j->end || *j->p != '"') return NULL;
    j->p++;
    out = (char *)malloc(cap);
    while (out && j->p < j->end && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c < 0x20) { free(out); return NULL; }
        if (c == '\\') {
            if (j->p >= j->end) { free(out); return NULL; }
            c = (unsigned char)*j->p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned v = 0;
                int k;
                if (j->end - j->p < 4) { free(out); return NULL; }
                for (k = 0; k < 4; k++) {
                    char h = *j->p++;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' :
                                            (tolower((unsigned char)h) - 'a' + 10));
                }
                c = v < 0x80 ? v : 0x80;            /* non-ASCII: never a valid asset name */
                break;
            }
            default: free(out); return NULL;
            }
        }
        if (n + 2 > cap) {
            char *g = (char *)realloc(out, cap *= 2);
            if (!g) { free(out); return NULL; }
            out = g;
        }
        out[n++] = (char)c;
    }
    if (!out || j->p >= j->end) { free(out); return NULL; }
    j->p++;
    out[n] = 0;
    return out;
}

static JNode *jvalue(JParser *j);

static int jadd(JNode *n, char *key, JNode *kid) {
    JNode **k = (JNode **)realloc(n->kids, (size_t)(n->n + 1) * sizeof(JNode *));
    if (!k) return -1;
    n->kids = k;
    if (n->type == J_OBJ) {
        char **ks = (char **)realloc(n->keys, (size_t)(n->n + 1) * sizeof(char *));
        if (!ks) return -1;
        n->keys = ks;
        n->keys[n->n] = key;
    }
    n->kids[n->n++] = kid;
    return 0;
}

static JNode *jvalue(JParser *j) {
    JNode *n;
    jws(j);
    if (j->p >= j->end || ++j->depth > 32) return NULL;
    n = (JNode *)calloc(1, sizeof(JNode));
    if (!n) return NULL;
    if (*j->p == '{' || *j->p == '[') {
        int obj = *j->p == '{';
        char close = obj ? '}' : ']';
        n->type = obj ? J_OBJ : J_ARR;
        j->p++;
        jws(j);
        if (j->p < j->end && *j->p == close) { j->p++; j->depth--; return n; }
        for (;;) {
            char *key = NULL;
            JNode *kid;
            jws(j);
            if (obj) {
                key = jstring(j);
                jws(j);
                if (!key || j->p >= j->end || *j->p != ':') { free(key); jfree(n); return NULL; }
                j->p++;
            }
            kid = jvalue(j);
            if (!kid || jadd(n, key, kid)) { free(key); jfree(kid); jfree(n); return NULL; }
            jws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == close) { j->p++; break; }
            jfree(n);
            return NULL;
        }
    } else if (*j->p == '"') {
        n->type = J_STR;
        if (!(n->s = jstring(j))) { jfree(n); return NULL; }
    } else if (!strncmp(j->p, "true", 4)) { n->type = J_TRUE; j->p += 4; }
    else if (!strncmp(j->p, "false", 5)) { n->type = J_FALSE; j->p += 5; }
    else if (!strncmp(j->p, "null", 4)) { n->type = J_NULL; j->p += 4; }
    else if (*j->p == '-' || isdigit((unsigned char)*j->p)) {
        const char *s = j->p;
        int neg = *s == '-';
        int64_t v = 0;
        if (neg) s++;
        if (!isdigit((unsigned char)*s)) { jfree(n); return NULL; }
        while (s < j->end && isdigit((unsigned char)*s)) {
            if (v > (INT64_MAX - 9) / 10) { jfree(n); return NULL; }
            v = v * 10 + (*s++ - '0');
        }
        n->type = J_INT;
        if (s < j->end && (*s == '.' || *s == 'e' || *s == 'E')) {
            n->type = J_NUM;                       /* a float: never a valid size */
            s++;
            while (s < j->end && (isdigit((unsigned char)*s) || strchr("+-eE", *s))) s++;
        }
        n->i = neg ? -v : v;
        j->p = s;
    } else {
        jfree(n);
        return NULL;
    }
    j->depth--;
    return n;
}

static JNode *jget(const JNode *o, const char *key) {
    int k;
    JNode *hit = NULL;
    if (!o || o->type != J_OBJ) return NULL;
    for (k = 0; k < o->n; k++)
        if (!strcmp(o->keys[k], key)) hit = o->kids[k];   /* the last one wins, as in Python */
    return hit;
}

static int jstr_is(const JNode *n, const char *s) { return n && n->type == J_STR && !strcmp(n->s, s); }
static int jint_is(const JNode *n, int64_t v) { return n && n->type == J_INT && n->i == v; }

/* data.get('source') != asdict(profile) */
static int source_matches(const JNode *s, const AsProfile *p) {
    return s && s->type == J_OBJ && s->n == 12 &&
           jint_is(jget(s, "track_size"), p->track_size) && jstr_is(jget(s, "track_sha1"), p->track_sha1) &&
           jint_is(jget(s, "exe_lba"), p->exe_lba) && jint_is(jget(s, "exe_size"), p->exe_size) &&
           jstr_is(jget(s, "exe_sha1"), p->exe_sha1) && jint_is(jget(s, "file_count"), p->file_count) &&
           jint_is(jget(s, "audio_size"), p->audio_size) && jstr_is(jget(s, "audio_sha1"), p->audio_sha1) &&
           jint_is(jget(s, "audio_skip"), p->audio_skip) && jint_is(jget(s, "cdda_lba"), p->cdda_lba) &&
           jint_is(jget(s, "cdda_size"), p->cdda_size) &&
           jint_is(jget(s, "preserved_subheader_lba"), p->preserved_subheader_lba);
}

/* ------------------------------------------------------------------ */
/* verify_cache                                                        */
/* ------------------------------------------------------------------ */

static int file_digest(const char *path, int sha256, char *hex, AsCtl *ctl, AsError *err) {
    static unsigned char buf[1 << 20];
    FILE *f = pc_fs_fopen(path, "rb");
    size_t got;
    PcSha256 c2;
    PcSha1 c1;
    uint8_t dg[32];
    if (!f) return fail(err, AS_E_CORRUPT, "missing or corrupt asset: %s", path);
    if (sha256) pc_sha256_init(&c2); else pc_sha1_init(&c1);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (cancelled(ctl)) { fclose(f); return fail(err, AS_E_CANCELLED, "cancelled"); }
        if (sha256) pc_sha256_update(&c2, buf, got); else pc_sha1_update(&c1, buf, got);
        if (sha256) progress(ctl, (int64_t)got);
    }
    if (ferror(f)) { fclose(f); return fail(err, AS_E_CORRUPT, "cannot read %s", path); }
    fclose(f);
    if (sha256) { pc_sha256_final(&c2, dg); pc_hash_hex(dg, 32, hex); }
    else { pc_sha1_final(&c1, dg); pc_hash_hex(dg, 20, hex); }
    return AS_OK;
}

static int is_hex64(const char *s) {
    int k;
    for (k = 0; k < 64; k++)
        if (!s[k] || !strchr("0123456789abcdef", s[k])) return 0;
    return s[64] == 0;
}

static int row_icmp(const void *a, const void *b) {
    return icmp(((const Row *)a)->path, ((const Row *)b)->path);
}

static const Row *find_row(const Rows *sorted, const char *path) {
    int lo = 0, hi = sorted->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(sorted->r[mid].path, path);
        if (!c) return &sorted->r[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int as_verify(const char *dest, const AsProfile *profile, AsCtl *ctl, AsError *err) {
    const AsProfile *p = profile ? profile : &as_supported_profile;
    int fast = ctl && ctl->fast_verify;
    char mpath[PATH_N], hex[65];
    PcStat st;
    FILE *f;
    char *text = NULL;
    JNode *root = NULL, *files, *anoms, *omitted;
    Rows rows = {0};
    int rc = AS_OK, k, raw_n = 0, have_exe = 0, omit = 0, expected_n = 0;
    int64_t total = 0;
    set_path(err, dest);
    if (pc_fs_stat(dest, &st) || st.is_link || !st.is_dir)
        return fail(err, AS_E_MANIFEST, "asset directory is missing or is a symlink");
    if ((rc = checked_path(dest, AS_MANIFEST, mpath, sizeof mpath, err))) return rc;
    set_path(err, mpath);
    if (pc_fs_stat(mpath, &st) || !st.is_reg || st.size > 4 * 1024 * 1024)
        return fail(err, AS_E_MANIFEST, "asset completion manifest is missing or exceeds 4 MiB");
    f = pc_fs_fopen(mpath, "rb");
    if (!f) return fail(err, AS_E_MANIFEST, "cannot read %s", mpath);
    text = (char *)malloc((size_t)st.size + 1);
    if (!text || fread(text, 1, (size_t)st.size, f) != (size_t)st.size) {
        fclose(f); free(text);
        return fail(err, AS_E_MANIFEST, "cannot read %s", mpath);
    }
    fclose(f);
    text[st.size] = 0;
    {
        JParser j;
        j.p = text;
        j.end = text + st.size;
        j.depth = 0;
        root = jvalue(&j);
        if (root) { jws(&j); if (j.p != j.end) { jfree(root); root = NULL; } }
    }
    free(text);
    if (!root || root->type != J_OBJ || !jint_is(jget(root, "schema"), 1) ||
        !jget(root, "complete") || jget(root, "complete")->type != J_TRUE ||
        !source_matches(jget(root, "source"), p)) {
        rc = fail(err, AS_E_MANIFEST, "asset manifest has unsupported schema or disc identity");
        goto out;
    }
    files = jget(root, "files");
    if (!files || files->type != J_ARR || files->n <= 0 || files->n > 10000) {
        rc = fail(err, AS_E_MANIFEST, "invalid asset manifest file list");
        goto out;
    }
    omitted = jget(root, "omitted_files");
    if (omitted) {
        /* the C setup's no-Track-2 extension (asset_setup.h) */
        if (omitted->type != J_ARR || omitted->n != 1 || !jstr_is(omitted->kids[0], AS_CDDA_NAME)) {
            rc = fail(err, AS_E_MANIFEST, "invalid omitted_files record");
            goto out;
        }
        omit = 1;
    }
    for (k = 0; k < files->n; k++) {
        JNode *r = files->kids[k], *pa = jget(r, "path"), *sz = jget(r, "size"), *sh = jget(r, "sha256");
        if (!pa || pa->type != J_STR || as_safe_relative(pa->s) || !sz || sz->type != J_INT || sz->i < 0 ||
            !sh || sh->type != J_STR || !is_hex64(sh->s) || strlen(pa->s) >= sizeof rows.r[0].path) {
            rc = fail(err, AS_E_MANIFEST, "duplicate or invalid asset manifest entry");
            goto out;
        }
        if (rows_add(&rows, pa->s, sz->i, sh->s)) { rc = fail(err, AS_E_WRITE, "out of memory"); goto out; }
        total += sz->i;
    }
    qsort(rows.r, (size_t)rows.n, sizeof(Row), row_icmp);
    for (k = 1; k < rows.n; k++)
        if (!icmp(rows.r[k - 1].path, rows.r[k].path)) {
            rc = fail(err, AS_E_MANIFEST, "duplicate or invalid asset manifest entry");
            goto out;
        }
    if (total > MAX_OUTPUT) { rc = fail(err, AS_E_MANIFEST, "asset cache exceeds 2 GiB verification budget"); goto out; }
    qsort(rows.r, (size_t)rows.n, sizeof(Row), row_cmp);
    for (k = 0; k < rows.n; k++)
        if (!strncmp(rows.r[k].path, "raw/", 4)) {
            raw_n++;
            if (!strcmp(rows.r[k].path, "raw/PSX.EXE")) have_exe = 1;
            if (omit && !strcmp(rows.r[k].path + 4, AS_CDDA_NAME)) {
                rc = fail(err, AS_E_MANIFEST, "invalid omitted_files record");
                goto out;
            }
        }
    if (raw_n != p->file_count - omit || !have_exe) {
        rc = fail(err, AS_E_MANIFEST, "incomplete raw filesystem inventory");
        goto out;
    }
    anoms = jget(root, "preserved_source_anomalies");
    {
        int want = p->preserved_subheader_lba >= 0;
        if (!anoms || anoms->type != J_ARR || anoms->n != want) {
            rc = fail(err, AS_E_MANIFEST, "missing or inconsistent source anomaly record");
            goto out;
        }
        if (want) {
            JNode *a = anoms->kids[0], *af = jget(a, "file");
            char rp[300];
            if (af && af->type == J_STR) snprintf(rp, sizeof rp, "raw/%s", af->s);
            if (a->type != J_OBJ || !af || af->type != J_STR || !find_row(&rows, rp) ||
                !jint_is(jget(a, "lba"), p->preserved_subheader_lba) ||
                !jstr_is(jget(a, "reason"), ANOMALY_REASON)) {
                rc = fail(err, AS_E_MANIFEST, "invalid source anomaly record");
                goto out;
            }
        }
    }
    expected_n = raw_n;
    for (k = 0; k < rows.n; k++) {
        char alias[300];
        const Row *a;
        if (strncmp(rows.r[k].path, "raw/", 4) || !alias_for(rows.r[k].path + 4, alias, sizeof alias)) continue;
        expected_n++;
        a = find_row(&rows, alias);
        if (!a || a->size != rows.r[k].size || strcmp(a->sha256, rows.r[k].sha256)) {
            rc = fail(err, AS_E_MANIFEST, "missing or inconsistent organized asset: %s", alias);
            goto out;
        }
    }
    if (expected_n != rows.n) { rc = fail(err, AS_E_MANIFEST, "unexpected managed files in asset manifest"); goto out; }
    if (ctl) {
        ctl->phase = AS_PH_VERIFY;
        if (!fast) { ctl->total = ctl->done + total; }
    }
    for (k = 0; k < rows.n; k++) {
        char path[PATH_N];
        if ((rc = checked_path(dest, rows.r[k].path, path, sizeof path, err))) goto out;
        set_path(err, path);
        if (pc_fs_stat(path, &st) || !st.is_reg || st.size != rows.r[k].size) {
            if (err) {
                snprintf(err->expected, sizeof err->expected, "%lld bytes", (long long)rows.r[k].size);
                if (st.exists) snprintf(err->found, sizeof err->found, "%lld bytes", (long long)st.size);
                else snprintf(err->found, sizeof err->found, "missing");
            }
            rc = fail(err, AS_E_CORRUPT, "missing or corrupt asset: %s", rows.r[k].path);
            goto out;
        }
        if (fast) continue;
        if ((rc = file_digest(path, 1, hex, ctl, err))) goto out;
        if (strcmp(hex, rows.r[k].sha256)) {
            if (err) {
                snprintf(err->expected, sizeof err->expected, "%s", rows.r[k].sha256);
                snprintf(err->found, sizeof err->found, "%s", hex);
            }
            rc = fail(err, AS_E_CORRUPT, "missing or corrupt asset: %s", rows.r[k].path);
            goto out;
        }
    }
    {
        char exe[PATH_N];
        join(exe, sizeof exe, dest, "raw/PSX.EXE");
        if ((rc = file_digest(exe, 0, hex, NULL, err))) goto out;
        if (strcmp(hex, p->exe_sha1)) {
            if (err) {
                snprintf(err->expected, sizeof err->expected, "%s", p->exe_sha1);
                snprintf(err->found, sizeof err->found, "%s", hex);
            }
            rc = fail(err, AS_E_CORRUPT, "cached executable differs from supported retail executable");
            goto out;
        }
    }
    if (ctl) {
        ctl->files = rows.n;
        ctl->anomalies = anoms->n;
        ctl->audio_omitted = omit;
    }
out:
    jfree(root);
    free(rows.r);
    return rc;
}

/* ------------------------------------------------------------------ */
/* extract_to_stage / prepare_assets                                   */
/* ------------------------------------------------------------------ */

typedef struct { FILE *f; PcSha256 c; int64_t n; AsCtl *ctl; } FileSink;

static int file_sink(void *ud, const unsigned char *p, size_t n) {
    FileSink *s = (FileSink *)ud;
    if (fwrite(p, 1, n, s->f) != n) return -1;
    pc_sha256_update(&s->c, p, n);
    s->n += (int64_t)n;
    progress(s->ctl, (int64_t)n);
    return 0;
}

static int parent_mkdirs(const char *path) {
    char dir[PATH_N];
    char *slash;
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = 0;
    return pc_fs_mkdirs(dir);
}

static int link_or_copy(const char *src, const char *dst) {
    if (parent_mkdirs(dst)) return -1;
    if (!getenv("TOKI_ASSET_NO_LINK") && pc_fs_link(src, dst) == 0) return 0;
    return pc_fs_copy(src, dst);
}

static int extract_to_stage(Disc *d, const char *stage, AsCtl *ctl, AsError *err) {
    Rows rows = {0};
    char *aliases = NULL;           /* folded alias names, for the collision check */
    int na = 0, i, rc = AS_OK;
    char mpath[PATH_N];
    aliases = (char *)calloc((size_t)d->n + 1, 300);
    if (!aliases) return fail(err, AS_E_WRITE, "out of memory");
    if (ctl) ctl->phase = AS_PH_EXTRACT;
    for (i = 0; i < d->n; i++) {
        Entry *e = &d->e[i];
        char name[300], target[PATH_N], alias[300], hex[65];
        FileSink s;
        uint8_t dg[32];
        if (e->omit) continue;
        snprintf(name, sizeof name, "raw/%s", e->name);
        join(target, sizeof target, stage, name);
        set_path(err, target);
        if (parent_mkdirs(target)) { rc = fail(err, AS_E_WRITE, "cannot create a folder for %s: %s", target, strerror(errno)); break; }
        s.f = pc_fs_create_excl(target);
        if (!s.f) { rc = fail(err, AS_E_WRITE, "cannot create %s: %s", target, strerror(errno)); break; }
        pc_sha256_init(&s.c);
        s.n = 0;
        s.ctl = ctl;
        rc = read_entry(d, e, file_sink, &s, ctl, err);
        if (fclose(s.f) && !rc) rc = fail(err, AS_E_WRITE, "cannot write %s: %s", target, strerror(errno));
        if (rc) break;
        if (s.n != output_size(e)) { rc = fail(err, AS_E_LAYOUT, "incomplete extracted file: %s", e->name); break; }
        pc_sha256_final(&s.c, dg);
        pc_hash_hex(dg, 32, hex);
        if (rows_add(&rows, name, s.n, hex)) { rc = fail(err, AS_E_WRITE, "out of memory"); break; }
        if (alias_for(e->name, alias, sizeof alias)) {
            char apath[PATH_N];
            int k;
            for (k = 0; k < na; k++)
                if (!icmp(aliases + k * 300, alias)) break;
            if (k < na) { rc = fail(err, AS_E_LAYOUT, "cannot flatten ambiguous asset basename: %s", alias); break; }
            snprintf(aliases + na * 300, 300, "%s", alias);
            na++;
            join(apath, sizeof apath, stage, alias);
            if (link_or_copy(target, apath)) { rc = fail(err, AS_E_WRITE, "cannot create %s: %s", apath, strerror(errno)); break; }
            if (rows_add(&rows, alias, s.n, hex)) { rc = fail(err, AS_E_WRITE, "out of memory"); break; }
        }
    }
    free(aliases);
    if (!rc) rc = disc_unchanged(d, err);
    if (!rc) {
        qsort(rows.r, (size_t)rows.n, sizeof(Row), row_cmp);
        join(mpath, sizeof mpath, stage, AS_MANIFEST);
        if (write_manifest(mpath, d->prof, &rows, d, d->audio_omitted))
            rc = fail(err, AS_E_WRITE, "cannot write %s: %s", mpath, strerror(errno));
    }
    free(rows.r);
    /* Verify actual copied output too, including the portable no-hard-link path. */
    if (!rc) {
        AsCtl local;
        int fast = 0;
        if (ctl) { fast = ctl->fast_verify; ctl->fast_verify = 0; }
        else { memset(&local, 0, sizeof local); ctl = &local; }
        rc = as_verify(stage, d->prof, ctl, err);
        ctl->fast_verify = fast;
    }
    return rc;
}

static int64_t needed_bytes(Disc *d) {
    int64_t n = 0;
    int i;
    char alias[300];
    for (i = 0; i < d->n; i++)
        if (!d->e[i].omit) n += output_size(&d->e[i]) * (alias_for(d->e[i].name, alias, sizeof alias) ? 2 : 1);
    return n;
}

static void split_dest(const char *dest, char *parent, size_t pn, char *name, size_t nn) {
    char tmp[PATH_N];
    char *slash;
    snprintf(tmp, sizeof tmp, "%s", dest);
    while (strlen(tmp) > 1 && (tmp[strlen(tmp) - 1] == '/' || tmp[strlen(tmp) - 1] == '\\'))
        tmp[strlen(tmp) - 1] = 0;
    slash = strrchr(tmp, '/');
#ifdef _WIN32
    {
        char *b = strrchr(tmp, '\\');
        if (b && (!slash || b > slash)) slash = b;
    }
#endif
    if (!slash) { snprintf(parent, pn, "."); snprintf(name, nn, "%s", tmp); return; }
    snprintf(name, nn, "%s", slash + 1);
    *slash = 0;
    snprintf(parent, pn, "%s", tmp[0] ? tmp : "/");
}

int as_owned_entry(const char *name) {
    return name[0] == '.' || !strcmp(name, "tokimeki-pc.log") ||
           !strcmp(name, "tokimeki-pc.log.old") || !strcmp(name, "en");
}

typedef struct { int other; char first[256]; } DestScan;

static int dest_scan_entry(const char *name, const PcStat *st, void *ud) {
    DestScan *d = (DestScan *)ud;
    (void)st;
    if (as_owned_entry(name)) return 0;
    if (!d->other++) snprintf(d->first, sizeof d->first, "%s", name);
    return 0;
}

int as_dest_status(const char *dest, char *other, size_t on) {
    char m[PATH_N];
    PcStat st;
    DestScan d;
    if (pc_fs_stat(dest, &st)) return AS_DEST_EMPTY;
    if (!st.is_dir || st.is_link) return AS_DEST_OTHER;
    join(m, sizeof m, dest, AS_MANIFEST);
    if (!pc_fs_stat(m, &st)) return AS_DEST_DATA;
    memset(&d, 0, sizeof d);
    pc_fs_listdir(dest, dest_scan_entry, &d);
    if (other && on) snprintf(other, on, "%s", d.first);
    return d.other ? AS_DEST_OTHER : AS_DEST_EMPTY;
}

/* Remove what a crashed run left: <parent>/.<name>.stage-* (only called
 * with the lock held, so no stage is in use). */
typedef struct { const char *parent; char prefix[600]; } StaleCtx;

static int stale_entry(const char *n, const PcStat *st, void *ud) {
    StaleCtx *c = (StaleCtx *)ud;
    char full[PATH_N];
    if (!st->is_dir || strncmp(n, c->prefix, strlen(c->prefix))) return 0;
    join(full, sizeof full, c->parent, n);
    pc_fs_rmtree(full);
    return 0;
}

/* The staged tree into an existing `dest` that holds no game data (only
 * the log, en/ ...): entry by entry, the manifest last, so `dest` reads as
 * game data only once everything is there.  On failure the entries moved
 * so far go back to the stage. */
typedef struct { const char *stage, *dest; char moved[64][128]; int n, failed; char why[400]; } MergeCtx;

static int merge_one(const char *n, const PcStat *st, void *ud) {
    MergeCtx *m = (MergeCtx *)ud;
    char a[PATH_N], b[PATH_N];
    (void)st;
    if (m->failed || !strcmp(n, AS_MANIFEST)) return 0;
    if (m->n >= 64 || strlen(n) >= sizeof m->moved[0]) {
        m->failed = 1;
        snprintf(m->why, sizeof m->why, "unexpected entry %s", n);
        return 1;
    }
    join(a, sizeof a, m->stage, n);
    join(b, sizeof b, m->dest, n);
    if (pc_fs_rename(a, b)) {
        m->failed = 1;
        snprintf(m->why, sizeof m->why, "%s: %s", b, strerror(errno));
        return 1;
    }
    snprintf(m->moved[m->n++], sizeof m->moved[0], "%s", n);
    return 0;
}

static int merge_stage(const char *stage, const char *dest, char *why, size_t wn) {
    MergeCtx *m = (MergeCtx *)calloc(1, sizeof *m);
    char a[PATH_N], b[PATH_N];
    int i, rc = 0;
    if (!m) { snprintf(why, wn, "out of memory"); return -1; }
    m->stage = stage;
    m->dest = dest;
    pc_fs_listdir(stage, merge_one, m);
    if (!m->failed) {
        join(a, sizeof a, stage, AS_MANIFEST);
        join(b, sizeof b, dest, AS_MANIFEST);
        if (pc_fs_rename(a, b)) {
            m->failed = 1;
            snprintf(m->why, sizeof m->why, "%s: %s", b, strerror(errno));
        }
    }
    if (m->failed) {
        for (i = 0; i < m->n; i++) {
            join(a, sizeof a, stage, m->moved[i]);
            join(b, sizeof b, dest, m->moved[i]);
            pc_fs_rename(b, a);
        }
        snprintf(why, wn, "%s", m->why);
        rc = -1;
    }
    free(m);
    return rc;
}

int as_prepare(const char *image, const char *audio_track, const char *dest_in,
               const AsProfile *profile, AsCtl *ctl, AsError *err) {
    const AsProfile *p = profile ? profile : &as_supported_profile;
    char dest[PATH_N], parent[PATH_N], name[512], lock[PATH_N], stage_root[PATH_N], stage[PATH_N];
    char other[256];
    PcStat st;
    PcFsLock lk;
    Disc d;
    int rc, merge, ds;
    AsCtl local;
    if (!ctl) { memset(&local, 0, sizeof local); ctl = &local; }
    if (pc_fs_absolute(dest_in, dest, sizeof dest)) snprintf(dest, sizeof dest, "%s", dest_in);
    ds = as_dest_status(dest, other, sizeof other);
    if (ds == AS_DEST_OTHER) {
        set_path(err, dest);
        return fail(err, AS_E_DEST_INVALID, "%s: invalid/unmanaged assets (it holds %s but no %s); move "
                    "this folder aside or choose a different game directory; existing files were NOT changed",
                    dest, other[0] ? other : "something else", AS_MANIFEST);
    }
    /* a folder with only the program's own files (the log, en/): no game
     * data yet, extracted into it entry by entry */
    merge = ds == AS_DEST_EMPTY && !pc_fs_stat(dest, &st);
    if (ds == AS_DEST_DATA) {
        int fast = ctl->fast_verify;
        ctl->fast_verify = 0;
        rc = as_verify(dest, p, ctl, err);
        ctl->fast_verify = fast;
        if (rc) {
            char why[768];
            snprintf(why, sizeof why, "%s", err ? err->message : "");
            if (err) set_path(err, dest);
            return fail(err, AS_E_DEST_INVALID, "%s: invalid/unmanaged assets (%s); move this folder aside or "
                        "choose a different game directory; existing files were NOT changed", dest, why);
        }
        if (image) {
            rc = as_check_image(image, audio_track, p, ctl, err);   /* a supplied image must pass the gate even on a cache hit */
            if (rc) return rc;
        }
        return AS_OK;
    }
    if (!image) return fail(err, AS_E_NOT_FOUND, "first run requires --disc /path/to/Track1.bin (or .cue); "
                            "you must own a legal copy of SLPS-00065");
    split_dest(dest, parent, sizeof parent, name, sizeof name);
    if (pc_fs_mkdirs(parent)) {
        set_path(err, parent);
        return fail(err, AS_E_WRITE, "cannot create %s: %s", parent, strerror(errno));
    }
    /* a locked file, removed when the setup ends; a crashed setup's lock is
     * released by the system and never blocks the next one */
    snprintf(lock, sizeof lock, "%s/.%s.prepare.lock", parent, name);
    rc = pc_fs_lock(lock, 0, &lk);
    if (rc) {
        set_path(err, lock);
        if (rc > 0)
            return fail(err, AS_E_LOCKED, "another setup is preparing %s right now (it holds %s)", dest, lock);
        return fail(err, AS_E_WRITE, "cannot create %s: %s", lock, strerror(errno));
    }
    {
        StaleCtx c;
        c.parent = parent;
        snprintf(c.prefix, sizeof c.prefix, ".%s.stage-", name);
        pc_fs_listdir(parent, stale_entry, &c);
    }
    rc = disc_open(&d, image, audio_track, p, ctl, err);
    if (!rc) {
        int64_t needed = needed_bytes(&d), free_b = pc_fs_free_space(parent);
        /* test hook (pc/tools/fresh_install.py): pretend the drive is this full */
        if (getenv("TOKI_TEST_FREE_BYTES")) free_b = strtoll(getenv("TOKI_TEST_FREE_BYTES"), NULL, 10);
        ctl->audio_omitted = d.audio_omitted;
        int64_t extract = 0;
        int k;
        for (k = 0; k < d.n; k++) if (!d.e[k].omit) extract += output_size(&d.e[k]);
        ctl->total = ctl->done + extract + needed;  /* extraction + its verification */
        if (needed > MAX_OUTPUT || (free_b >= 0 && free_b < needed + 16 * 1024 * 1024)) {
            if (err) {
                set_path(err, parent);
                snprintf(err->expected, sizeof err->expected, "%lld MB", (long long)((needed + 16 * 1024 * 1024) >> 20));
                snprintf(err->found, sizeof err->found, "%lld MB", (long long)(free_b >> 20));
            }
            rc = fail(err, AS_E_DISK_SPACE, "not enough free disk space for complete extraction and portable copies");
        }
        if (!rc) {
            int k;
            for (k = 0; k < 1000; k++) {
                snprintf(stage_root, sizeof stage_root, "%s/.%s.stage-%ld-%d", parent, name, as_getpid(), k);
                if (!pc_fs_mkdir(stage_root)) break;
                if (errno != EEXIST) k = 1000;
            }
            if (k >= 1000) {
                set_path(err, parent);
                rc = fail(err, AS_E_WRITE, "cannot create a staging folder in %s: %s", parent, strerror(errno));
            } else {
                join(stage, sizeof stage, stage_root, "assets");
                if (pc_fs_mkdir(stage)) {
                    rc = fail(err, AS_E_WRITE, "cannot create %s: %s", stage, strerror(errno));
                } else {
                    rc = extract_to_stage(&d, stage, ctl, err);
                    if (!rc) rc = disc_unchanged(&d, err);
                    if (!rc && (merge ? as_dest_status(dest, NULL, 0) != AS_DEST_EMPTY : !pc_fs_stat(dest, &st)))
                        rc = fail(err, AS_E_DEST_INVALID, "asset destination appeared during setup; refusing to replace it");
                    if (!rc && merge) {
                        char why[400];
                        if (merge_stage(stage, dest, why, sizeof why)) {
                            set_path(err, dest);
                            rc = fail(err, AS_E_WRITE, "cannot move the prepared assets into %s: %s", dest, why);
                        }
                    } else if (!rc && pc_fs_rename(stage, dest)) {
                        set_path(err, dest);
                        rc = fail(err, AS_E_WRITE, "cannot move the prepared assets to %s: %s", dest, strerror(errno));
                    }
                    if (!rc) {
                        ctl->anomalies = d.nanom;
                    }
                }
                pc_fs_rmtree(stage_root);
            }
        }
        disc_close(&d);
    }
    pc_fs_unlock(&lk);
    if (!rc) ctl->phase = AS_PH_DONE;
    return rc;
}

/* ------------------------------------------------------------------ */
/* candidates (no hashing)                                             */
/* ------------------------------------------------------------------ */

int as_quick_candidate(const char *path, const AsProfile *profile) {
    const AsProfile *p = profile ? profile : &as_supported_profile;
    PcStat st;
    Sources s;
    AsError e;
    if (pc_fs_stat(path, &st) || !(st.is_reg || st.is_link)) return 0;
    if (ieq(suffix_of(path), ".cue")) {
        if (st.size > 65536 || resolve_sources(path, NULL, &s, &e)) return 0;
        if (pc_fs_stat(s.track1, &st)) return 0;
        return (s.t1_len >= 0 ? s.t1_len : st.size) == p->track_size;
    }
    if (!ieq(suffix_of(path), ".bin") && !ieq(suffix_of(path), ".img")) return 0;
    return st.size == p->track_size;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

static int load_test_profile(const char *path, AsProfile *p) {
    FILE *f = pc_fs_fopen(path, "rb");
    char line[256];
    if (!f) return -1;
    memset(p, 0, sizeof *p);
    p->preserved_subheader_lba = -1;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v;
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
#define NUM(k) if (!strcmp(line, #k)) p->k = strtoll(v, NULL, 10);
#define STR(k) if (!strcmp(line, #k)) snprintf(p->k, sizeof p->k, "%s", v);
        NUM(track_size) STR(track_sha1) NUM(exe_lba) NUM(exe_size) STR(exe_sha1) NUM(file_count)
        NUM(audio_size) STR(audio_sha1) NUM(audio_skip) NUM(cdda_lba) NUM(cdda_size)
        NUM(preserved_subheader_lba)
#undef NUM
#undef STR
    }
    fclose(f);
    return 0;
}

int as_cli(int argc, char **argv) {
    const char *image = NULL, *out = NULL, *audio = NULL, *verify = NULL;
    AsProfile prof = as_supported_profile;
    AsCtl ctl;
    AsError err;
    int i, rc;
    memset(&ctl, 0, sizeof ctl);
    memset(&err, 0, sizeof err);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--prepare-assets") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(a, "--prepare-assets")) image = "";
        else if (!strcmp(a, "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--audio-track") && i + 1 < argc) audio = argv[++i];
        else if (!strcmp(a, "--verify-assets") && i + 1 < argc) verify = argv[++i];
        else if (!strcmp(a, "--fast")) ctl.fast_verify = 1;
        else if (!strcmp(a, "--allow-no-audio")) ctl.allow_no_audio = 1;
        else if (!strcmp(a, "--test-profile") && i + 1 < argc) {
            if (load_test_profile(argv[++i], &prof)) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        }
    }
    if (verify) {
        rc = as_verify(verify, &prof, &ctl, &err);
        if (rc) { printf("Asset verification failed: %s\n", err.message); return 1; }
        printf("Assets verified (%s): %s (%d managed files)\n", ctl.fast_verify ? "fast" : "full", verify, ctl.files);
        return 0;
    }
    if (!out) { fprintf(stderr, "--out is required\n"); return 2; }
    rc = as_prepare(image && *image ? image : NULL, audio, out, &prof, &ctl, &err);
    if (rc) { printf("Asset setup failed: %s\n", err.message); return 1; }
    printf("Assets verified: %s (%d managed files)\n", out, ctl.files);
    if (ctl.anomalies)
        printf("Warning: source XA subheader anomaly preserved byte-for-byte; see assets.manifest.json.\n");
    if (ctl.audio_omitted)
        printf("Warning: CD audio Track 2 was not supplied; %s was left out (the game never plays it).\n", AS_CDDA_NAME);
    return 0;
}
