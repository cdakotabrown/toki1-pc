/* SPDX-License-Identifier: GPL-3.0-or-later */
/* translation.h -- the embedded English translation pack and its applier.
 *
 * The pack (format below) is built by
 * pc/tools/translation_pack.py from the repo's English patch build and is
 * compiled into the executable.  It carries only bytes this project
 * authored -- English text, our font, our lettering pixels, our compiled
 * patch module, pointer words -- plus COPY / SAME instructions that take
 * every other byte from the player's own extracted Japanese file.  It is
 * applied on the player's machine:
 *
 *     translation_prepare("<dir>/data", "<dir>/data/en")
 *
 * reads each base file under <assets>/raw/, checks its SHA-1 against the
 * pack (the retail file the recipe was made for), applies the recipe,
 * checks the result's SHA-1 and writes <en>/raw/<path>.  Only the
 * changed files are written; pc/hal/lang.c serves them in front of the
 * Japanese tree when [game] language = en.
 *
 * Pack format (little endian):
 *   "TKTP" u32 version(1) u32 nfiles
 *   per file: u16 pathlen, path (relative to raw/, '/' separated)
 *             u32 base_size, u8 base_sha1[20]   (base_size 0: a new file)
 *             u32 out_size,  u8 out_sha1[20]
 *             u32 nops, then ops, each advancing the output position:
 *               1 SAME u32 len               base[pos..pos+len)
 *               2 COPY u32 src, u32 len      base[src..src+len)
 *               3 MASK u32 len, mask[len], val[len]
 *                    out = (base[pos] & ~mask) | (val & mask); val has no
 *                    bit outside mask, so an unchanged retail nibble is
 *                    never stored
 *               4 FILL u32 len, u8 value
 */
#ifndef PC_TRANSLATION_H
#define PC_TRANSLATION_H

#include <stddef.h>

#define TKTP_SAME 1
#define TKTP_COPY 2
#define TKTP_MASK 3
#define TKTP_FILL 4

/* The pack compiled into this executable (pc/build/translation_pack.S),
 * or NULL/0 in a build made without one. */
const unsigned char *translation_pack_data(size_t *size);

/* SHA-1 (also used by the tests). */
void translation_sha1(const void *data, size_t n, unsigned char out[20]);

/* Results of translation_prepare. */
typedef struct {
    int files;          /* files in the pack */
    int written;        /* files written this call */
    int up_to_date;     /* files already present with the right hash */
    int missing_base;   /* base file absent */
    int bad_base;       /* base file present, wrong hash (not the supported disc) */
    int failed;         /* recipe or write failure */
    char first_error[256];
} TranslationResult;

/* Apply `pack` (NULL: the embedded one) to `assets_root`/raw -> `out_root`/raw.
 * Returns 0 when every file of the pack is in place afterwards.  Safe across
 * processes: it holds `out_root`/.prepare.lock for the whole run (a second
 * process waits, then finds the files up to date) and writes each file
 * through a temporary of its own (2026-11-02). */
int translation_prepare_pack(const unsigned char *pack, size_t pack_size,
                             const char *assets_root, const char *out_root,
                             TranslationResult *res);
int translation_prepare(const char *assets_root, const char *out_root);

/* 1 when the embedded pack makes raw/<rel>, 0 when it does not, -1 when
 * this build has no pack (pc/hal/lang.c hal_lang_path). */
int translation_pack_lists(const char *rel);
/* the same for a given pack (no cache; the tests) */
int translation_pack_lists_in(const unsigned char *pack, size_t size, const char *rel);

/* Apply one file's recipe in memory (tests).  Returns a malloc'd buffer of
 * *out_size bytes, or NULL (bad base hash / malformed). */
unsigned char *translation_apply_file(const unsigned char *pack, size_t pack_size,
                                      const char *path, const unsigned char *base,
                                      size_t base_size, size_t *out_size);

#endif
