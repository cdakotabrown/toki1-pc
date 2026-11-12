/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/hal/mdec_native.c -- BS bitstream decoder and MDEC model.
 *
 * Turns a BS bitstream (a still picture or one movie frame) into the
 * pixels the console's MDEC produces, in two stages:
 *
 *   1. The VLC pass libpress runs on the CPU: Huffman codes -> the
 *      16-bit run-level halfwords the MDEC consumes.  The code tables
 *      are built at first use from the code lists in psx-spx, "CDROM
 *      File Video BS Compression DC Values" / "... AC Values" (v1/v2
 *      raw 10-bit DC, v3 size-coded DC differences).
 *   2. The MDEC itself (psx-spx "MDEC Decompression"): dequantization,
 *      a two-pass 8x8 inverse DCT against the scale matrix libpress
 *      uploads, then the YCbCr -> RGB step and the 15-bit rounding.
 *      The scale matrix and quantizer tables are computed from their
 *      definitions (the DCT-III basis, the MPEG-1 default intra matrix)
 *      and match the tables in the retail executable.
 *
 * Written from psx-spx and the textbook definitions; the fixed-point
 * details psx-spx leaves open were measured on the reference's output
 * (DuckStation's: 24-bit movie frames and 15-bit stills dumped from its
 * VRAM) and are checked by pc/tools/test_mdec_retail.py.  "The reference"
 * below means that emulator.
 *
 * Credit: the fixed-point MDEC model (dequantization precision, IDCT
 * rounding, the YCbCr coefficients and green-channel truncation) follows
 * the MDEC research in DuckStation, (C) Connor McLaughlin, GPL-3.0.
 */
#include "types.h"
#include "mdec_native.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ bit reader
 * The bitstream is a sequence of little-endian 16-bit words; each word is
 * read most significant bit first.  Only whole words that lie inside the
 * buffer are available: a code that needs a bit past the end is an error,
 * never satisfied by zero padding. */
typedef struct {
    const u8 *buf;
    u32 nbits;          /* bits available (whole words only) */
    u32 pos;            /* next bit */
} Bits;

static void bits_init(Bits *b, const u8 *data, u32 size)
{
    b->buf = data;
    b->nbits = size >= 8 ? ((size - 8) / 2) * 16 : 0;
    b->pos = 0;
}

static u32 bit_at(const Bits *b, u32 i)
{
    u32 word = i >> 4, k = i & 15;
    u32 v = (u32)b->buf[8 + word * 2] | ((u32)b->buf[8 + word * 2 + 1] << 8);
    return (v >> (15 - k)) & 1;
}

/* the next n bits (n <= 24) left-aligned in an n-bit value; missing bits
 * read as 0.  *avail gets how many of them really exist. */
static u32 bits_peek(const Bits *b, u32 n, u32 *avail)
{
    u32 v = 0, i, have = b->nbits > b->pos ? b->nbits - b->pos : 0;
    for (i = 0; i < n; i++)
        v = (v << 1) | (i < have ? bit_at(b, b->pos + i) : 0);
    *avail = have < n ? have : n;
    return v;
}

static int bits_take(Bits *b, u32 n, u32 *out)
{
    u32 avail, v = bits_peek(b, n, &avail);
    if (avail < n) return -1;
    b->pos += n;
    *out = v;
    return 0;
}

/* ------------------------------------------------------------ AC codes
 * psx-spx "Huffman codes for AC values BS v1/v2/v3/iki": each group is a
 * prefix, some index bits selecting one of the listed MDEC halfwords
 * (run << 10 | level), then a sign bit negating the level. */
typedef struct {
    const char *prefix;
    u32 index_bits;
    u16 value[16];
} AcGroup;

static const AcGroup AC_GROUPS[] = {
    { "11", 0, { 0x0001 } },
    { "011", 0, { 0x0401 } },
    { "010", 1, { 0x0002, 0x0801 } },
    { "0011", 1, { 0x1001, 0x0C01 } },
    { "00101", 0, { 0x0003 } },
    { "00100", 3, { 0x3401, 0x0006, 0x3001, 0x2C01, 0x0C02, 0x0403, 0x0005,
                    0x2801 } },
    { "0001", 2, { 0x1C01, 0x1801, 0x0402, 0x1401 } },
    { "00001", 2, { 0x0802, 0x2401, 0x0004, 0x2001 } },
    { "0000001", 3, { 0x4001, 0x1402, 0x0007, 0x0803, 0x0404, 0x3C01,
                      0x3801, 0x1002 } },
    { "00000001", 4, { 0x000B, 0x2002, 0x1003, 0x000A, 0x0804, 0x1C02,
                       0x5401, 0x5001, 0x0009, 0x4C01, 0x4801, 0x0405,
                       0x0C03, 0x0008, 0x1802, 0x4401 } },
    { "000000001", 4, { 0x2802, 0x2402, 0x1403, 0x0C04, 0x0805, 0x0407,
                        0x0406, 0x000F, 0x000E, 0x000D, 0x000C, 0x6801,
                        0x6401, 0x6001, 0x5C01, 0x5801 } },
    { "0000000001", 4, { 0x001F, 0x001E, 0x001D, 0x001C, 0x001B, 0x001A,
                         0x0019, 0x0018, 0x0017, 0x0016, 0x0015, 0x0014,
                         0x0013, 0x0012, 0x0011, 0x0010 } },
    { "00000000001", 4, { 0x0028, 0x0027, 0x0026, 0x0025, 0x0024, 0x0023,
                          0x0022, 0x0021, 0x0020, 0x040E, 0x040D, 0x040C,
                          0x040B, 0x040A, 0x0409, 0x0408 } },
    { "000000000001", 4, { 0x0412, 0x0411, 0x0410, 0x040F, 0x1803, 0x4002,
                           0x3C02, 0x3802, 0x3402, 0x3002, 0x2C02, 0x7C01,
                           0x7801, 0x7401, 0x7001, 0x6C01 } },
};

#define AC_LOOKAHEAD 18            /* AC_ZEROS_MAX + AC_REST_BITS */
#define AC_ZEROS_MAX 12            /* 12 leading zeros: no code */
#define AC_REST_BITS 6             /* bits after the first 1, at most */
#define AC_EOB       1             /* "10" */
#define AC_ESCAPE    2             /* "000001" + 16 raw bits */
#define AC_CODE      3

/* Every code is some zeros, a 1, then at most AC_REST_BITS bits.  The
 * table is indexed by the zero count and the (zero-padded) bits after the
 * 1: the code's halfword, its whole length in bits, and its kind (0 for
 * no code).  Plain values, no pointers: it may sit in a save state. */
typedef struct { u16 value; u8 length; u8 kind; } AcEntry;
static AcEntry g_ac[AC_ZEROS_MAX][1 << AC_REST_BITS];
static int g_ac_ready;

static u32 prefix_bits(const char *s, u32 *len)
{
    u32 v = 0, n = 0;
    for (; *s; s++, n++) v = (v << 1) | (u32)(*s == '1');
    *len = n;
    return v;
}

/* enter the `len`-bit code `code` (leading bit first) */
static void ac_fill(u32 code, u32 len, AcEntry e)
{
    u32 zeros = 0, rest_len, rest, shift, i;
    while (zeros < len && !((code >> (len - 1 - zeros)) & 1)) zeros++;
    rest_len = len - zeros - 1;
    rest = code & ((1u << rest_len) - 1);
    shift = AC_REST_BITS - rest_len;
    for (i = 0; i < (1u << shift); i++) g_ac[zeros][(rest << shift) | i] = e;
}

static void ac_tables(void)
{
    u32 g, idx, sign, len, code;
    AcEntry e;
    if (g_ac_ready) return;
    memset(g_ac, 0, sizeof g_ac);
    for (g = 0; g < sizeof AC_GROUPS / sizeof AC_GROUPS[0]; g++) {
        const AcGroup *grp = &AC_GROUPS[g];
        u32 plen, pre = prefix_bits(grp->prefix, &plen);
        for (idx = 0; idx < (1u << grp->index_bits); idx++) {
            for (sign = 0; sign < 2; sign++) {
                u16 v = grp->value[idx];
                if (sign) /* negate the 10-bit level, keep the run */
                    v = (u16)((v & 0xFC00) | ((0x400 - (v & 0x3FF)) & 0x3FF));
                len = plen + grp->index_bits + 1;
                code = (((pre << grp->index_bits) | idx) << 1) | sign;
                e.value = v; e.length = (u8)len; e.kind = AC_CODE;
                ac_fill(code, len, e);
            }
        }
    }
    e.value = 0xFE00; e.length = 2; e.kind = AC_EOB;
    ac_fill(2, 2, e);                        /* 10 */
    e.value = 0; e.length = 6; e.kind = AC_ESCAPE;
    ac_fill(1, 6, e);                        /* 000001 */
    g_ac_ready = 1;
}

/* One AC code -> its run-level halfword (0xFE00 for end of block). */
static int ac_next(Bits *b, u16 *out)
{
    u32 avail, look = bits_peek(b, AC_LOOKAHEAD, &avail), raw, zeros = 0;
    AcEntry e;
    while (zeros < AC_ZEROS_MAX && !((look >> (AC_LOOKAHEAD - 1 - zeros)) & 1)) zeros++;
    if (zeros == AC_ZEROS_MAX) return -1;
    e = g_ac[zeros][(look >> (AC_LOOKAHEAD - 1 - zeros - AC_REST_BITS)) & ((1u << AC_REST_BITS) - 1)];
    if (!e.kind || e.length > avail) return -1;
    b->pos += e.length;
    if (e.kind == AC_ESCAPE) {
        if (bits_take(b, 16, &raw)) return -1;
        *out = (u16)raw;
        return 0;
    }
    *out = e.value;
    return 0;
}

/* ------------------------------------------------------------ DC codes
 * v1/v2: a raw signed 10-bit DC; +1FFh in a Cr position ends the frame.
 * v3: psx-spx "DC v3": a size code (one table for Cr/Cb, one for Y),
 * then `size` bits: a value with its top bit clear is negative
 * (bits - (2^size - 1)).  The difference, in units of 4, is added to a
 * predictor kept per component (Cr, Cb, and one for Y1..Y4), reset at the
 * start of each picture; 1111111111 ends the frame.  The retail size
 * tables (indexed by the next 8 bits) hold a zero-length, size-0 entry
 * for the unused all-ones prefixes, so such a prefix consumes nothing
 * and leaves the predictor unchanged; that is reproduced here. */
static const char *const DC_SIZE_CHROMA[9] = {
    "00", "01", "10", "110", "1110", "11110", "111110", "1111110",
    "11111110"
};
static const char *const DC_SIZE_LUMA[9] = {
    "100", "00", "01", "101", "110", "1110", "11110", "111110", "1111110"
};

typedef struct {
    s32 version;
    s32 pred[3];        /* Cr, Cb, Y */
} DcState;

/* size from the next 8 bits; -1 when no code matches (zero-length) */
static int dc_size(u32 next8, int chroma, u32 *len)
{
    const char *const *t = chroma ? DC_SIZE_CHROMA : DC_SIZE_LUMA;
    int s;
    for (s = 0; s < 9; s++) {
        u32 l, c = prefix_bits(t[s], &l);
        if ((next8 >> (8 - l)) == c) { *len = l; return s; }
    }
    *len = 0;
    return -1;
}

/* the DC halfword's low 10 bits for block `comp` (0 Cr, 1 Cb, 2 Y) */
static int dc_next(Bits *b, DcState *dc, int comp, u32 *field)
{
    u32 v, len, avail, next8, diffbits;
    int size;
    s32 diff;
    if (dc->version != 3) {
        if (bits_take(b, 10, &v)) return -1;
        *field = v;
        return 0;
    }
    next8 = bits_peek(b, 8, &avail);
    size = dc_size(next8, comp < 2, &len);
    if (size < 0) {                     /* retail's zero-length entry */
        *field = ((u32)dc->pred[comp] << 2) & 0x3FF;
        return 0;
    }
    if (len > avail) return -1;
    b->pos += len;
    diff = 0;
    if (size) {
        if (bits_take(b, (u32)size, &diffbits)) return -1;
        diff = (s32)diffbits;
        if (!(diffbits >> (size - 1))) diff -= (1 << size) - 1;
    }
    dc->pred[comp] += diff;
    *field = ((u32)dc->pred[comp] << 2) & 0x3FF;
    return 0;
}

/* Is the frame's end code next (checked where a macroblock's Cr starts)? */
static int frame_end(const Bits *b, const DcState *dc)
{
    u32 avail, v;
    if (dc->version != 3) return 0;      /* v1/v2: see decode_block */
    v = bits_peek(b, 10, &avail);
    return avail == 10 && v == 0x3FF;
}

/* ------------------------------------------------------------ tables
 * Zigzag scan (psx-spx "zigzag"): position in the scan -> raster index.
 * Generated by walking the anti-diagonals of the 8x8 block. */
static u8 g_zigzag[64];
/* libpress's quantizer table (both luminance and chrominance): the
 * MPEG-1 default intra quantizer matrix (ISO/IEC 11172-2), in scan
 * order, with the DC entry replaced by 2. */
static const u8 MPEG1_INTRA[64] = {          /* raster order */
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};
static u8 g_quant[64];                       /* scan order */
/* The IDCT scale matrix libpress uploads (psx-spx MDEC(3)): row u,
 * column x = 2^15 * c(u) * cos((2x + 1) u pi / 16), c(0) = 1/sqrt(2),
 * c(u) = 1 otherwise, rounded down: the DCT-III basis at 15 fraction
 * bits (5A82h, 7D8Ah, ...; equal to the table in the retail executable). */
static s32 g_scale[64];
static int g_tables_ready;
#define MDEC_PI 3.14159265358979323846

static void build_tables(void)
{
    int i, d, x, y, u, n = 0;
    if (g_tables_ready) return;
    for (d = 0; d < 15; d++) {             /* anti-diagonal x + y = d */
        for (i = 0; i <= d; i++) {
            x = (d & 1) ? d - i : i;
            y = d - x;
            if (x < 8 && y < 8) g_zigzag[n++] = (u8)(y * 8 + x);
        }
    }
    for (i = 0; i < 64; i++) g_quant[i] = MPEG1_INTRA[g_zigzag[i]];
    g_quant[0] = 2;
    for (u = 0; u < 8; u++) {
        for (x = 0; x < 8; x++) {
            double c = u ? 1.0 : sqrt(0.5);
            double v = 32768.0 * c * cos((2 * x + 1) * u * MDEC_PI / 16.0);
            g_scale[u * 8 + x] = (s32)floor(v);
        }
    }
    g_tables_ready = 1;
}

/* ------------------------------------------------------------ MDEC
 * psx-spx "MDEC Decompression": rl_decode_block, the two-pass IDCT
 * against the scale matrix, yuv_to_rgb.  psx-spx leaves the exact fixed
 * point open; the precision and rounding below were measured against the
 * reference's output (24-bit movie frames and 15-bit stills, compared
 * exactly; see pc/tools/test_mdec_retail.py).
 *
 * Coefficients carry 4 fraction bits.  An AC value is psx-spx's
 * level * quant * qscale / 8 with the division an arithmetic shift, then
 * moved half a unit toward zero; the DC is dc * quant[0], also moved half
 * a unit toward zero.  Each IDCT pass is a matrix product with the scale
 * matrix (2^15-scaled DCT-III basis, row = frequency), rounded to nearest
 * at 2^18 and saturated to 16 bits; the result wraps to signed 9 bits and
 * saturates to signed 8 (psx-spx y_to_mono). */
static s32 sext10(u32 v) { v &= 0x3FF; return (v & 0x200) ? (s32)v - 0x400 : (s32)v; }

static s32 clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

static s32 sgn(s32 v) { return (v > 0) - (v < 0); }

#define COEF_FRAC 4

/* v8 / 8 (floor) with COEF_FRAC fraction bits, half a unit toward 0 */
static s32 dequant(s32 v8)
{
    s32 whole = clamp(v8 >> 3, -0x400, 0x3FF);
    return whole * (1 << COEF_FRAC) - sgn(v8) * (1 << (COEF_FRAC - 1));
}

static s32 idct_round(s64 sum)
{
    s64 v = (sum + ((s64)1 << 17)) >> 18;
    return (s32)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void idct(const s32 *coef, s32 *out)
{
    s32 tmp[64];
    int x, y, z;
    for (x = 0; x < 8; x++) {               /* columns: vertical frequencies */
        for (y = 0; y < 8; y++) {
            s64 sum = 0;
            for (z = 0; z < 8; z++)
                sum += (s64)coef[z * 8 + x] * g_scale[z * 8 + y];
            tmp[y * 8 + x] = idct_round(sum);
        }
    }
    for (y = 0; y < 8; y++) {               /* rows: horizontal frequencies */
        for (x = 0; x < 8; x++) {
            s64 sum = 0;
            for (z = 0; z < 8; z++)
                sum += (s64)tmp[y * 8 + z] * g_scale[z * 8 + x];
            out[y * 8 + x] = idct_round(sum);
        }
    }
}

typedef struct {
    Bits bits;
    DcState dc;
    u32 qscale;
    s32 halfwords;      /* run-level halfwords produced so far */
} Decoder;

/* Decode one block into 64 IDCT output samples; 1 at the frame's end
 * (only possible when check_end), 0 ok, negative on a bad stream. */
static int decode_block(Decoder *d, int comp, int check_end, s32 *samples)
{
    s32 coef[64];
    u32 dcfield, q;
    u16 hw;
    int k;
    if (check_end && frame_end(&d->bits, &d->dc)) return 1;
    if (dc_next(&d->bits, &d->dc, comp, &dcfield)) return -2;
    if (d->dc.version != 3 && check_end && dcfield == 0x1FF) return 1;
    d->halfwords++;
    memset(coef, 0, sizeof coef);
    q = d->qscale & 63;
    /* q = 0 is psx-spx's mode without a quantizer table: value * 2, stored
     * in scan position order rather than through the zigzag */
    if (q) coef[0] = dequant(sext10(dcfield) * g_quant[0] * 8);
    else coef[0] = clamp(sext10(dcfield) * 2, -0x400, 0x3FF) * (1 << COEF_FRAC);
    k = 0;
    for (;;) {
        if (ac_next(&d->bits, &hw)) return -3;
        d->halfwords++;
        if (hw == 0xFE00) break;
        k += (hw >> 10) + 1;
        if (k > 63) return -4;
        if (q) coef[g_zigzag[k]] = dequant(sext10(hw) * g_quant[k] * (s32)q);
        else coef[k] = clamp(sext10(hw) * 2, -0x400, 0x3FF) * (1 << COEF_FRAC);
    }
    idct(coef, samples);
    return 0;
}

/* A sample as the colour stage sees it: wrapped to signed 9 bits, then
 * saturated to signed 8 bits (psx-spx y_to_mono). */
static s32 sample8(s32 v)
{
    v &= 0x1FF;
    if (v & 0x100) v -= 0x200;
    return clamp(v, -128, 127);
}

/* YCbCr -> one 8-bit unsigned channel triple: psx-spx yuv_to_rgb, the
 * ITU-R BT.601 conversion with the MDEC's coefficients (R = Y + 1.402 Cr,
 * G = Y - 0.3437 Cb - 0.7143 Cr, B = Y + 1.772 Cb) at 8 fraction bits,
 * rounded; each channel saturated to signed 8 bits and offset by 128.
 * Measured on the reference: the two green products drop their low 5
 * (Cb) and 3 (Cr) bits before the sum. */
static void ycc_to_rgb(s32 y, s32 cb, s32 cr, u8 *rgb)
{
    s32 r = (359 * cr + 128) >> 8;
    s32 g = (((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 128) >> 8;
    s32 bl = (454 * cb + 128) >> 8;
    rgb[0] = (u8)(clamp(y + r, -128, 127) + 128);
    rgb[1] = (u8)(clamp(y + g, -128, 127) + 128);
    rgb[2] = (u8)(clamp(y + bl, -128, 127) + 128);
}

static u32 to5(u32 c) { c = (c + 4) >> 3; return c > 31 ? 31 : c; }

/* One macroblock (Cr, Cb, Y1..Y4) -> 16x16 RGB triples. */
static int decode_macroblock(Decoder *d, u8 *rgb /* 16*16*3 */)
{
    s32 cr[64], cb[64], yb[4][64];
    int r, i, x, y;
    r = decode_block(d, 0, 1, cr);
    if (r) return r;
    if ((r = decode_block(d, 1, 0, cb)) != 0) return r;
    for (i = 0; i < 4; i++)
        if ((r = decode_block(d, 2, 0, yb[i])) != 0) return r;
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            int blk = (y >> 3) * 2 + (x >> 3);
            int c = (y >> 1) * 8 + (x >> 1);
            ycc_to_rgb(sample8(yb[blk][(y & 7) * 8 + (x & 7)]),
                       sample8(cb[c]), sample8(cr[c]),
                       rgb + (y * 16 + x) * 3);
        }
    }
    return 0;
}

static int decoder_open(Decoder *d, const u8 *bs, u32 size)
{
    u32 version;
    if (!bs || size < 8 || bs[2] != 0x00 || bs[3] != 0x38) return -1;
    version = (u32)bs[6] | ((u32)bs[7] << 8);
    if (version < 1 || version > 3) return -1;
    ac_tables();
    build_tables();
    memset(d, 0, sizeof *d);
    bits_init(&d->bits, bs, size);
    d->dc.version = (s32)version;
    d->qscale = ((u32)bs[4] | ((u32)bs[5] << 8)) & 63;
    return 0;
}

/* ------------------------------------------------------------ pictures
 * Macroblocks are stored column by column (psx-spx "MDEC Data Format":
 * vertically arranged), top to bottom inside a 16-pixel column. */
static s32 decode_picture(const u8 *bs, u32 bs_size, u32 width, u32 height,
                          u16 *out555, u8 *out24, s32 white_to_transparent)
{
    Decoder d;
    u8 rgb[16 * 16 * 3];
    u32 mx, my, x, y;
    if (decoder_open(&d, bs, bs_size)) return -1;
    for (mx = 0; mx < (width + 15) / 16; mx++) {
        for (my = 0; my < (height + 15) / 16; my++) {
            int r = decode_macroblock(&d, rgb);
            if (r > 0) return 0;                 /* the frame ended early */
            if (r < 0) return r;
            for (y = 0; y < 16 && my * 16 + y < height; y++) {
                for (x = 0; x < 16 && mx * 16 + x < width; x++) {
                    const u8 *p = rgb + (y * 16 + x) * 3;
                    size_t o = (size_t)(my * 16 + y) * width + mx * 16 + x;
                    if (out24) {
                        memcpy(out24 + o * 3, p, 3);
                    } else {
                        u16 v = (u16)(to5(p[0]) | to5(p[1]) << 5 | to5(p[2]) << 10);
                        if (white_to_transparent && p[0] >= 248 && p[1] >= 248 &&
                            p[2] >= 248)
                            v = 0;
                        out555[o] = v;
                    }
                }
            }
        }
    }
    return 0;
}

s32 mdec_decode_bs_native(const u8 *bs, u32 bs_size, u32 width, u32 height,
                          u16 *out_psx555, s32 white_to_transparent)
{
    return decode_picture(bs, bs_size, width, height, out_psx555, NULL,
                          white_to_transparent);
}

s32 mdec_decode_bs_native_rgb24(const u8 *bs, u32 bs_size, u32 width,
                                u32 height, u8 *out_rgb24)
{
    return decode_picture(bs, bs_size, width, height, NULL, out_rgb24, 0);
}

/* ------------------------------------------------------------ streams */
struct MdecBsStream {
    Decoder d;
    int done;
    u8 *copy;
};

MdecBsStream *mdec_bs_stream_open(const u8 *bs, u32 bs_size)
{
    MdecBsStream *s = (MdecBsStream *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->copy = (u8 *)malloc(bs_size ? bs_size : 1);
    if (!s->copy) { free(s); return NULL; }
    if (bs_size) memcpy(s->copy, bs, bs_size);
    if (decoder_open(&s->d, s->copy, bs_size)) {
        free(s->copy);
        free(s);
        return NULL;
    }
    return s;
}

s32 mdec_bs_stream_macroblock(MdecBsStream *s, u16 *out_psx555, u32 stride)
{
    u8 rgb[16 * 16 * 3];
    u32 x, y;
    int r;
    if (!s || s->done) return -1;
    r = decode_macroblock(&s->d, rgb);
    if (r) { s->done = 1; return -1; }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            const u8 *p = rgb + (y * 16 + x) * 3;
            out_psx555[y * stride + x] =
                (u16)(to5(p[0]) | to5(p[1]) << 5 | to5(p[2]) << 10);
        }
    return 0;
}

void mdec_bs_stream_close(MdecBsStream *s)
{
    if (!s) return;
    free(s->copy);
    free(s);
}

/* ------------------------------------------------------------ VLC pass
 * What libpress's CPU pass (MDEC_vlc) consumes and produces: it reads the
 * bitstream until it has produced the header's count of run-level data
 * (halfword 0, in 32-bit words) or reaches the frame's end code.  Only
 * the code structure matters here, not the pixel arithmetic. */
u32 mdec_bs_vlc_extent(const u8 *bs, u32 bs_size, u32 *halfwords)
{
    Decoder d;
    u32 limit;
    s32 dummy[64];
    int comp = 0, block = 0;
    if (halfwords) *halfwords = 0;
    if (decoder_open(&d, bs, bs_size)) return 0;
    limit = ((u32)bs[0] | ((u32)bs[1] << 8)) * 2u;
    while ((u32)d.halfwords < limit) {
        int r;
        comp = block < 2 ? block : 2;
        r = decode_block(&d, comp, block == 0, dummy);
        if (r) break;
        block = (block + 1) % 6;
    }
    if (halfwords) *halfwords = (u32)d.halfwords < limit ? (u32)d.halfwords : limit;
    /* the header plus the whole bytes the codes (end code included) span:
     * 19,329 bytes / 26,448 halfwords for NAME_SAK.BS, the still the
     * libpress cost fit was measured on */
    return 8 + d.bits.pos / 8;
}

/* Kept for the old VLC test harness's link line: no standalone VLC
 * output is produced. */
s32 mdec_vlc_decode(const u8 *bs, u32 bs_size, u16 *out_rle, u32 out_max)
{
    (void)bs; (void)bs_size; (void)out_rle; (void)out_max;
    return -1;
}
