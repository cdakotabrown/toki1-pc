/* pc_hash.c -- SHA-1 and SHA-256 (FIPS 180-4).  See pc_hash.h.
 * Written for this port; public domain (CC0). */
#include "pc_hash.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- SHA-1 ---- */

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void pc_sha1_init(PcSha1 *c) {
    static const uint32_t iv[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

void pc_sha1_update(PcSha1 *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    if (c->n) {
        size_t take = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n < 64) return;
        sha1_block(c->h, c->buf);
        c->n = 0;
    }
    for (; len >= 64; p += 64, len -= 64) sha1_block(c->h, p);
    memcpy(c->buf, p, len);
    c->n = len;
}

void pc_sha1_final(PcSha1 *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenbuf[8];
    int i;
    pc_sha1_update(c, &pad, 1);
    while (c->n != 56) pc_sha1_update(c, &zero, 1);
    for (i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    pc_sha1_update(c, lenbuf, 8);
    for (i = 0; i < 5; i++) put_be32(out + 4 * i, c->h[i]);
}

/* ---- SHA-256 ---- */

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], s[8], t1, t2;
    int i;
    for (i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (i = 0; i < 64; i++) {
        uint32_t e = s[4], a = s[0];
        t1 = s[7] + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & s[5]) ^ (~e & s[6])) + K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & s[1]) ^ (a & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
        s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) h[i] += s[i];
}

void pc_sha256_init(PcSha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

void pc_sha256_update(PcSha256 *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    if (c->n) {
        size_t take = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n < 64) return;
        sha256_block(c->h, c->buf);
        c->n = 0;
    }
    for (; len >= 64; p += 64, len -= 64) sha256_block(c->h, p);
    memcpy(c->buf, p, len);
    c->n = len;
}

void pc_sha256_final(PcSha256 *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenbuf[8];
    int i;
    pc_sha256_update(c, &pad, 1);
    while (c->n != 56) pc_sha256_update(c, &zero, 1);
    for (i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    pc_sha256_update(c, lenbuf, 8);
    for (i = 0; i < 8; i++) put_be32(out + 4 * i, c->h[i]);
}

void pc_hash_hex(const uint8_t *digest, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[2 * n] = 0;
}
