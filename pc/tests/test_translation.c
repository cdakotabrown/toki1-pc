/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_translation.c -- the translation pack applier and the language layer
 * (pc/hal/translation.c, pc/hal/lang.c; docs/pc-port/TRANSLATION-ON-PC.md).
 * No assets: the packs are built here, byte by byte, in the format
 * pc/tools/translation_pack.py writes.  Each property has a negative
 * control showing the check can fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../hal/translation.h"
#include "../hal/lang.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #c); return __LINE__; } } while (0)

static unsigned char g_pack[4096];
static size_t g_n;

static void put(const void *p, size_t n) { memcpy(g_pack + g_n, p, n); g_n += n; }
static void u32le(unsigned v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; put(b, 4); }
static void u16le(unsigned v) { unsigned char b[2] = { v, v >> 8 }; put(b, 2); }

/* base "ABCDEFGHIJKLMNOP" -> out: SAME 4, COPY 8..12 (IJKL), MASK of two
 * nibbles over "IJ", FILL 2 x '!', SAME the rest from 12. */
static const unsigned char BASE[16] = "ABCDEFGHIJKLMNOP";

static void build_pack(const unsigned char *base_sha, const unsigned char *out, size_t out_n) {
    unsigned char h[20];
    static const unsigned char mask[2] = { 0x0F, 0xF0 }, val[2] = { 0x0A, 0x60 };
    g_n = 0;
    put("TKTP", 4); u32le(1); u32le(1);
    u16le(9); put("DIR/F.BIN", 9);
    u32le(16); put(base_sha, 20);
    translation_sha1(out, out_n, h);
    u32le((unsigned)out_n); put(h, 20);
    u32le(5);
    put("\x01", 1); u32le(4);
    put("\x02", 1); u32le(8); u32le(4);
    put("\x03", 1); u32le(2); put(mask, 2); put(val, 2);
    put("\x04", 1); u32le(2); put("!", 1);
    put("\x01", 1); u32le(4);
}

/* the result the ops above describe */
static void expected(unsigned char out[16]) {
    memcpy(out, "ABCD", 4);
    memcpy(out + 4, "IJKL", 4);
    out[8] = (unsigned char)(('I' & 0xF0) | 0x0A);      /* low nibble ours */
    out[9] = (unsigned char)(('J' & 0x0F) | 0x60);      /* high nibble ours */
    out[10] = '!'; out[11] = '!';
    memcpy(out + 12, "MNOP", 4);
}

int test_translation_sha1(void) {
    unsigned char h[20];
    static const unsigned char abc[20] = { 0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
                                           0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
    static const unsigned char empty[20] = { 0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,
                                             0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09 };
    char big[200];
    translation_sha1("abc", 3, h);
    CHECK(!memcmp(h, abc, 20));
    translation_sha1("", 0, h);
    CHECK(!memcmp(h, empty, 20));
    /* negative control: one changed byte changes the digest */
    translation_sha1("abd", 3, h);
    CHECK(memcmp(h, abc, 20) != 0);
    /* lengths across the 55/56/64 padding boundaries do not crash or repeat */
    memset(big, 'x', sizeof big);
    {
        unsigned char a[20], b[20];
        translation_sha1(big, 55, a); translation_sha1(big, 56, b); CHECK(memcmp(a, b, 20));
        translation_sha1(big, 64, a); translation_sha1(big, 119, b); CHECK(memcmp(a, b, 20));
    }
    return 0;
}

int test_translation_apply(void) {
    unsigned char sha[20], want[16], *out;
    size_t n = 0;
    translation_sha1(BASE, 16, sha);
    expected(want);
    build_pack(sha, want, 16);
    out = translation_apply_file(g_pack, g_n, "DIR/F.BIN", BASE, 16, &n);
    CHECK(out != NULL);
    CHECK(n == 16 && !memcmp(out, want, 16));
    free(out);
    /* negative control: a base that is not the retail file is refused */
    {
        unsigned char other[16];
        memcpy(other, BASE, 16);
        other[15] ^= 1;
        CHECK(translation_apply_file(g_pack, g_n, "DIR/F.BIN", other, 16, &n) == NULL);
    }
    /* negative control: a result hash that does not match is refused */
    {
        unsigned char wrong[16];
        memcpy(wrong, want, 16);
        wrong[0] ^= 1;
        build_pack(sha, wrong, 16);
        CHECK(translation_apply_file(g_pack, g_n, "DIR/F.BIN", BASE, 16, &n) == NULL);
    }
    /* a truncated pack is refused, not read past its end */
    build_pack(sha, want, 16);
    CHECK(translation_apply_file(g_pack, g_n - 3, "DIR/F.BIN", BASE, 16, &n) == NULL);
    return 0;
}

static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(data, 1, n, f);
    return fclose(f);
}

int test_translation_prepare(void) {
    char root[64] = "/tmp/toki_tktp_XXXXXX", assets[128], en[128], p[256];
    unsigned char sha[20], want[16], got[16];
    TranslationResult r;
    FILE *f;
    CHECK(mkdtemp(root) != NULL);
    /* the data folder and its en/ (2026-11-07) */
    snprintf(assets, sizeof assets, "%s/data", root);
    snprintf(en, sizeof en, "%s/data/en", root);
    snprintf(p, sizeof p, "%s/raw", assets); mkdir(assets, 0755); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/raw/DIR", assets); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", assets);
    CHECK(write_file(p, BASE, 16) == 0);
    translation_sha1(BASE, 16, sha);
    expected(want);
    build_pack(sha, want, 16);
    CHECK(translation_prepare_pack(g_pack, g_n, assets, en, &r) == 0);
    CHECK(r.files == 1 && r.written == 1);
    snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", en);
    f = fopen(p, "rb");
    CHECK(f != NULL);
    CHECK(fread(got, 1, 16, f) == 16);
    fclose(f);
    CHECK(!memcmp(got, want, 16));
    /* the preparation lock is gone afterwards */
    snprintf(p, sizeof p, "%s/.prepare.lock", en);
    CHECK(access(p, F_OK) != 0);
    /* the Japanese file is untouched */
    snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", assets);
    f = fopen(p, "rb"); CHECK(f && fread(got, 1, 16, f) == 16); fclose(f);
    CHECK(!memcmp(got, BASE, 16));
    /* a second run finds it up to date */
    CHECK(translation_prepare_pack(g_pack, g_n, assets, en, &r) == 0);
    CHECK(r.up_to_date == 1 && r.written == 0);
    /* negative control: a modified Japanese file is not the supported disc */
    snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", en); remove(p);
    {
        unsigned char other[16];
        memcpy(other, BASE, 16); other[3] = 'z';
        snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", assets);
        CHECK(write_file(p, other, 16) == 0);
    }
    CHECK(translation_prepare_pack(g_pack, g_n, assets, en, &r) != 0);
    CHECK(r.bad_base == 1 && r.written == 0);
    snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", en);
    CHECK(access(p, F_OK) != 0);
    /* clean up */
    snprintf(p, sizeof p, "%s/raw/DIR/F.BIN", assets); remove(p);
    snprintf(p, sizeof p, "%s/raw/DIR", assets); rmdir(p);
    snprintf(p, sizeof p, "%s/raw", assets); rmdir(p);
    snprintf(p, sizeof p, "%s/raw/DIR", en); rmdir(p);
    snprintf(p, sizeof p, "%s/raw", en); rmdir(p);
    CHECK(rmdir(en) == 0);              /* nothing else left in en/ */
    rmdir(assets);
    rmdir(root);
    return 0;
}

int test_lang_japanese_is_noop(void) {
    char buf[256], root[256];
    const char *in = "./pc/assets/raw/CDROM/SERIFU/X.BIN";
    /* Japanese (the default): the path is returned unchanged, as the same
     * pointer, and the pointer is all the HAL call sites look at */
    g_hal_lang_en = 0;
    CHECK(hal_lang_path(in, buf, sizeof buf) == in);
    CHECK(hal_lang_glyph_x(0, 0, 3, 42) == 42);
    {
        unsigned char bits[18] = { 0 };
        CHECK(hal_lang_glyph_code(bits) == 0);
    }
    /* the developer layout and any other dir: the "-en" sibling */
    hal_lang_en_root("pc/assets", root, sizeof root);
    CHECK(!strcmp(root, "pc/assets-en"));
    hal_lang_en_root("/x/assets/", root, sizeof root);
    CHECK(!strcmp(root, "/x/assets-en"));
    /* the player's data folder: data/en; an old assets-en beside it only
     * while it exists and data/en does not */
    {
        char top[64] = "/tmp/toki_enroot_XXXXXX", d[160], want[200];
        CHECK(mkdtemp(top) != NULL);
        snprintf(d, sizeof d, "%s/data", top);
        mkdir(d, 0755);
        hal_lang_en_root(d, root, sizeof root);
        snprintf(want, sizeof want, "%s/data/en", top);
        CHECK(!strcmp(root, want));
        snprintf(d, sizeof d, "%s/data/", top);
        hal_lang_en_root(d, root, sizeof root);
        CHECK(!strcmp(root, want));
        snprintf(d, sizeof d, "%s/assets-en", top);
        mkdir(d, 0755);
        snprintf(d, sizeof d, "%s/data", top);
        hal_lang_en_root(d, root, sizeof root);
        snprintf(want, sizeof want, "%s/assets-en", top);
        CHECK(!strcmp(root, want));
        snprintf(d, sizeof d, "%s/data/en", top);
        mkdir(d, 0755);
        snprintf(d, sizeof d, "%s/data", top);
        hal_lang_en_root(d, root, sizeof root);
        snprintf(want, sizeof want, "%s/data/en", top);
        CHECK(!strcmp(root, want));
        snprintf(d, sizeof d, "%s/data/en", top); rmdir(d);
        snprintf(d, sizeof d, "%s/data", top); rmdir(d);
        snprintf(d, sizeof d, "%s/assets-en", top); rmdir(d);
        rmdir(top);
    }
    /* negative control: with English on and no English copy of the file,
     * the path is still the Japanese one (a missing file never breaks) */
    g_hal_lang_en = 1;
    CHECK(hal_lang_path(in, buf, sizeof buf) == in);
    g_hal_lang_en = 0;
    return 0;
}

#include "pc_settings.h"

static const char *lang_test_harness_env(const char *name) {
    return strcmp(name, "PC_INPUT") == 0 ? "script.txt" : NULL;
}

int test_lang_setting(void) {
    PcSettings s;
    char v[16];
    pc_settings_defaults(&s);
    CHECK(s.language == 1);                                   /* en by default */
    CHECK(pc_settings_format(&s, "game.language", v, sizeof v) == 0 && !strcmp(v, "en"));
    pc_settings_parse(&s, "[game]\nlanguage = ja\n", "test");
    CHECK(s.language == 0);
    CHECK(pc_settings_format(&s, "game.language", v, sizeof v) == 0 && !strcmp(v, "ja"));
    /* negative control: an unknown language keeps the value it had */
    pc_settings_parse(&s, "[game]\nlanguage = fr\n", "test");
    CHECK(s.language == 0);
    CHECK(pc_settings_set(&s, "game.language", "en") == 0 && s.language == 1);
    /* an automated run ignores the file and plays the original Japanese */
    pc_settings_load(&s, lang_test_harness_env, 0);
    CHECK(s.harness == 1 && s.language == 0);
    return 0;
}
