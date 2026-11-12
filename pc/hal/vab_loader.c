/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vab_loader.c -- PS1 VAB soundbank loader.
 *
 * Parses VH + VB pairs, uploads ADPCM samples to SPU RAM, and exposes a
 * program/note lookup for the SEQ driver.
 *
 * Reference implementations:
 *   - PSY-Q 4.7 libsnd (SsVabOpenHeadWithMode; upstream PSY-Z mirrors it)
 *   - PSX-SPX VAB format notes
 *   - pc/tools/vab2sf2.py (Python reference already in-tree)
 *
 * VH layout (VAB version >= 5, which is the Tokimeki Memorial case: ver=6):
 *   [0x00]  VabHdr (32 bytes)
 *           int  form    : 'pBAV' in memory order ('VABp' when read as int LE)
 *           int  ver
 *           int  id
 *           u32  fsize
 *           u16  reserved0
 *           u16  ps        : count of programs with tones (<=128 for ver>=5)
 *           u16  ts        : total tones in bank
 *           u16  vs        : number of VAGs (samples)
 *           u8   mvol, pan, attr1, attr2
 *           u32  reserved1
 *   [0x20]  Program table: 128 × 16 bytes (kMaxPrograms = 128 for ver>=5).
 *           Each ProgAtr: tones, mvol, prior, mode, mpan, reserved0,
 *                         s16 attr, u32 reserved1, u32 reserved2.
 *           Only entries with tones>0 own a tone row.
 *   [ + ]   Tone table: (num_progs_with_tones) × 16 × 32 bytes.
 *           Rows are packed in program-index order for programs where tones>0.
 *   [ + ]   VAG offset table: 256 × 2 bytes. Entry 0 is unused (zero). Entries
 *           1..vs give per-VAG size in 8-byte units (ver>=5) or 4-byte units
 *           (ver<5). Actual VAG byte offsets in VB are cumulative.
 *
 * VB layout: flat concatenation of raw ADPCM streams. Each stream is a run of
 * 16-byte blocks (2-byte header + 14 bytes of 4-bit-nibble samples). Upload
 * straight to SPU RAM; the SPU decodes ADPCM natively.
 */

#include "vab_loader.h"
#include "spu_psx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VAB_MAGIC_PBAV   0x56414270u   /* 'pBAV' as little-endian u32 */
#define VAB_MAX_PROGRAMS 128
#define VAB_TONES_PER_PROGRAM 16
#define VAB_TONE_STRIDE  32
#define VAB_PROG_STRIDE  16
#define VAB_HEADER_SIZE  32
#define VAB_VAG_TABLE_ENTRIES 256

/* Packed program attribute. 16 bytes on disk. */
typedef struct {
    u8  tones;
    u8  mvol;
    u8  prior;
    u8  mode;
    u8  mpan;
    u8  reserved0;
    s16 attr;
    u32 reserved1;
    u32 reserved2;
} VabProgAtr;

struct VabBank {
    /* Parsed header */
    u32 version;
    u32 vab_id;
    u16 num_programs;   /* count of programs with tones (header ps) */
    u16 num_tones;      /* total tones (header ts) */
    u16 num_vags;       /* header vs */
    u8  master_vol;
    u8  master_pan;

    /* Base SPU address samples were uploaded to. */
    u32 spu_base;

    /* Per-program metadata, indexed 0..127. */
    struct {
        u8  tone_count;   /* 0 if program has no tones */
        u8  mvol;
        u8  prior;
        u8  mode;
        u8  mpan;
        /* Offset in tone_array[] of this program's first tone, or 0xFFFF. */
        u16 tone_start;
    } programs[VAB_MAX_PROGRAMS];

    /* Flat tone array. One entry per non-blank tone (vag_idx != 0). */
    VabTone *tones;
    u32 tone_count;

    /* VAG addresses in SPU RAM (1-indexed: vag_addr[0] unused). */
    u32 vag_addr[VAB_VAG_TABLE_ENTRIES];
    u32 vag_size[VAB_VAG_TABLE_ENTRIES];
};

/* ------------------------------------------------------------------------- */

static u8 *read_file(const char *path, u32 *out_size)
{
    FILE *fp;
    long size;
    u8 *buf;
    size_t got;

    fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (u8 *)malloc((size_t)size);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    if (out_size) {
        *out_size = (u32)size;
    }
    return buf;
}

/* Little-endian scalar reads -- VH is always LE. */
static u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Look at a single VAG's ADPCM stream (already in VB buffer at [off, off+size))
 * and find the byte offset of the first block with the loop-start flag (bit 2
 * of block flags byte). Returns offset into the VAG, or 0 if no explicit loop.
 * PSX SPU loop addresses are at 16-byte block granularity. */
static u32 find_loop_offset(const u8 *vag_data, u32 vag_size)
{
    u32 i;
    for (i = 0; i + 16 <= vag_size; i += 16) {
        u8 flags = vag_data[i + 1];
        if (flags & 0x04) {
            return i;
        }
    }
    return 0;
}

VabBank *vab_load(const char *vh_path, const char *vb_path, u32 spu_base_addr)
{
    u8 *vh = NULL;
    u8 *vb = NULL;
    u32 vh_size = 0;
    u32 vb_size = 0;
    VabBank *bank = NULL;
    u32 prog_table_off;
    u32 tone_table_off;
    u32 vag_table_off;
    u32 tone_rows;        /* number of 16-tone rows packed in the VH */
    u32 tone_capacity;
    u32 tone_write;
    u32 i;
    u32 vag_cursor;
    u32 multiplier;

    vh = read_file(vh_path, &vh_size);
    if (!vh) {
        fprintf(stderr, "vab_load: cannot open VH %s\n", vh_path);
        goto fail;
    }
    vb = read_file(vb_path, &vb_size);
    if (!vb) {
        fprintf(stderr, "vab_load: cannot open VB %s\n", vb_path);
        goto fail;
    }

    if (vh_size < VAB_HEADER_SIZE) {
        fprintf(stderr, "vab_load: VH too small (%u bytes)\n", vh_size);
        goto fail;
    }
    if (rd_u32(vh + 0) != VAB_MAGIC_PBAV) {
        fprintf(stderr, "vab_load: bad magic in %s\n", vh_path);
        goto fail;
    }

    bank = (VabBank *)calloc(1, sizeof(*bank));
    if (!bank) {
        goto fail;
    }

    bank->version      = rd_u32(vh + 0x04);
    bank->vab_id       = rd_u32(vh + 0x08);
    /* 0x0C fsize, 0x10 reserved0 (u16), 0x12 ps (u16) */
    bank->num_programs = rd_u16(vh + 0x12);
    bank->num_tones    = rd_u16(vh + 0x14);
    bank->num_vags     = rd_u16(vh + 0x16);
    bank->master_vol   = vh[0x18];
    bank->master_pan   = vh[0x19];
    bank->spu_base     = spu_base_addr;

    /* For ver >= 5 the program table is always 128 entries. Earlier bank
     * versions use 64, but every VAB in this game is ver 6. */
    if (bank->version < 5) {
        fprintf(stderr, "vab_load: legacy VAB version %u not supported\n",
                bank->version);
        goto fail;
    }

    prog_table_off = VAB_HEADER_SIZE;
    tone_table_off = prog_table_off + VAB_MAX_PROGRAMS * VAB_PROG_STRIDE;

    /* Initialize program table and count tone rows we expect. */
    tone_rows = 0;
    for (i = 0; i < VAB_MAX_PROGRAMS; i++) {
        const u8 *p = vh + prog_table_off + i * VAB_PROG_STRIDE;
        u8 tones = p[0];
        bank->programs[i].tone_count = tones;
        bank->programs[i].mvol       = p[1];
        bank->programs[i].prior      = p[2];
        bank->programs[i].mode       = p[3];
        bank->programs[i].mpan       = p[4];
        bank->programs[i].tone_start = 0xFFFFu;
        if (tones != 0) {
            tone_rows++;
        }
    }

    /* Sanity: VH must be large enough to hold tone rows + VAG table. */
    vag_table_off = tone_table_off + tone_rows * VAB_TONES_PER_PROGRAM * VAB_TONE_STRIDE;
    if (vag_table_off + VAB_VAG_TABLE_ENTRIES * 2 > vh_size) {
        fprintf(stderr,
                "vab_load: VH short: need %u bytes for %u tone rows + vag tbl, have %u\n",
                vag_table_off + VAB_VAG_TABLE_ENTRIES * 2, tone_rows, vh_size);
        goto fail;
    }

    /* Reserve a tone slot per-program up front: maximum is tone_rows * 16. */
    tone_capacity = tone_rows * VAB_TONES_PER_PROGRAM;
    bank->tones = (VabTone *)calloc(tone_capacity > 0 ? tone_capacity : 1,
                                    sizeof(VabTone));
    if (!bank->tones) {
        goto fail;
    }

    /* Walk program-by-program, reading the correct tone row. Match libsnd's
     * packing order: rows appear for programs with tones>0, in program-index
     * order. Blank tones (vag_idx==0) are skipped but still reserve a slot. */
    tone_write = 0;
    {
        u32 row = 0;
        u32 p;
        for (p = 0; p < VAB_MAX_PROGRAMS; p++) {
            u32 count = bank->programs[p].tone_count;
            u32 row_base;
            u32 t;
            if (count == 0) {
                continue;
            }
            row_base = tone_table_off + row * VAB_TONES_PER_PROGRAM * VAB_TONE_STRIDE;
            bank->programs[p].tone_start = (u16)tone_write;

            /* Iterate all 16 tone slots of this row. libsnd only gives logical
             * meaning to the first `count`; others are padding with vag==0. */
            for (t = 0; t < VAB_TONES_PER_PROGRAM; t++) {
                const u8 *tp = vh + row_base + t * VAB_TONE_STRIDE;
                u16 adsr1, adsr2, vag_idx;
                VabTone *out;

                /* Layout per PSX-SPX / libsnd VagAtr:
                 *   0x00 prior,mode,vol,pan, 0x04 center,shift,min,max,
                 *   0x08 vibW,vibT,porW,porT, 0x0C pbmin,pbmax,rsv,rsv,
                 *   0x10 adsr1(u16), 0x12 adsr2(u16),
                 *   0x14 parent_prog(s16), 0x16 vag_idx(s16, 1-based),
                 *   0x18..0x1F: 4 × s16 reserved. */
                vag_idx = rd_u16(tp + 0x16); /* VAG id, 1-based */
                if (vag_idx == 0) {
                    continue; /* blank slot -- no sample assigned */
                }

                adsr1 = rd_u16(tp + 0x10);
                adsr2 = rd_u16(tp + 0x12);

                out = &bank->tones[tone_write++];
                out->priority    = tp[0x00];
                /* tp[0x01] = mode (reverb on/off) -- not needed by our player */
                out->vol         = tp[0x02];
                out->pan         = tp[0x03];
                out->center_note = tp[0x04];
                out->pitch_fine  = (s8)tp[0x05]; /* signed cents/128 */
                out->min_note    = tp[0x06];
                out->max_note    = tp[0x07];
                out->adsr1       = adsr1;
                out->adsr2       = adsr2;
                /* tp[0x14..0x15] holds the parent program index. We trust
                 * our iterator instead (it matches packing order). */
                out->program     = (u8)p;
                /* VAG index saved temporarily in spu_addr slot; resolved below
                 * once we've parsed the VAG offset table. */
                out->spu_addr      = vag_idx;
                out->spu_loop_addr = 0;
            }
            row++;
        }
    }

    bank->tone_count = tone_write;

    /* -------- VAG offset table -------- */
    /* ver>=5: each entry is size in 8-byte units; ver<5 uses 4-byte units.
     * Entry 0 is reserved (always 0). Entries 1..num_vags carry real sizes. */
    multiplier = (bank->version >= 5) ? 8u : 4u;
    vag_cursor = 0; /* offset from start of VB for next VAG */

    memset(bank->vag_addr, 0, sizeof(bank->vag_addr));
    memset(bank->vag_size, 0, sizeof(bank->vag_size));

    for (i = 0; i < VAB_VAG_TABLE_ENTRIES; i++) {
        u16 sz_units = rd_u16(vh + vag_table_off + i * 2);
        u32 sz_bytes = (u32)sz_units * multiplier;
        if (i == 0) {
            /* Reserved entry. */
            continue;
        }
        if (i > bank->num_vags) {
            continue;
        }
        if (vag_cursor + sz_bytes > vb_size) {
            fprintf(stderr,
                    "vab_load: VAG %u out of bounds (cursor=%u size=%u vb=%u)\n",
                    i, vag_cursor, sz_bytes, vb_size);
            goto fail;
        }
        /* Upload raw ADPCM to SPU RAM. */
        if (sz_bytes > 0) {
            spu_psx_dma_write(vb + vag_cursor,
                              spu_base_addr + vag_cursor,
                              sz_bytes);
        }
        bank->vag_addr[i] = spu_base_addr + vag_cursor;
        bank->vag_size[i] = sz_bytes;
        vag_cursor += sz_bytes;
    }

    /* Resolve tone vag_idx placeholders to actual SPU addresses. */
    for (i = 0; i < bank->tone_count; i++) {
        VabTone *t = &bank->tones[i];
        u32 idx = t->spu_addr; /* placeholder vag index */
        u32 base, loop;
        if (idx == 0 || idx >= VAB_VAG_TABLE_ENTRIES || bank->vag_size[idx] == 0) {
            /* Dangling reference -- mark with zero address; the caller can
             * check by noting spu_addr == spu_base_addr is atypical. */
            t->spu_addr = 0;
            t->spu_loop_addr = 0;
            continue;
        }
        base = bank->vag_addr[idx];
        loop = find_loop_offset(vb + (base - spu_base_addr), bank->vag_size[idx]);
        t->spu_addr      = base;
        t->spu_loop_addr = base + loop;
    }

    free(vh);
    free(vb);
    return bank;

fail:
    free(vh);
    free(vb);
    if (bank) {
        free(bank->tones);
        free(bank);
    }
    return NULL;
}

void vab_free(VabBank *bank)
{
    if (!bank) {
        return;
    }
    free(bank->tones);
    free(bank);
}

int vab_find_tone(const VabBank *bank, u8 program, u8 note, VabTone *out)
{
    u32 count;
    u32 start;
    u32 i;

    if (!bank || program >= VAB_MAX_PROGRAMS) {
        return 0;
    }
    count = bank->programs[program].tone_count;
    if (count == 0) {
        return 0;
    }
    start = bank->programs[program].tone_start;
    if (start == 0xFFFFu) {
        return 0;
    }

    /* tone_count is the number of non-blank tones the program declared;
     * they were written into bank->tones[] contiguously starting at
     * `start`. Scan for a tone whose [min,max] contains `note`. */
    for (i = 0; i < count; i++) {
        if (start + i >= bank->tone_count) {
            break;
        }
        {
            const VabTone *t = &bank->tones[start + i];
            if (t->program != program) {
                /* Reached the next program's tones -- we're done. */
                break;
            }
            if (note >= t->min_note && note <= t->max_note) {
                if (out) {
                    *out = *t;
                }
                return 1;
            }
        }
    }
    return 0;
}

void vab_dump(const VabBank *bank)
{
    u32 i;
    u32 active_programs;
    u32 total_vag_bytes;

    if (!bank) {
        printf("vab_dump: NULL bank\n");
        return;
    }

    active_programs = 0;
    for (i = 0; i < VAB_MAX_PROGRAMS; i++) {
        if (bank->programs[i].tone_count != 0) {
            active_programs++;
        }
    }
    total_vag_bytes = 0;
    for (i = 0; i < VAB_VAG_TABLE_ENTRIES; i++) {
        total_vag_bytes += bank->vag_size[i];
    }

    printf("VAB bank @ %p:\n", (const void *)bank);
    printf("  version       : %u\n", bank->version);
    printf("  vab_id        : %u\n", bank->vab_id);
    printf("  header ps     : %u (active programs: %u)\n",
           bank->num_programs, active_programs);
    printf("  header ts     : %u (tones stored: %u)\n",
           bank->num_tones, bank->tone_count);
    printf("  header vs     : %u\n", bank->num_vags);
    printf("  master vol/pan: %u / %u\n", bank->master_vol, bank->master_pan);
    printf("  SPU base      : 0x%05X\n", bank->spu_base);
    printf("  SPU usage     : %u bytes\n", total_vag_bytes);
    printf("  per-program tone counts:\n");
    for (i = 0; i < VAB_MAX_PROGRAMS; i++) {
        if (bank->programs[i].tone_count != 0) {
            printf("    prog %3u: %u tones (start=%u, mvol=%u, mpan=%u, prior=%u)\n",
                   i,
                   bank->programs[i].tone_count,
                   bank->programs[i].tone_start,
                   bank->programs[i].mvol,
                   bank->programs[i].mpan,
                   bank->programs[i].prior);
        }
    }
}
